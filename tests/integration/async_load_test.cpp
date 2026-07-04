// P1-M4-C1d: Test — queue 50 mesh loads, poll until all ready, verify all
// loaded correctly.

#include "engine/core/cvar.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_streaming.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <thread>

using namespace engine::renderer;

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Handles make id.
static AssetId make_id(std::uint64_t n) noexcept {
  return static_cast<AssetId>(n + 1U);
}

// Dummy load callback — always succeeds, reports 1KB per asset.
static bool dummy_load(AssetId /*id*/, const char * /*path*/,
                       std::uint64_t *outSizeBytes,
                       void * /*userData*/) noexcept {
  if (outSizeBytes != nullptr) {
    *outSizeBytes = 1024ULL;
  }
  return true;
}

// Dummy upload callback — always succeeds.
static bool dummy_upload(AssetId /*id*/, void * /*userData*/) noexcept {
  return true;
}

/// Records worker load order for priority assertions.
struct LoadOrder final {
  AssetId ids[64]{};
  std::size_t count = 0U;
};

/// Handles recording load.
static bool recording_load(AssetId id, const char *path,
                           std::uint64_t *outSizeBytes,
                           void *userData) noexcept {
  auto *order = static_cast<LoadOrder *>(userData);
  if ((order != nullptr) && (order->count < 64U)) {
    order->ids[order->count++] = id;
  }
  return dummy_load(id, path, outSizeBytes, nullptr);
}

