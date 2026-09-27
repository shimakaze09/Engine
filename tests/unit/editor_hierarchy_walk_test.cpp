// Verifies the Entities panel's hierarchy walk on random forests of 1,000
// entities, before and after reparents. Every alive entity appears once,
// each after its parent at the parent's depth plus one, and each parent's
// children are exactly those a scan of the world finds. The walk's
// hierarchy work is exactly two visits per parent-child edge, so drawing
// the panel is linear, where the per-row scan it replaces was quadratic.
// A closed row hides its subtree, the depth cap turns deep nodes into
// leaves, and an empty world walks nothing.

#include "editor_hierarchy_walk.h"

#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace {

using engine::editor::walk_entity_hierarchy;
using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::kInvalidPersistentId;
using engine::runtime::Transform;
using engine::runtime::World;

constexpr std::size_t kDepthCap = 64U;

/// Deterministic generator so every run builds the same forests.
struct Lcg final {
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  std::uint32_t next(std::uint32_t bound) noexcept {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>((state >> 33U) % bound);
  }
};

struct Row final {
  Entity entity{};
  std::size_t depth = 0U;
  bool hasChildren = false;
};

Entity parent_of(const World &world, Entity entity) noexcept {
  Transform transform{};
  if (!world.get_transform(entity, &transform) ||
      (transform.parentId == kInvalidPersistentId)) {
    return kInvalidEntity;
  }
  return world.find_entity_by_persistent_id(transform.parentId);
}

bool set_parent(World &world, Entity child, Entity parent) noexcept {
  Transform transform{};
  if (!world.get_transform(child, &transform)) {
    return false;
  }
  transform.parentId = (parent == kInvalidEntity) ? kInvalidPersistentId
                                                  : world.persistent_id(parent);
  return world.add_transform(child, transform);
}

std::vector<Row> walk_all_open(World &world) {
  std::vector<Row> rows;
  walk_entity_hierarchy(
      world, kDepthCap,
      [&rows](Entity entity, std::size_t depth, bool hasChildren) noexcept {
        rows.push_back(Row{entity, depth, hasChildren});
        return true;
      },
      [](Entity) noexcept {});
  return rows;
}

/// Checks the walk against the world. Returns the number of parent-child
/// edges on success, or -1 on the first disagreement.
long long check_rows(World &world, const std::vector<Row> &rows) {
  if (rows.size() != world.alive_entity_count()) {
    return -1;
  }
  std::vector<std::size_t> position(World::kMaxEntities + 1U, SIZE_MAX);
  for (std::size_t i = 0U; i < rows.size(); ++i) {
    if (position[rows[i].entity.index] != SIZE_MAX) {
      return -1; // drawn twice
    }
    position[rows[i].entity.index] = i;
  }
  long long edges = 0;
  for (std::size_t i = 0U; i < rows.size(); ++i) {
    const Entity parent = parent_of(world, rows[i].entity);
    if (parent == kInvalidEntity) {
      if (rows[i].depth != 0U) {
        return -1;
      }
      continue;
    }
    ++edges;
    const std::size_t at = position[parent.index];
    if ((at == SIZE_MAX) || (at >= i) ||
        (rows[at].depth + 1U != rows[i].depth) || !rows[at].hasChildren) {
      return -1;
    }
  }
  // A row reports children exactly when the scan finds some.
  for (const Row &row : rows) {
    bool scanned = false;
    world.for_each_alive([&](Entity candidate) {
      scanned = scanned || (parent_of(world, candidate) == row.entity);
    });
    if (scanned != row.hasChildren) {
      return -1;
    }
  }
  return edges;
}

std::vector<Entity> build_forest(World &world, Lcg &rng, std::size_t count) {
  std::vector<Entity> entities;
  for (std::size_t i = 0U; i < count; ++i) {
    const Entity entity = world.create_scene_object();
    if (entity == kInvalidEntity) {
      break;
    }
    if (!entities.empty() && (rng.next(4U) != 0U)) {
      static_cast<void>(set_parent(
          world, entity,
          entities[rng.next(static_cast<std::uint32_t>(entities.size()))]));
    }
    entities.push_back(entity);
  }
  return entities;
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }

  t.check(walk_all_open(*world).empty(), "an empty world walks nothing");

  Lcg rng{};
  for (int round = 0; round < 3; ++round) {
    engine::runtime::reset_world(*world);
    const std::vector<Entity> entities = build_forest(*world, rng, 1000U);
    t.check(entities.size() == 1000U, "1,000 entities");

    // Settle the world's child links once, then measure a walk.
    static_cast<void>(walk_all_open(*world));
    std::uint64_t before = world->hierarchy_visits();
    std::vector<Row> rows = walk_all_open(*world);
    std::uint64_t visits = world->hierarchy_visits() - before;
    long long edges = check_rows(*world, rows);
    t.check(edges >= 0, "every entity once, after its parent, one deeper");
    t.check(visits == static_cast<std::uint64_t>(2 * edges),
            "the walk visits each edge exactly twice");

    // Reparent a batch (under earlier entities, so no cycles), or to root.
    for (int i = 0; i < 100; ++i) {
      const std::size_t child = 1U + rng.next(999U);
      const Entity parent =
          (rng.next(5U) == 0U)
              ? kInvalidEntity
              : entities[rng.next(static_cast<std::uint32_t>(child))];
      static_cast<void>(set_parent(*world, entities[child], parent));
    }
    static_cast<void>(walk_all_open(*world));
    before = world->hierarchy_visits();
    rows = walk_all_open(*world);
    visits = world->hierarchy_visits() - before;
    edges = check_rows(*world, rows);
    t.check(edges >= 0, "after reparents: every entity once, in place");
    t.check(visits == static_cast<std::uint64_t>(2 * edges),
            "after reparents: each edge visited exactly twice");
  }

  // --- A closed row hides its subtree; leave pairs with every open enter.
  engine::runtime::reset_world(*world);
  const Entity root = world->create_scene_object();
  const Entity child = world->create_scene_object();
  const Entity grandchild = world->create_scene_object();
  t.check(set_parent(*world, child, root) &&
              set_parent(*world, grandchild, child),
          "a three-level chain");
  std::vector<Entity> seen;
  int opened = 0;
  int left = 0;
  walk_entity_hierarchy(
      *world, kDepthCap,
      [&](Entity entity, std::size_t, bool) noexcept {
        seen.push_back(entity);
        const bool open = entity != child;
        opened += open ? 1 : 0;
        return open;
      },
      [&left](Entity) noexcept { ++left; });
  t.check((seen.size() == 2U) && (seen[0] == root) && (seen[1] == child),
          "a closed row's children are not drawn");
  t.check(opened == left, "every open row is closed again");

  // --- The depth cap: a chain of 70 draws rows to depth 64, the last as a
  // leaf.
  engine::runtime::reset_world(*world);
  Entity previous = world->create_scene_object();
  for (int i = 1; i < 70; ++i) {
    const Entity next = world->create_scene_object();
    static_cast<void>(set_parent(*world, next, previous));
    previous = next;
  }
  const std::vector<Row> chain = walk_all_open(*world);
  t.check((chain.size() == kDepthCap + 1U) &&
              (chain.back().depth == kDepthCap) && !chain.back().hasChildren,
          "the depth cap turns the deepest drawn node into a leaf");

  return t.finish("editor_hierarchy_walk");
}
