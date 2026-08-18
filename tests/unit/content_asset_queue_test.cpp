// Proves the #171 C3 contract that the asset transition-request queue works
// with no renderer dependency: this target links engine_content only.
// Covers the ring's FIFO order, duplicate-pending detection, overflow
// accounting (droppedRequests), pop-on-empty, and clear.

#include "engine/content/asset_request_queue.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

} // namespace

/// Runs this executable or test program.
int main() {
  using namespace engine::content;

  // ~270 KB ring: heap-allocate like production owners do.
  std::unique_ptr<AssetRequestQueue> queue(
      new (std::nothrow) AssetRequestQueue());
  CHECK(queue != nullptr, "queue allocation");

  // Empty-boundary behavior.
  AssetRequest popped{};
  CHECK(!pop_asset_request(queue.get(), &popped), "pop on empty fails");
  CHECK(pending_asset_request_count(queue.get()) == 0U, "empty count");

  // FIFO order across types.
  CHECK(push_asset_request(queue.get(), AssetRequestType::Load, 1U, "a"),
        "push load");
  CHECK(push_asset_request(queue.get(), AssetRequestType::Unload, 2U, nullptr),
        "push unload");
  CHECK(has_pending_asset_request(queue.get(), AssetRequestType::Load, 1U),
        "load pending for id 1");
  CHECK(!has_pending_asset_request(queue.get(), AssetRequestType::Unload, 1U),
        "no unload pending for id 1");
  CHECK(pop_asset_request(queue.get(), &popped) &&
            (popped.type == AssetRequestType::Load) && (popped.id == 1U),
        "FIFO pops the load first");
  CHECK(pop_asset_request(queue.get(), &popped) &&
            (popped.type == AssetRequestType::Unload) && (popped.id == 2U),
        "then the unload");

  // Invalid-id rejection.
  CHECK(!push_asset_request(queue.get(), AssetRequestType::Load,
                            kInvalidAssetId, "x"),
        "invalid id rejected");

  // Overflow: fill to capacity, then one more is dropped and counted.
  for (std::size_t i = 0U; i < AssetRequestQueue::kMaxQueuedRequests; ++i) {
    CHECK(push_asset_request(queue.get(), AssetRequestType::Load,
                             static_cast<AssetId>(i + 1U), nullptr),
          "fill push");
    if (g_failures != 0) {
      break;
    }
  }
  CHECK(!push_asset_request(queue.get(), AssetRequestType::Load, 9999U,
                            nullptr),
        "overflow push fails");
  CHECK(queue->droppedRequests == 1U, "dropped request counted");

  // Clear resets everything including the drop counter.
  clear_asset_request_queue(queue.get());
  CHECK(pending_asset_request_count(queue.get()) == 0U, "cleared count");
  CHECK(queue->droppedRequests == 0U, "cleared drop counter");

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("content_asset_queue_test passed");
  return 0;
}