/// Advances the queue until all supplied handles are ready or failed.
static void pump_until_all_ready(AssetStreamingQueue *queue,
                                 const LoadHandle *handles,
                                 std::size_t handleCount,
                                 AssetLoadCallback loadCallback,
                                 AssetUploadCallback uploadCallback,
                                 void *userData) noexcept {
  for (std::size_t frame = 0U; frame < 160U; ++frame) {
    bool allReady = true;
    for (std::size_t i = 0U; i < handleCount; ++i) {
      if (!is_load_ready(queue, handles[i])) {
        allReady = false;
        break;
      }
    }
    if (allReady) {
      return;
    }
    begin_streaming_frame(queue);
    static_cast<void>(
        update_asset_streaming(queue, loadCallback, uploadCallback, userData));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// --- Queue 50 loads and poll to completion ---
static void test_50_mesh_loads() noexcept {
  engine::core::initialize_cvars();

  auto queue = std::make_unique<AssetStreamingQueue>();
  CHECK(initialize_asset_streaming(queue.get()), "init streaming");

  constexpr std::size_t kLoadCount = 50U;
  LoadHandle handles[kLoadCount]{};

  for (std::size_t i = 0U; i < kLoadCount; ++i) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "meshes/mesh_%03zu.mesh", i);
    handles[i] =
        load_asset_async(queue.get(), make_id(i), path, LoadPriority::Normal);
    CHECK(handles[i].valid(), "handle valid");
  }

  CHECK(pending_load_count(queue.get()) == kLoadCount, "all 50 pending");

  // Process frames until all loads are ready.
  std::size_t frameCount = 0U;
  constexpr std::size_t kMaxFrames = 100U;

  while (frameCount < kMaxFrames) {
    begin_streaming_frame(queue.get());
    update_asset_streaming(queue.get(), &dummy_load, &dummy_upload, nullptr);
    ++frameCount;

    // Check if all are ready.
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Verify all 50 are ready.
  std::size_t readyCount = 0U;
  for (std::size_t i = 0U; i < kLoadCount; ++i) {
    if (is_load_ready(queue.get(), handles[i])) {
      ++readyCount;
    }
  }

  CHECK(readyCount == kLoadCount, "all 50 loads completed");
  std::printf("  50 mesh loads completed in %zu frame(s)\n", frameCount);

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Priority ordering ---
static void test_priority_ordering() noexcept {
  engine::core::initialize_cvars();

  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  // Set budget low enough to allow one conservative load slot per frame.
  engine::core::cvar_set_int("asset.streaming_budget_mb", 1);
  engine::core::cvar_set_int("asset.max_uploads_per_frame", 1024);

  // Queue in order: Low, Normal, High, Immediate.
  LoadHandle hLow =
      load_asset_async(queue.get(), make_id(0), "low.mesh", LoadPriority::Low);
  LoadHandle hNorm = load_asset_async(queue.get(), make_id(1), "norm.mesh",
                                      LoadPriority::Normal);
  LoadHandle hHigh = load_asset_async(queue.get(), make_id(2), "high.mesh",
                                      LoadPriority::High);
  LoadHandle hImm = load_asset_async(queue.get(), make_id(3), "imm.mesh",
                                     LoadPriority::Immediate);

  LoadHandle handles[] = {hLow, hNorm, hHigh, hImm};
  LoadOrder order{};
  pump_until_all_ready(queue.get(), handles, 4U, &recording_load,
                       &dummy_upload, &order);
  CHECK(order.count >= 4U, "all priority loads recorded");
  CHECK(order.ids[0] == make_id(3), "Immediate first");
  CHECK(order.ids[1] == make_id(2), "High second");
  CHECK(order.ids[2] == make_id(1), "Normal third");
  CHECK(order.ids[3] == make_id(0), "Low fourth");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Update priority while queued ---
static void test_update_priority() noexcept {
  engine::core::initialize_cvars();

  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  engine::core::cvar_set_int("asset.streaming_budget_mb", 1);
  engine::core::cvar_set_int("asset.max_uploads_per_frame", 1024);

  LoadHandle hA =
      load_asset_async(queue.get(), make_id(0), "a.mesh", LoadPriority::Low);
  // hB intentionally unused — we only check A gets promoted.
  static_cast<void>(load_asset_async(queue.get(), make_id(1), "b.mesh",
                                     LoadPriority::Normal));

  // Promote A to Immediate.
  CHECK(update_load_priority(queue.get(), hA, LoadPriority::Immediate),
        "priority update");

  LoadHandle handles[] = {hA};
  LoadOrder order{};
  pump_until_all_ready(queue.get(), handles, 1U, &recording_load,
                       &dummy_upload, &order);
  CHECK(is_load_ready(queue.get(), hA), "promoted asset loaded first");
  CHECK((order.count > 0U) && (order.ids[0] == make_id(0)),
        "promoted asset was first worker load");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Cancel load ---
static void test_cancel_load() noexcept {
  engine::core::initialize_cvars();

  auto queue = std::make_unique<AssetStreamingQueue>();
  initialize_asset_streaming(queue.get());

  LoadHandle h =
      load_asset_async(queue.get(), make_id(0), "x.mesh", LoadPriority::Normal);
  CHECK(h.valid(), "handle valid");
  CHECK(cancel_load(queue.get(), h), "cancel succeeds");
  CHECK(get_load_state(queue.get(), h) == LoadingState::Failed,
        "cancelled state = Failed");

  shutdown_asset_streaming(queue.get());
  engine::core::shutdown_cvars();
}

// --- Null/invalid handle safety ---
static void test_null_safety() noexcept {
  CHECK(!is_load_ready(nullptr, kInvalidLoadHandle), "null queue ready");
  CHECK(get_load_state(nullptr, kInvalidLoadHandle) == LoadingState::Failed,
        "null queue state");
  CHECK(!update_load_priority(nullptr, kInvalidLoadHandle, LoadPriority::High),
        "null queue update");
  CHECK(!cancel_load(nullptr, kInvalidLoadHandle), "null queue cancel");
  CHECK(pending_load_count(nullptr) == 0U, "null queue pending count");
}

/// Runs this executable or test program.
int main() {
  std::printf("=== Async Load Tests ===\n");

  test_50_mesh_loads();
  test_priority_ordering();
  test_update_priority();
  test_cancel_load();
  test_null_safety();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
