// Verifies SparseSet and CompactSparseSet storage behavior (#167):
// stale-generation handle rejection so recycled entity indices cannot
// alias old components, identical dense ordering across both layouts, the
// compact layout's tombstone-rebuild churn bound, and its
// capacity-proportional footprint.

#include "engine/core/entity.h"
#include "engine/core/sparse_set.h"

#include <cstdint>
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

using CompactPayloadSet =
    engine::core::CompactSparseSet<Entity, Payload, kMaxEntities,
                                   kMaxComponents>;

/// #167: the compact layout honors every SparseSet contract — round trip,
/// stale-generation rejection, recycled-index adoption, range guards, and
/// capacity — through the identical operation sequences.
int test_compact_matches_contracts() {
  CompactPayloadSet set;

  const Entity a{1U, 1U};
  const Entity b{2U, 1U};
  if (!set.add(a, Payload{10}) || !set.add(b, Payload{20})) {
    return 601;
  }
  Payload out{};
  if (!set.get(a, &out) || (out.value != 10)) {
    return 602;
  }
  if (!set.remove(a) || set.contains(a) || (set.count() != 1U)) {
    return 603; // swap-and-pop kept b reachable
  }
  if (!set.get(b, &out) || (out.value != 20)) {
    return 604;
  }

  const Entity staleB{2U, 0U};
  if (set.contains(staleB) || set.get(staleB, &out) || set.remove(staleB)) {
    return 605; // stale generation must miss, never alias
  }

  const Entity recycledB{2U, 2U};
  if (!set.add(recycledB, Payload{30})) {
    return 606; // newer generation adopts the recycled index's slot
  }
  if (!set.get(recycledB, &out) || (out.value != 30) || (set.count() != 1U)) {
    return 607;
  }
  if (set.contains(b)) {
    return 608; // the old generation lost the slot
  }

  if (set.add(Entity{0U, 1U}, Payload{}) ||
      set.add(Entity{kMaxEntities + 1U, 1U}, Payload{})) {
    return 609; // index 0 and beyond-range rejected
  }

  set.clear();
  for (std::uint32_t i = 0U; i < kMaxComponents; ++i) {
    if (!set.add(Entity{i + 1U, 1U}, Payload{static_cast<int>(i)})) {
      return 610;
    }
  }
  if (set.add(Entity{kMaxComponents + 1U, 1U}, Payload{})) {
    return 611; // full pool rejects, never silently drops
  }
  return 0;
}

/// #167: identical operation sequences produce byte-identical dense order
/// in both layouts — serialization and iteration determinism cannot depend
/// on which lookup structure a pool uses.
int test_compact_dense_order_matches_sparse() {
  PayloadSet classic;
  CompactPayloadSet compact;

  // A deterministic add/remove churn that exercises swap-remove holes.
  std::uint32_t nextValue = 0U;
  for (std::uint32_t round = 0U; round < 6U; ++round) {
    for (std::uint32_t i = 1U; i <= 24U; ++i) {
      const Entity entity{i, round + 1U};
      const Payload payload{static_cast<int>(++nextValue)};
      if (classic.add(entity, payload) != compact.add(entity, payload)) {
        return 620;
      }
    }
    for (std::uint32_t i = 1U; i <= 24U; i += 3U) {
      const Entity entity{i, round + 1U};
      if (classic.remove(entity) != compact.remove(entity)) {
        return 621;
      }
    }
  }

  if (classic.count() != compact.count()) {
    return 622;
  }
  for (std::size_t slot = 0U; slot < classic.count(); ++slot) {
    if ((classic.entity_at(slot).index != compact.entity_at(slot).index) ||
        (classic.entity_at(slot).generation !=
         compact.entity_at(slot).generation) ||
        (classic.component_at(slot).value != compact.component_at(slot).value)) {
      return 623;
    }
  }
  return 0;
}

/// #167: heavy add/remove churn far beyond the tombstone-rebuild boundary
/// keeps lookups exact — the bounded rebuild must not lose or corrupt the
/// index mapping.
int test_compact_churn_survives_tombstone_rebuild() {
  CompactPayloadSet set;
  std::uint32_t generation = 1U;
  for (std::uint32_t round = 0U; round < 512U; ++round) {
    // Rotate through the whole entity-index range so hash slots vary.
    const std::uint32_t index = (round % kMaxEntities) + 1U;
    const Entity entity{index, generation};
    if (!set.add(entity, Payload{static_cast<int>(round)})) {
      return 630;
    }
    Payload out{};
    if (!set.get(entity, &out) || (out.value != static_cast<int>(round))) {
      return 631;
    }
    if (!set.remove(entity)) {
      return 632;
    }
    if (set.contains(entity) || (set.count() != 0U)) {
      return 633;
    }
    ++generation;
  }
  // The set is still fully usable after the churn.
  for (std::uint32_t i = 0U; i < kMaxComponents; ++i) {
    if (!set.add(Entity{i + 1U, generation}, Payload{static_cast<int>(i)})) {
      return 634;
    }
  }
  return (set.count() == kMaxComponents) ? 0 : 635;
}

/// #167: the compact layout's whole point — its footprint tracks the dense
/// capacity, not the entity capacity.
int test_compact_footprint_is_capacity_bound() {
  using WideCompact =
      engine::core::CompactSparseSet<Entity, Payload, 65536U, 64U>;
  using WideClassic = engine::core::SparseSet<Entity, Payload, 65536U, 64U>;
  static_assert(sizeof(WideCompact) < (16U * 1024U),
                "compact pool must not scale with MaxEntities");
  static_assert(sizeof(WideClassic) > (256U * 1024U),
                "classic pool is entity-indexed by design");
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const int results[] = {
      test_basic_round_trip(),    test_stale_generation_rejected(),
      test_add_adopts_recycled_index(), test_range_guards(),
      test_capacity(),
      test_compact_matches_contracts(),
      test_compact_dense_order_matches_sparse(),
      test_compact_churn_survives_tombstone_rebuild(),
      test_compact_footprint_is_capacity_bound(),
  };

  for (const int result : results) {
    if (result != 0) {
      std::fprintf(stderr, "sparse_set_test failed with code %d\n", result);
      return result;
    }
  }

  return 0;
}
