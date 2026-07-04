// P1-M4-C unit tests — exercises the AssetStreamingQueue API in isolation
// without real IO or GL.

#include "engine/core/cvar.h"
#include "engine/renderer/asset_streaming.h"


#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "../test_harness.h"

using namespace engine::renderer;

static engine::tests::TestContext g_tests;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    g_tests.check((cond), (msg));                                              \
  } while (false)

/// Handles make id.
static AssetId make_id(std::uint64_t n) noexcept {
  return static_cast<AssetId>(n + 1U);
}

/// Handles ok load.
static bool ok_load(AssetId, const char *, std::uint64_t *outSz,
                    void *) noexcept {
  if (outSz != nullptr) {
    *outSz = 1024ULL;
  }
  return true;
}

/// Handles ok upload.
static bool ok_upload(AssetId, void *) noexcept { return true; }

struct ConcurrentLoadProbe final {
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
};

/// Records the highest value observed for a concurrent counter.
static void record_max(std::atomic<int> *target, int value) noexcept {
  int observed = target->load(std::memory_order_relaxed);
  while ((observed < value) &&
         !target->compare_exchange_weak(observed, value,
                                        std::memory_order_relaxed)) {
  }
}

/// Handles a slow load while tracking callback concurrency.
static bool concurrent_load(AssetId, const char *, std::uint64_t *outSz,
                            void *userData) noexcept {
  auto *probe = static_cast<ConcurrentLoadProbe *>(userData);
  const int active =
      probe->active.fetch_add(1, std::memory_order_relaxed) + 1;
  record_max(&probe->maxActive, active);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  probe->active.fetch_sub(1, std::memory_order_relaxed);
  if (outSz != nullptr) {
    *outSz = 1024ULL;
  }
  return true;
}

/// Advances the streaming queue until a handle reaches a terminal state.
static void pump_until_terminal(AssetStreamingQueue *queue, LoadHandle handle,
                                AssetLoadCallback loadCallback,
                                AssetUploadCallback uploadCallback,
                                void *userData) noexcept {
  for (std::size_t frame = 0U; frame < 128U; ++frame) {
    const LoadingState state = get_load_state(queue, handle);
    if ((state == LoadingState::Ready) || (state == LoadingState::Failed)) {
      return;
    }
    begin_streaming_frame(queue);
    static_cast<void>(
        update_asset_streaming(queue, loadCallback, uploadCallback, userData));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

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

  pump_until_terminal(queue.get(), h, &ok_load, &ok_upload, nullptr);
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

  pump_until_terminal(queue.get(), h, nullptr, nullptr, nullptr);
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

  pump_until_terminal(queue.get(), h, +fail_load, &ok_upload, nullptr);
  CHECK(get_load_state(queue.get(), h) == LoadingState::Failed,
        "marked failed");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

static void test_release_terminal_requests() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  LoadHandle ready = load_asset_async(queue.get(), make_id(0), "ready.mesh",
                                      LoadPriority::Normal);
  pump_until_terminal(queue.get(), ready, &ok_load, &ok_upload, nullptr);
  CHECK(is_load_ready(queue.get(), ready), "ready request completed");
  CHECK(release_load(queue.get(), ready), "release ready request");
  CHECK(queue->count == 0U, "ready release decrements queue count");
  CHECK(!release_load(queue.get(), ready), "cannot release twice");

  auto fail_load = [](AssetId, const char *, std::uint64_t *,
                      void *) noexcept -> bool { return false; };

  LoadHandle failed = load_asset_async(queue.get(), make_id(1), "failed.mesh",
                                       LoadPriority::Normal);
  pump_until_terminal(queue.get(), failed, +fail_load, &ok_upload, nullptr);
  CHECK(get_load_state(queue.get(), failed) == LoadingState::Failed,
        "failed request reached terminal state");
  CHECK(release_load(queue.get(), failed), "release failed request");
  CHECK(queue->count == 0U, "failed release decrements queue count");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

static void test_release_reuses_terminal_slots() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
      LoadRequest &request = queue->requests[i];
      request.assetId = make_id(i);
      request.state = LoadingState::Ready;
      request.occupied = true;
    }
    queue->count = AssetStreamingQueue::kMaxRequests;
  }

  const LoadHandle full =
      load_asset_async(queue.get(), make_id(AssetStreamingQueue::kMaxRequests),
                       "overflow.mesh", LoadPriority::Normal);
  CHECK(!full.valid(), "full terminal queue rejects new request");

  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    CHECK(release_load(queue.get(), LoadHandle{i, queue->requests[i].generation}),
          "release terminal slot");
  }
  CHECK(queue->count == 0U, "all terminal slots released");

  LoadHandle reused =
      load_asset_async(queue.get(), make_id(AssetStreamingQueue::kMaxRequests),
                       "reused.mesh", LoadPriority::Normal);
  CHECK(reused.valid(), "released terminal slots can be reused");
  CHECK(cancel_load(queue.get(), reused), "cleanup reused queued request");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

