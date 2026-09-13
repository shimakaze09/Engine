// Verifies core::FixedRing's FIFO contract at its boundaries: empty pop
// and lookups fail, one element round-trips, a full ring refuses push but
// push_overwrite drops the oldest, logical indices stay oldest-first after
// the ring wraps, a popped slot is reset to the default, and clear empties
// the ring and its slots.

#include <cstddef>
#include <cstdint>

#include "../test_harness.h"
#include "engine/core/fixed_ring.h"

namespace {

struct Item final {
  std::uint32_t value = 0U;
  std::uint32_t tag = 0U;
};

using Ring = engine::core::FixedRing<Item, 4U>;

Item make(std::uint32_t value) noexcept {
  Item item{};
  item.value = value;
  item.tag = 0xABCDU;
  return item;
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  Ring ring{};

  // Zero elements.
  Item out{};
  ctx.check(ring.empty() && !ring.full() && (ring.size() == 0U), "starts empty");
  ctx.check(!ring.pop(&out), "pop on empty fails");
  ctx.check((ring.at(0U) == nullptr) && (ring.back() == nullptr),
            "lookups on empty return null");
  ctx.check(!ring.pop(nullptr), "pop into null fails");

  // One element.
  ctx.check(ring.push(make(1U)), "push one");
  ctx.check((ring.size() == 1U) && !ring.empty() && !ring.full(),
            "one element counted");
  ctx.check((ring.at(0U) != nullptr) && (ring.at(0U)->value == 1U) &&
                (ring.back() == ring.at(0U)) && (ring.at(1U) == nullptr),
            "front and back are the one element");
  ctx.check(ring.pop(&out) && (out.value == 1U) && ring.empty(),
            "pop returns it and empties the ring");

  // Many, to capacity, then refusal.
  for (std::uint32_t i = 1U; i <= Ring::kCapacity; ++i) {
    ctx.check(ring.push(make(i)), "fill push");
  }
  ctx.check(ring.full() && (ring.size() == Ring::kCapacity), "ring is full");
  ctx.check(!ring.push(make(99U)), "push on full refuses");
  ctx.check((ring.back() != nullptr) && (ring.back()->value == Ring::kCapacity),
            "refused push wrote nothing");
  ctx.check(ring.at(Ring::kCapacity) == nullptr, "index at size is out of range");

  // Overwrite drops the oldest and keeps oldest-first order across the wrap.
  ctx.check(ring.push_overwrite(make(5U)), "push_overwrite on full drops one");
  ctx.check(ring.full() && (ring.size() == Ring::kCapacity),
            "size unchanged after overwrite");
  bool ordered = true;
  for (std::size_t i = 0U; i < ring.size(); ++i) {
    const Item *item = ring.at(i);
    ordered = ordered && (item != nullptr) &&
              (item->value == static_cast<std::uint32_t>(i + 2U));
  }
  ctx.check(ordered, "indices read 2, 3, 4, 5 after the wrap");
  ctx.check((ring.back() != nullptr) && (ring.back()->value == 5U),
            "back is the overwriting element");

  // Pop after the wrap, slot reset, then push_overwrite with room.
  ctx.check(ring.pop(&out) && (out.value == 2U) && (ring.size() == 3U),
            "pop after wrap returns the oldest");
  ctx.check(!ring.push_overwrite(make(6U)), "push_overwrite with room drops nothing");
  ctx.check((ring.size() == 4U) && (ring.at(0U)->value == 3U) &&
                (ring.at(3U)->value == 6U),
            "order holds after pop and refill");

  // Drain fully, then verify vacated slots read as default when reused.
  std::uint32_t popped = 0U;
  while (ring.pop(&out)) {
    ++popped;
  }
  ctx.check((popped == 4U) && ring.empty(), "drain pops every element");
  ctx.check(ring.push(Item{}) && (ring.at(0U)->tag == 0U),
            "a vacated slot holds the default before reuse");

  // Clear empties the ring and resets slots.
  ctx.check(ring.push(make(7U)), "push before clear");
  ring.clear();
  ctx.check(ring.empty() && (ring.at(0U) == nullptr), "clear empties the ring");
  ctx.check(ring.push(Item{}) && (ring.at(0U)->tag == 0U) &&
                (ring.at(0U)->value == 0U),
            "cleared slots hold the default");

  return ctx.finish("fixed_ring");
}
