// P1-M4-C unit tests — exercises the AssetStreamingQueue API in isolation
// without real IO or GL.

#include "engine/core/cvar.h"
#include "engine/renderer/asset_streaming.h"


#include <cstdio>
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

static bool ok_load(AssetId, const char *, std::uint64_t *outSz,
                    void *) noexcept {
  if (outSz != nullptr) {
    *outSz = 1024ULL;
  }
  return true;
}

static bool ok_upload(AssetId, void *) noexcept { return true; }

// --- Basic queue and poll ---
static void test_basic_queue_poll() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  LoadHandle h = load_asset_async(queue.get(), make_id(0), "test.mesh",
                                  LoadPriority::Normal);
  CHECK(h.valid(), "handle valid");
  CHECK(!is_load_ready(queue.get(), h), "not ready before processing");
  CHECK(get_load_state(queue.get(), h) == LoadingState::Queued, "queued");

  begin_streaming_frame(queue.get());
  update_asset_streaming(queue.get(), &ok_load, &ok_upload, nullptr);
  CHECK(is_load_ready(queue.get(), h), "ready after processing");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Dedup: queuing the same asset twice returns the same handle ---
static void test_dedup() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  LoadHandle h1 =
      load_asset_async(queue.get(), make_id(0), "a.mesh", LoadPriority::Low);
  LoadHandle h2 =
      load_asset_async(queue.get(), make_id(0), "a.mesh", LoadPriority::High);
  CHECK(h1 == h2, "same handle for duplicate");
  // Priority should have been upgraded.
  CHECK(pending_load_count(queue.get()) == 1U, "only 1 pending");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Loading states transition correctly ---
static void test_state_transitions() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  // With null callbacks, auto-advances: Queued → Loading → Uploading → Ready.
  LoadHandle h =
      load_asset_async(queue.get(), make_id(0), "s.mesh", LoadPriority::Normal);
  CHECK(get_load_state(queue.get(), h) == LoadingState::Queued,
        "starts queued");

  begin_streaming_frame(queue.get());
  update_asset_streaming(queue.get(), nullptr, nullptr, nullptr);
  CHECK(get_load_state(queue.get(), h) == LoadingState::Ready,
        "ready with null callbacks");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Failed load callback ---
static void test_load_failure() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  // Callback always fails.
  auto fail_load = [](AssetId, const char *, std::uint64_t *,
                      void *) noexcept -> bool { return false; };

  LoadHandle h = load_asset_async(queue.get(), make_id(0), "bad.mesh",
                                  LoadPriority::Normal);

  begin_streaming_frame(queue.get());
  update_asset_streaming(queue.get(), +fail_load, &ok_upload, nullptr);
  CHECK(get_load_state(queue.get(), h) == LoadingState::Failed,
        "marked failed");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Pending count tracking ---
static void test_pending_count() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  CHECK(pending_load_count(queue.get()) == 0U, "initially 0");

  load_asset_async(queue.get(), make_id(0), "a.mesh", LoadPriority::Normal);
  load_asset_async(queue.get(), make_id(1), "b.mesh", LoadPriority::Normal);
  CHECK(pending_load_count(queue.get()) == 2U, "2 pending");

  begin_streaming_frame(queue.get());
  update_asset_streaming(queue.get(), &ok_load, &ok_upload, nullptr);
  CHECK(pending_load_count(queue.get()) == 0U, "0 after processing");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

int main() {
  std::printf("=== Async Streaming Unit Tests ===\n");

  test_basic_queue_poll();
  test_dedup();
  test_state_transitions();
  test_load_failure();
  test_pending_count();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
