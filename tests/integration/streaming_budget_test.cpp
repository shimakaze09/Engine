// P1-M4-C3c: Test — queue more than budget allows, verify spreading across
// frames.

#include "engine/core/cvar.h"
#include "engine/renderer/asset_streaming.h"


#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace engine::renderer;

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

static AssetId make_id(std::uint64_t n) noexcept {
  return static_cast<AssetId>(n + 1U);
}

// Each asset reports 64 MB — so with a 256 MB budget, at most 4 can load per
// frame.
static constexpr std::uint64_t kAssetSize = 64ULL * 1024ULL * 1024ULL;

static bool big_load(AssetId /*id*/, const char * /*path*/,
                     std::uint64_t *outSizeBytes,
                     void * /*userData*/) noexcept {
  if (outSizeBytes != nullptr) {
    *outSizeBytes = kAssetSize;
  }
  return true;
}

static bool big_upload(AssetId /*id*/, void * /*userData*/) noexcept {
  return true;
}

// --- Budget enforcement: 20 loads × 64 MB each, 256 MB budget → spreads
//     across multiple frames. ---
static void test_budget_spreading() noexcept {
  engine::core::initialize_cvars();

  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  // Budget = 256 MB, max uploads = 1024 (no upload bottleneck).
  engine::core::cvar_set_int("asset.streaming_budget_mb", 256);
  engine::core::cvar_set_int("asset.max_uploads_per_frame", 1024);

  constexpr std::size_t kLoadCount = 20U;
  LoadHandle handles[kLoadCount]{};

  for (std::size_t i = 0U; i < kLoadCount; ++i) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "big/asset_%02zu.mesh", i);
    handles[i] =
        load_asset_async(queue.get(), make_id(i), path, LoadPriority::Normal);
    CHECK(handles[i].valid(), "handle valid");
  }

  // Process frames and count how many are required.
  std::size_t frameCount = 0U;
  std::size_t readyPerFrame[32]{};
  constexpr std::size_t kMaxFrames = 32U;

  while (frameCount < kMaxFrames) {
    begin_streaming_frame(queue.get());
    const std::size_t ready =
        update_asset_streaming(queue.get(), &big_load, &big_upload, nullptr);
    readyPerFrame[frameCount] = ready;
    ++frameCount;

    bool allReady = true;
    for (std::size_t i = 0U; i < kLoadCount; ++i) {
      if (!is_load_ready(queue.get(), handles[i])) {
        allReady = false;
        break;
      }
    }
    if (allReady) {
      break;
    }
  }

  // With 20 × 64 MB and 256 MB budget, should take at least 5 frames
  // (4 per frame × 5 frames = 20).
  CHECK(frameCount >= 5U, "takes multiple frames due to budget");

  // Verify all loaded.
  std::size_t totalReady = 0U;
  for (std::size_t i = 0U; i < kLoadCount; ++i) {
    if (is_load_ready(queue.get(), handles[i])) {
      ++totalReady;
    }
  }
  CHECK(totalReady == kLoadCount, "all 20 eventually loaded");

  // No single frame should have processed all 20 (budget prevents it).
  bool anyFrameWithAll = false;
  for (std::size_t f = 0U; f < frameCount; ++f) {
    if (readyPerFrame[f] >= kLoadCount) {
      anyFrameWithAll = true;
    }
  }
  CHECK(!anyFrameWithAll, "no single frame loads everything");

  std::printf("  20 big loads completed across %zu frame(s)\n", frameCount);

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Upload-per-frame limit ---
static void test_upload_limit() noexcept {
  engine::core::initialize_cvars();

  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  // Huge budget but only 2 uploads per frame.
  engine::core::cvar_set_int("asset.streaming_budget_mb", 4096);
  engine::core::cvar_set_int("asset.max_uploads_per_frame", 2);

  constexpr std::size_t kLoadCount = 10U;
  LoadHandle handles[kLoadCount]{};

  for (std::size_t i = 0U; i < kLoadCount; ++i) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "small/asset_%02zu.mesh", i);
    handles[i] =
        load_asset_async(queue.get(), make_id(i), path, LoadPriority::Normal);
  }

  // First frame: all 10 should load (budget is huge), but only 2 upload.
  begin_streaming_frame(queue.get());
  const std::size_t readyFrame1 =
      update_asset_streaming(queue.get(), &big_load, &big_upload, nullptr);
  CHECK(readyFrame1 <= 2U, "at most 2 uploads in frame 1");

  // Process remaining frames.
  std::size_t frameCount = 1U;
  constexpr std::size_t kMaxFrames = 20U;
  while (frameCount < kMaxFrames) {
    begin_streaming_frame(queue.get());
    update_asset_streaming(queue.get(), &big_load, &big_upload, nullptr);
    ++frameCount;

    bool allReady = true;
    for (std::size_t i = 0U; i < kLoadCount; ++i) {
      if (!is_load_ready(queue.get(), handles[i])) {
        allReady = false;
        break;
      }
    }
    if (allReady) {
      break;
    }
  }

  CHECK(frameCount >= 5U, "takes multiple frames due to upload cap");

  // Verify all loaded eventually.
  std::size_t totalReady = 0U;
  for (std::size_t i = 0U; i < kLoadCount; ++i) {
    if (is_load_ready(queue.get(), handles[i])) {
      ++totalReady;
    }
  }
  CHECK(totalReady == kLoadCount, "all 10 eventually loaded with upload cap");

  std::printf("  10 loads with upload cap completed in %zu frame(s)\n",
              frameCount);

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

int main() {
  std::printf("=== Streaming Budget Tests ===\n");

  test_budget_spreading();
  test_upload_limit();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
