// Verifies SparseSet storage behavior, including stale-generation handle
// rejection so recycled entity indices cannot alias old components.

#include "engine/core/entity.h"
#include "engine/core/sparse_set.h"

#include <cstdio>

namespace {

using engine::core::Entity;

constexpr std::size_t kMaxEntities = 64U;
constexpr std::size_t kMaxComponents = 32U;

struct Payload final {
  int value = 0;
};

using PayloadSet =
    engine::core::SparseSet<Entity, Payload, kMaxEntities, kMaxComponents>;

/// Basic add/get/remove round trip with swap-and-pop integrity.
int test_basic_round_trip() {
  PayloadSet set;

  const Entity a{1U, 1U};
  const Entity b{2U, 1U};
  const Entity c{3U, 1U};

  if (!set.add(a, Payload{10}) || !set.add(b, Payload{20}) ||
      !set.add(c, Payload{30})) {
    return 1;
  }

  if (set.count() != 3U) {
    return 2;
  }

  if (!set.remove(b)) {
    return 3;
  }

  Payload out{};
  if (!set.get(a, &out) || (out.value != 10)) {
    return 4;
  }
  if (!set.get(c, &out) || (out.value != 30)) {
    return 5;
  }
  if (set.contains(b) || (set.count() != 2U)) {
    return 6;
  }

  return 0;
}

/// A handle with an outdated generation must miss, not alias the new entity.
int test_stale_generation_rejected() {
  PayloadSet set;

  const Entity oldEntity{5U, 1U};
  if (!set.add(oldEntity, Payload{111})) {
    return 10;
  }

  // Simulate destroy + index reuse: the live entity at index 5 is now gen 2.
  if (!set.remove(oldEntity)) {
    return 11;
  }
  const Entity newEntity{5U, 2U};
  if (!set.add(newEntity, Payload{222})) {
    return 12;
  }

  Payload out{};
  if (set.contains(oldEntity) || set.get(oldEntity, &out) ||
      (set.get_ptr(oldEntity) != nullptr)) {
    return 13;
  }

  if (set.remove(oldEntity)) {
    return 14; // stale remove must not evict the live entity's component
  }

  if (!set.get(newEntity, &out) || (out.value != 222)) {
    return 15;
  }

  return 0;
}

/// Adding under a newer generation of an occupied index adopts the slot.
int test_add_adopts_recycled_index() {
  PayloadSet set;

  const Entity oldEntity{7U, 1U};
  if (!set.add(oldEntity, Payload{7})) {
    return 20;
  }

  // World-level code always removes components on destroy, but if an add
  // arrives for the next generation the slot must transfer, not duplicate.
  const Entity newEntity{7U, 2U};
  if (!set.add(newEntity, Payload{77})) {
    return 21;
  }

  if (set.count() != 1U) {
    return 22;
  }

  Payload out{};
  if (!set.get(newEntity, &out) || (out.value != 77)) {
    return 23;
  }
  if (set.contains(oldEntity)) {
    return 24;
  }

  return 0;
}

/// Out-of-range and reserved index-0 handles are rejected.
int test_range_guards() {
  PayloadSet set;

  if (set.add(Entity{0U, 1U}, Payload{1})) {
    return 30;
  }
  if (set.add(Entity{kMaxEntities + 1U, 1U}, Payload{1})) {
    return 31;
  }
  if (set.contains(Entity{0U, 0U})) {
    return 32;
  }

  return 0;
}

/// Capacity is enforced for fresh inserts but not for overwrites.
int test_capacity() {
  PayloadSet set;

  for (std::size_t i = 0U; i < kMaxComponents; ++i) {
    const Entity entity{static_cast<std::uint32_t>(i + 1U), 1U};
    if (!set.add(entity, Payload{static_cast<int>(i)})) {
      return 40;
    }
  }

  if (set.add(Entity{static_cast<std::uint32_t>(kMaxComponents + 1U), 1U},
              Payload{99})) {
    return 41;
  }

  // Overwrite of an existing entry must still succeed at full capacity.
  if (!set.add(Entity{1U, 1U}, Payload{123})) {
    return 42;
  }

  Payload out{};
  if (!set.get(Entity{1U, 1U}, &out) || (out.value != 123)) {
    return 43;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const int results[] = {
      test_basic_round_trip(),    test_stale_generation_rejected(),
      test_add_adopts_recycled_index(), test_range_guards(),
      test_capacity(),
  };

  for (const int result : results) {
    if (result != 0) {
      std::fprintf(stderr, "sparse_set_test failed with code %d\n", result);
      return result;
    }
  }

  return 0;
}
