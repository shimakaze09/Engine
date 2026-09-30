// Verifies that the cooked-asset checks remember every asset of a project
// larger than a sample: with 2,000 cooked paths, far past the first
// table's 512 slots, each stale asset still warns exactly once, and each
// generation verdict is computed once and read from the cache after. Four
// threads check the same paths at once, as the streaming worker and the
// main thread do, so the tables grow under contention. The verdict count
// is the once-per-computation "no cook stamp" notice an unstamped asset
// logs.

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "../test_harness.h"
#include "engine/content/asset_staleness.h"
#include "engine/core/logging.h"

namespace {

engine::tests::TestContext g_tests;

constexpr int kAssets = 2000;
constexpr int kThreads = 4;

std::atomic<int> g_staleWarnings{0};
std::atomic<int> g_verdictsComputed{0};

void count_sink(engine::core::LogLevel level, const char *channel,
                const char *message, void *) noexcept {
  if ((channel == nullptr) || (std::strcmp(channel, "assets") != 0) ||
      (message == nullptr)) {
    return;
  }
  if ((level == engine::core::LogLevel::Warning) &&
      (std::strncmp(message, "stale cooked asset", 18) == 0)) {
    g_staleWarnings.fetch_add(1, std::memory_order_relaxed);
  }
  if (std::strncmp(message, "no cook stamp", 13) == 0) {
    g_verdictsComputed.fetch_add(1, std::memory_order_relaxed);
  }
}

void cooked_path(int index, char (&out)[256]) noexcept {
  std::snprintf(out, sizeof(out), "asset_check_capacity/a%04d.mesh", index);
}

/// A cookmeta per asset recording a source hash the shared source no
/// longer has, so every asset is stale.
bool write_fixtures() noexcept {
  std::error_code error{};
  std::filesystem::remove_all("asset_check_capacity", error);
  std::filesystem::create_directories("asset_check_capacity", error);
  if (error) {
    return false;
  }
  {
    std::ofstream source("asset_check_capacity/source.gltf", std::ios::binary);
    source << "changed";
    if (!source.good()) {
      return false;
    }
  }
  for (int i = 0; i < kAssets; ++i) {
    char path[256] = {};
    cooked_path(i, path);
    char metaPath[300] = {};
    std::snprintf(metaPath, sizeof(metaPath), "%s.cookmeta", path);
    std::ofstream meta(metaPath, std::ios::binary);
    meta << "{\"source\":\"asset_check_capacity/source.gltf\","
            "\"sourceContentHash\":\"0000000000000001\"}";
    if (!meta.good()) {
      return false;
    }
  }
  return true;
}

/// Every thread checks every path twice, each starting at its own offset
/// so they race to claim the same slots.
void check_all(int offset) noexcept {
  for (int pass = 0; pass < 2; ++pass) {
    for (int n = 0; n < kAssets; ++n) {
      char path[256] = {};
      cooked_path((n + offset) % kAssets, path);
      engine::content::warn_if_cooked_asset_stale(path);
      static_cast<void>(engine::content::cooked_asset_generation_ok(path));
    }
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!write_fixtures()) {
    g_tests.fail("write the cooked fixtures");
    return g_tests.finish("asset check capacity");
  }
  engine::content::reset_cooked_asset_stale_warnings();
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_sink, nullptr)) {
    g_tests.fail("register the log sink");
    return g_tests.finish("asset check capacity");
  }

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(check_all, t * (kAssets / kThreads));
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "each of %d stale assets warns exactly once (%d warnings)",
                kAssets, g_staleWarnings.load());
  g_tests.check(g_staleWarnings.load() == kAssets, message);
  // An in-flight verdict another thread is computing is recomputed, not
  // waited for, so racing threads may compute one twice; a verdict never
  // cached is computed on every one of the 2 x 4 checks.
  std::snprintf(message, sizeof(message),
                "each verdict is cached after it is computed (%d computed "
                "for %d assets)",
                g_verdictsComputed.load(), kAssets);
  g_tests.check((g_verdictsComputed.load() >= kAssets) &&
                    (g_verdictsComputed.load() <= kAssets * kThreads),
                message);

  // A single thread, after the tables have grown: every path is cached.
  const int computedBefore = g_verdictsComputed.load();
  const int warnedBefore = g_staleWarnings.load();
  check_all(0);
  g_tests.check((g_verdictsComputed.load() == computedBefore) &&
                    (g_staleWarnings.load() == warnedBefore),
                "a later pass over every asset computes and warns nothing");

  // The reset forgets them all: the next pass checks each afresh.
  engine::content::reset_cooked_asset_stale_warnings();
  g_staleWarnings.store(0);
  g_verdictsComputed.store(0);
  check_all(0);
  g_tests.check((g_staleWarnings.load() == kAssets) &&
                    (g_verdictsComputed.load() == kAssets),
                "after a reset each asset is checked once again");

  engine::core::log_unregister_sink(&count_sink, nullptr);
  engine::core::shutdown_logging();
  engine::content::reset_cooked_asset_stale_warnings();
  std::error_code error{};
  std::filesystem::remove_all("asset_check_capacity", error);
  return g_tests.finish("asset check capacity");
}
