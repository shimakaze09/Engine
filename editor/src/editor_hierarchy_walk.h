// Declares the Entities panel's traversal of the world's transform
// hierarchy. Children come from the world's own child links
// (World::for_each_child), so drawing the panel costs O(N) plus each open
// node's children, where scanning every entity per row cost O(N^2).

#pragma once

#include <cstddef>

#include "engine/runtime/world.h"

namespace engine::editor {

namespace detail {

template <typename Enter, typename Leave>
void walk_entity_node(runtime::World &world, runtime::Entity entity,
                      std::size_t depth, std::size_t maxDepth, Enter &enter,
                      Leave &leave) noexcept {
  bool hasChildren = false;
  if (depth < maxDepth) {
    world.for_each_child(entity, [&hasChildren](runtime::Entity) noexcept {
      hasChildren = true;
    });
  }
  if (!enter(entity, depth, hasChildren)) {
    return;
  }
  if (hasChildren) {
    world.for_each_child(entity, [&](runtime::Entity child) noexcept {
      walk_entity_node(world, child, depth + 1U, maxDepth, enter, leave);
    });
  }
  leave(entity);
}

} // namespace detail

/// Walks `world`'s hierarchy in Entities-panel order: each root (an entity
/// with no parent, or no transform) in entity-index order, pre-order, with
/// each node's children in the world's child-link order, which is the
/// order they were attached. enter(entity, depth, hasChildren) is called
/// per row and returns whether the row is open; an open row's children
/// follow, then leave(entity). A node at `maxDepth` reports no children,
/// so a deep chain cannot grow the call stack without bound. The world
/// must not gain or lose entities, or change parents, during the walk:
/// callers defer such edits until it returns.
template <typename Enter, typename Leave>
void walk_entity_hierarchy(runtime::World &world, std::size_t maxDepth,
                           Enter &&enter, Leave &&leave) noexcept {
  world.for_each_alive([&](runtime::Entity entity) noexcept {
    runtime::Transform transform{};
    if (world.get_transform(entity, &transform) &&
        (transform.parentId != runtime::kInvalidPersistentId)) {
      return;
    }
    detail::walk_entity_node(world, entity, 0U, maxDepth, enter, leave);
  });
}

/// Visits `root` and every descendant, pre-order, calling visit(entity)
/// on each; a node `maxDepth` levels below `root` is visited but its
/// children are not. The same no-edit rule as walk_entity_hierarchy holds.
template <typename Visit>
void walk_entity_subtree(runtime::World &world, runtime::Entity root,
                         std::size_t maxDepth, Visit &&visit) noexcept {
  auto enter = [&visit](runtime::Entity entity, std::size_t, bool) noexcept {
    visit(entity);
    return true;
  };
  auto leave = [](runtime::Entity) noexcept {};
  detail::walk_entity_node(world, root, 0U, maxDepth, enter, leave);
}

} // namespace engine::editor