static void test_stale_handles_do_not_alias_reused_slots() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  LoadHandle original =
      load_asset_async(queue.get(), make_id(0), "original.mesh",
                       LoadPriority::Normal);
  pump_until_terminal(queue.get(), original, &ok_load, &ok_upload, nullptr);
  CHECK(is_load_ready(queue.get(), original), "original request ready");
  CHECK(release_load(queue.get(), original), "release original request");

  LoadHandle reused =
      load_asset_async(queue.get(), make_id(1), "reused.mesh",
                       LoadPriority::Normal);
  CHECK(reused.valid(), "reused request valid");
  CHECK(reused.index == original.index, "released slot reused");
  CHECK(reused.generation != original.generation, "generation changed");

  CHECK(!is_load_ready(queue.get(), original), "stale handle not ready");
  CHECK(get_load_state(queue.get(), original) == LoadingState::Failed,
        "stale handle reports failed state");
  CHECK(!update_load_priority(queue.get(), original, LoadPriority::Immediate),
        "stale handle cannot update priority");
  CHECK(!cancel_load(queue.get(), original), "stale handle cannot cancel");
  CHECK(!release_load(queue.get(), original), "stale handle cannot release");
  wait_for_load(queue.get(), original);

  CHECK(cancel_load(queue.get(), reused), "current reused handle still works");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

static void test_upload_cap_clamps_non_positive_values() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  engine::core::cvar_set_int("asset.max_uploads_per_frame", -1);
  begin_streaming_frame(queue.get());
  CHECK(queue->maxUploadsPerFrame == 8U, "negative upload cap uses default");

  engine::core::cvar_set_int("asset.max_uploads_per_frame", 0);
  begin_streaming_frame(queue.get());
  CHECK(queue->maxUploadsPerFrame == 8U, "zero upload cap uses default");

  engine::core::cvar_set_int("asset.max_uploads_per_frame", 3);
  begin_streaming_frame(queue.get());
  CHECK(queue->maxUploadsPerFrame == 3U, "positive upload cap applied");

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

  for (std::size_t frame = 0U; frame < 128U; ++frame) {
    if (pending_load_count(queue.get()) == 0U) {
      break;
    }
    begin_streaming_frame(queue.get());
    static_cast<void>(
        update_asset_streaming(queue.get(), &ok_load, &ok_upload, nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(pending_load_count(queue.get()) == 0U, "0 after processing");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Load callbacks must not execute inline on the frame thread. ---
static void test_load_callback_is_async() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  auto slow_load = [](AssetId, const char *, std::uint64_t *outSz,
                      void *) noexcept -> bool {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (outSz != nullptr) {
      *outSz = 1024ULL;
    }
    return true;
  };

  LoadHandle h = load_asset_async(queue.get(), make_id(0), "slow.mesh",
                                  LoadPriority::Normal);
  const auto before = std::chrono::steady_clock::now();
  begin_streaming_frame(queue.get());
  static_cast<void>(
      update_asset_streaming(queue.get(), +slow_load, &ok_upload, nullptr));
  const auto after = std::chrono::steady_clock::now();
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(after - before)
          .count();
  CHECK(elapsedMs < 30, "update does not block on load callback");

  pump_until_terminal(queue.get(), h, +slow_load, &ok_upload, nullptr);
  CHECK(is_load_ready(queue.get(), h), "slow load eventually ready");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

static void test_worker_pool_runs_concurrent_loads() noexcept {
  engine::core::initialize_cvars();
  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  ConcurrentLoadProbe probe{};
  LoadHandle handles[AssetStreamingQueue::kWorkerCount]{};
  for (std::size_t i = 0U; i < AssetStreamingQueue::kWorkerCount; ++i) {
    handles[i] =
        load_asset_async(queue.get(), make_id(i), "pool.mesh",
                         LoadPriority::Normal);
    CHECK(handles[i].valid(), "worker pool test handle valid");
  }

  begin_streaming_frame(queue.get());
  static_cast<void>(
      update_asset_streaming(queue.get(), &concurrent_load, &ok_upload,
                             &probe));

  for (std::size_t wait = 0U; wait < 200U; ++wait) {
    if (probe.maxActive.load(std::memory_order_relaxed) >= 2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(probe.maxActive.load(std::memory_order_relaxed) >= 2,
        "worker pool runs load callbacks concurrently");

  bool allReady = false;
  for (std::size_t frame = 0U; frame < 128U; ++frame) {
    begin_streaming_frame(queue.get());
    static_cast<void>(
        update_asset_streaming(queue.get(), &concurrent_load, &ok_upload,
                               &probe));

    allReady = true;
    for (LoadHandle handle : handles) {
      if (!is_load_ready(queue.get(), handle)) {
        allReady = false;
        break;
      }
    }
    if (allReady) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(allReady, "worker pool requests eventually ready");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Async Streaming Unit Tests ===\n");

  test_basic_queue_poll();
  test_dedup();
  test_state_transitions();
  test_load_failure();
  test_release_terminal_requests();
  test_release_reuses_terminal_slots();
  test_stale_handles_do_not_alias_reused_slots();
  test_upload_cap_clamps_non_positive_values();
  test_pending_count();
  test_load_callback_is_async();
  test_worker_pool_runs_concurrent_loads();

  return g_tests.finish("Async Streaming Unit Tests");
}
