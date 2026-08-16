// Implements the editor's undoable edit commands and component snapshot
// helpers. Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_commands.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/physics/primitive_hulls.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

runtime::Entity resolve_command_target(
    runtime::Entity entity, runtime::PersistentId persistentId) noexcept {
  if ((editor_session().world == nullptr) ||
      (persistentId == runtime::kInvalidPersistentId)) {
    return entity;
  }
  return editor_session().world->find_entity_by_persistent_id(persistentId);
}

// capture_component_snapshot and apply_component_snapshot are generated in
// editor_component_registry.cpp from the persistent-component registry.

/// Pending inspector edit gesture: the opening component snapshot plus
/// the target identity, committed as one undoable command when the
/// gesture ends (widget deactivation, target switch, or panel handoff).
struct PendingInspectorEdit final {
  bool active = false;
  bool applied = false;
  ComponentEditType type = ComponentEditType::Transform;
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  ComponentEditSnapshot before{};
};

/// Process-wide pending gesture behind the inspector_*_edit functions.
static PendingInspectorEdit g_pendingInspectorEdit{};

/// Repairs state a raw field write could corrupt before it reaches the
/// world: a changed controller path drops the cached controller binding
/// and state-machine position, and an editable non-zero rotation is
/// renormalized (a zeroed rotation falls back to the pre-edit value).
static void sanitize_staged_component(ComponentEditType type,
                                      const ComponentEditSnapshot &before,
                                      ComponentEditSnapshot *after) noexcept {
  if (type == ComponentEditType::Animation) {
    if (std::strcmp(before.animation.controllerPath,
                    after->animation.controllerPath) != 0) {
      after->animation.controllerSlot = runtime::kInvalidAnimSlot;
      after->animation.currentState = 0U;
      after->animation.previousState = 0U;
      after->animation.stateTime = 0.0F;
      after->animation.previousStateTime = 0.0F;
      after->animation.blendRemaining = 0.0F;
      after->animation.blendDuration = 0.0F;
    }
    return;
  }
  if (type == ComponentEditType::Transform) {
    math::Quat &rotation = after->transform.rotation;
    const float lengthSq =
        (rotation.x * rotation.x) + (rotation.y * rotation.y) +
        (rotation.z * rotation.z) + (rotation.w * rotation.w);
    if (lengthSq > 1.0e-6F) {
      rotation = math::normalize(rotation);
    } else {
      rotation = before.transform.rotation;
    }
  }
}

bool inspector_stage_component_edit(
    runtime::Entity entity, ComponentEditType type,
    const ComponentEditSnapshot &before,
    const ComponentEditSnapshot &after) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }
  const runtime::PersistentId persistentId = world->persistent_id(entity);
  PendingInspectorEdit &pending = g_pendingInspectorEdit;
  if (pending.active &&
      ((pending.type != type) || (pending.entity != entity) ||
       (pending.persistentId != persistentId))) {
    inspector_commit_pending_edit();
  }
  ComponentEditSnapshot sanitized = after;
  sanitize_staged_component(type, before, &sanitized);
  if (!apply_component_snapshot(type, entity, true, sanitized)) {
    return false;
  }
  if (!pending.active) {
    pending.active = true;
    pending.applied = false;
    pending.type = type;
    pending.entity = entity;
    pending.persistentId = persistentId;
    pending.before = before;
  }
  pending.applied = true;
  return true;
}

void inspector_commit_pending_edit() noexcept {
  PendingInspectorEdit &pending = g_pendingInspectorEdit;
  if (!pending.active) {
    return;
  }
  pending.active = false;
  if (!pending.applied) {
    return;
  }
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return;
  }
  const runtime::Entity target =
      resolve_command_target(pending.entity, pending.persistentId);
  ComponentEditSnapshot current{};
  if (!capture_component_snapshot(pending.type, target, &current)) {
    return;
  }
  auto *cmd = new (std::nothrow) ComponentEditCommand();
  if (cmd == nullptr) {
    return;
  }
  cmd->entity = pending.entity;
  cmd->persistentId = pending.persistentId;
  cmd->type = pending.type;
  cmd->beforeExists = true;
  cmd->before = pending.before;
  cmd->afterExists = true;
  cmd->after = current;
  editor_session().commandHistory.execute(cmd);
}

void inspector_abandon_pending_edit() noexcept {
  g_pendingInspectorEdit.active = false;
  g_pendingInspectorEdit.applied = false;
}

bool inspector_has_pending_edit() noexcept {
  return g_pendingInspectorEdit.active;
}

void execute_component_add(runtime::Entity entity, ComponentEditType type,
                           const ComponentEditSnapshot &after) noexcept {
  inspector_commit_pending_edit();
  ComponentEditSnapshot before{};
  const bool beforeExists = capture_component_snapshot(type, entity, &before);

  auto *cmd = new (std::nothrow) ComponentEditCommand();
  if (cmd == nullptr) {
    static_cast<void>(apply_component_snapshot(type, entity, true, after));
    return;
  }

  cmd->entity = entity;
  cmd->persistentId = (editor_session().world != nullptr)
                          ? editor_session().world->persistent_id(entity)
                          : runtime::kInvalidPersistentId;
  cmd->type = type;
  cmd->beforeExists = beforeExists;
  cmd->before = before;
  cmd->afterExists = true;
  cmd->after = after;
  editor_session().commandHistory.execute(cmd);
}


void execute_component_remove(runtime::Entity entity,
                              ComponentEditType type) noexcept {
  inspector_commit_pending_edit();
  ComponentEditSnapshot before{};
  if (!capture_component_snapshot(type, entity, &before)) {
    return;
  }

  auto *cmd = new (std::nothrow) ComponentEditCommand();
  if (cmd == nullptr) {
    static_cast<void>(apply_component_snapshot(type, entity, false, before));
    return;
  }

  cmd->entity = entity;
  cmd->persistentId = editor_session().world->persistent_id(entity);
  cmd->type = type;
  cmd->beforeExists = true;
  cmd->before = before;
  cmd->afterExists = false;
  editor_session().commandHistory.execute(cmd);
}


/// Applies a parent persistent id onto the child's transform.
static bool apply_parent_id(runtime::Entity child,
                            runtime::PersistentId parentId) noexcept {
  runtime::World *world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity resolved = world->find_entity_by_index(child.index);
  if ((resolved == runtime::kInvalidEntity) ||
      (resolved.generation != child.generation)) {
    return false;
  }
  runtime::Transform transform{};
  if (!world->get_transform(resolved, &transform)) {
    return false;
  }
  transform.parentId = parentId;
  return world->add_transform(resolved, transform);
}

bool ReparentCommand::execute() noexcept {
  return apply_parent_id(
      resolve_command_target(child, childPersistentId), afterParentId);
}

bool ReparentCommand::undo() noexcept {
  return apply_parent_id(
      resolve_command_target(child, childPersistentId), beforeParentId);
}

bool execute_reparent(runtime::Entity child,
                      runtime::Entity newParent) noexcept {
  runtime::World *world = editor_session().world;
  if ((world == nullptr) || (child == runtime::kInvalidEntity) ||
      (child == newParent)) {
    return false;
  }

  runtime::PersistentId afterId = runtime::kInvalidPersistentId;
  if (newParent != runtime::kInvalidEntity) {
    afterId = world->persistent_id(newParent);
    if (afterId == runtime::kInvalidPersistentId) {
      return false;
    }
    runtime::Entity cursor = newParent;
    const std::size_t maxAncestors = world->alive_entity_count() + 1U;
    bool reachedRoot = false;
    for (std::size_t depth = 0U; depth < maxAncestors; ++depth) {
      runtime::Transform cursorTransform{};
      if (!world->get_transform(cursor, &cursorTransform) ||
          (cursorTransform.parentId == runtime::kInvalidPersistentId)) {
        reachedRoot = true;
        break;
      }
      cursor = world->find_entity_by_persistent_id(cursorTransform.parentId);
      if (cursor == runtime::kInvalidEntity) {
        reachedRoot = true;
        break;
      }
      if (cursor == child) {
        return false;
      }
    }
    if (!reachedRoot) {
      return false;
    }
  }

  runtime::Transform before{};
  if (!world->get_transform(child, &before)) {
    return false;
  }
  if (before.parentId == afterId) {
    return true;
  }

  // Prove the reparent is legal (add_transform enforces the
  // dynamic-body-root rule) before recording it, then revert and route
  // the real application through the command history.
  if (!apply_parent_id(child, afterId)) {
    return false;
  }
  runtime::Transform applied{};
  if (!world->get_transform(child, &applied) ||
      (applied.parentId != afterId)) {
    return false;
  }
  static_cast<void>(apply_parent_id(child, before.parentId));

  auto *command = new (std::nothrow) ReparentCommand();
  if (command == nullptr) {
    return false;
  }
  command->child = child;
  command->childPersistentId = world->persistent_id(child);
  command->beforeParentId = before.parentId;
  command->afterParentId = afterId;
  return editor_session().commandHistory.execute(command);
}

bool EntityCreateCommand::execute() noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity =
      (persistentId == runtime::kInvalidPersistentId)
          ? world->create_scene_object(transform)
          : world->create_scene_object_with_persistent_id(persistentId,
                                                          transform);
  if (entity == runtime::kInvalidEntity) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity create command could not create the entity");
    return false;
  }
  persistentId = world->persistent_id(entity);
  if (name.name[0] == '\0') {
    make_default_entity_name(entity.index, &name);
  }
  // Any failed insertion rolls the whole creation back so the history
  // never records a partially constructed entity (issue #117).
  bool ok = world->add_name_component(entity, name);
  if (ok && hasMesh) {
    ok = world->add_mesh_component(entity, mesh);
  }
  if (ok && hasCollider) {
    ok = world->add_collider(entity, colliderComponent);
  }
  if (!ok) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity create command rolled back a partial entity");
    static_cast<void>(world->destroy_entity(entity));
    return false;
  }
  return true;
}

bool EntityCreateCommand::undo() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) ||
      (persistentId == runtime::kInvalidPersistentId)) {
    return false;
  }
  const runtime::Entity entity =
      world->find_entity_by_persistent_id(persistentId);
  if (entity == runtime::kInvalidEntity) {
    return false;
  }
  return world->destroy_entity(entity);
}

/// Collects the entity's transform subtree into members (root first,
/// every parent before its children) with an iterative breadth-first
/// frontier and per-index visited marks, so corrupted parent links
/// (cycles, self-parenting) are captured once and 1000+-deep chains
/// cannot grow the call stack; returns the member count (bounded by
/// capacity). Builds a single parentId -> children index up front (one
/// world.for_each_alive pass) instead of rescanning every alive entity
/// per BFS member, so a subtree of S members in a world of N entities
/// costs O(N log N + S log N) rather than the former O(S * N) (issue
/// #86 L-01); the per-parent child order is unchanged (ascending entity
/// index), so member order is identical to the prior full-scan walk.
static std::size_t collect_subtree_members(runtime::World &world,
                                           runtime::Entity root,
                                           runtime::Entity *members,
                                           std::size_t capacity,
                                           bool *visited) noexcept {
  if ((members == nullptr) || (visited == nullptr) || (capacity == 0U) ||
      !world.is_alive(root)) {
    return 0U;
  }

  // Cold editor path (not per-frame): a heap-allocated index is fine here,
  // unlike the hot-path ECS/physics/render-prep code CLAUDE.md restricts.
  std::vector<std::pair<runtime::PersistentId, runtime::Entity>> byParent;
  world.for_each_alive([&](runtime::Entity candidate) {
    runtime::Transform transform{};
    if (world.get_transform(candidate, &transform) &&
        (transform.parentId != runtime::kInvalidPersistentId)) {
      byParent.emplace_back(transform.parentId, candidate);
    }
  });
  std::sort(byParent.begin(), byParent.end(),
           [](const auto &lhs, const auto &rhs) noexcept {
             if (lhs.first != rhs.first) {
               return lhs.first < rhs.first;
             }
             return lhs.second.index < rhs.second.index;
           });

  std::size_t count = 0U;
  members[count++] = root;
  visited[root.index] = true;
  for (std::size_t cursor = 0U; cursor < count; ++cursor) {
    const runtime::PersistentId ownId = world.persistent_id(members[cursor]);
    if (ownId == runtime::kInvalidPersistentId) {
      continue;
    }
    const auto range = std::equal_range(
        byParent.begin(), byParent.end(),
        std::pair<runtime::PersistentId, runtime::Entity>{ownId, {}},
        [](const auto &lhs, const auto &rhs) noexcept {
          return lhs.first < rhs.first;
        });
    for (auto it = range.first; (it != range.second) && (count < capacity);
        ++it) {
      const runtime::Entity candidate = it->second;
      if (!visited[candidate.index]) {
        visited[candidate.index] = true;
        members[count++] = candidate;
      }
    }
  }
  return count;
}

bool EntityDeleteCommand::execute() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (recordCount == 0U)) {
    return false;
  }
  const runtime::Entity root =
      world->find_entity_by_persistent_id(records[0].persistentId);
  if (root == runtime::kInvalidEntity) {
    return false;
  }
  return world->destroy_entity(root);
}

bool EntityDeleteCommand::undo() noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  constexpr std::size_t transformSlot =
      static_cast<std::size_t>(ComponentEditType::Transform);
  // All-or-nothing restore: `restored` tracks how many members exist so a
  // mid-subtree failure (entity or component capacity, stale ids) can
  // destroy exactly what this undo created and report failure with the
  // world back in its pre-undo state (issue #117).
  std::size_t restored = 0U;
  bool ok = true;
  for (std::size_t i = 0U; ok && (i < recordCount); ++i) {
    const EntityDeleteRecord &record = records[i];
    const runtime::Entity entity =
        record.present[transformSlot]
            ? world->create_scene_object_with_persistent_id(
                  record.persistentId, record.components.transform)
            : world->create_entity_with_persistent_id(record.persistentId);
    if (entity == runtime::kInvalidEntity) {
      ok = false;
      break;
    }
    ++restored;
    for (std::size_t typeIndex = 0U; typeIndex < kComponentEditTypeCount;
         ++typeIndex) {
      if (!record.present[typeIndex] || (typeIndex == transformSlot)) {
        continue;
      }
      if (!apply_component_snapshot(
              static_cast<ComponentEditType>(typeIndex), entity, true,
              record.components)) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    // Children before parents: destroy_entity takes whole transform
    // subtrees, so reverse order never double-frees a member.
    for (std::size_t i = restored; i > 0U; --i) {
      const runtime::Entity member =
          world->find_entity_by_persistent_id(records[i - 1U].persistentId);
      if (member != runtime::kInvalidEntity) {
        static_cast<void>(world->destroy_entity(member));
      }
    }
    core::log_message(
        core::LogLevel::Error, "editor",
        "entity delete undo could not restore the subtree — rolled back");
    return false;
  }
  return true;
}

runtime::Entity execute_entity_create() noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }
  auto *command = new (std::nothrow) EntityCreateCommand();
  if (command == nullptr) {
    const runtime::Entity entity = world->create_scene_object();
    if (entity != runtime::kInvalidEntity) {
      runtime::NameComponent nameComponent{};
      make_default_entity_name(entity.index, &nameComponent);
      static_cast<void>(world->add_name_component(entity, nameComponent));
    }
    return entity;
  }
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->persistentId);
}

/// Copies the file stem of a virtual asset path into a name component
/// ("assets/props/rock.mesh" names the spawn "rock").
static void make_asset_spawn_name(const char *virtualPath,
                                  runtime::NameComponent *outName) noexcept {
  if ((virtualPath == nullptr) || (outName == nullptr)) {
    return;
  }
  const char *stem = virtualPath;
  for (const char *cursor = virtualPath; *cursor != '\0'; ++cursor) {
    if ((*cursor == '/') || (*cursor == '\\')) {
      stem = cursor + 1;
    }
  }
  const int written =
      std::snprintf(outName->name, sizeof(outName->name), "%s", stem);
  // A long asset filename still spawns (naming is cosmetic, not fatal) but
  // must not clip into NameComponent::kMaxNameLength silently (issue #86
  // L-07): surface it once so the author can see why the entity name was
  // shortened instead of it just quietly not matching the file.
  if ((written < 0) || (static_cast<std::size_t>(written) >= sizeof(outName->name))) {
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "asset spawn name truncated to %zu chars (source: %s)",
                  sizeof(outName->name) - 1U, virtualPath);
    core::log_message(core::LogLevel::Warning, "editor", message);
  }
  char *dot = std::strrchr(outName->name, '.');
  if ((dot != nullptr) && (dot != outName->name)) {
    *dot = '\0';
  }
}

runtime::Entity execute_asset_spawn(
    const char *virtualPath, const runtime::Transform &transform) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return runtime::kInvalidEntity;
  }

  const std::uint64_t assetId =
      runtime::editor_request_mesh_asset(virtualPath);
  if (assetId == 0ULL) {
    return runtime::kInvalidEntity;
  }

  auto *command = new (std::nothrow) EntityCreateCommand();
  if (command == nullptr) {
    const runtime::Entity entity = world->create_scene_object(transform);
    if (entity != runtime::kInvalidEntity) {
      runtime::NameComponent nameComponent{};
      make_asset_spawn_name(virtualPath, &nameComponent);
      static_cast<void>(world->add_name_component(entity, nameComponent));
      runtime::MeshComponent meshComponent{};
      meshComponent.meshAssetId = assetId;
      static_cast<void>(world->add_mesh_component(entity, meshComponent));
    }
    return entity;
  }

  command->transform = transform;
  make_asset_spawn_name(virtualPath, &command->name);
  command->hasMesh = true;
  command->mesh.meshAssetId = assetId;
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->persistentId);
}

/// Per-primitive spawn description: display name, builtin mesh path,
/// resting height, fallback collider, and hull provenance.
struct PrimitiveSpawnDesc final {
  const char *name = nullptr;
  const char *builtinPath = nullptr;
  float groundY = 0.5F;
  math::ColliderShape fallbackShape = math::ColliderShape::AABB;
  math::Vec3 halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  math::Vec3 colliderLocalPosition = math::Vec3(0.0F, 0.0F, 0.0F);
  math::HullSource hullSource = math::HullSource::None;
};

/// Returns the spawn description for a built-in blockout primitive.
static PrimitiveSpawnDesc primitive_spawn_desc(
    EditorPrimitive primitive) noexcept {
  PrimitiveSpawnDesc desc{};
  switch (primitive) {
  case EditorPrimitive::Cube:
    desc.name = "Cube";
    desc.builtinPath = "builtin://cube";
    break;
  case EditorPrimitive::Sphere:
    desc.name = "Sphere";
    desc.builtinPath = "builtin://sphere";
    desc.fallbackShape = math::ColliderShape::Sphere;
    break;
  case EditorPrimitive::Cylinder:
    desc.name = "Cylinder";
    desc.builtinPath = "builtin://cylinder";
    desc.fallbackShape = math::ColliderShape::Capsule;
    desc.hullSource = math::HullSource::Cylinder;
    break;
  case EditorPrimitive::Capsule:
    desc.name = "Capsule";
    desc.builtinPath = "builtin://capsule";
    desc.groundY = 1.0F;
    desc.fallbackShape = math::ColliderShape::Capsule;
    break;
  case EditorPrimitive::Pyramid:
    desc.name = "Pyramid";
    desc.builtinPath = "builtin://pyramid";
    desc.halfExtents = math::Vec3(0.5F, 0.5F, 0.58F);
    desc.hullSource = math::HullSource::Pyramid;
    break;
  case EditorPrimitive::Plane:
    desc.name = "Plane";
    desc.builtinPath = "builtin://plane";
    desc.groundY = -0.5F;
    desc.halfExtents = math::Vec3(5.0F, 0.1F, 5.0F);
    desc.colliderLocalPosition = math::Vec3(0.0F, 0.4F, 0.0F);
    break;
  }
  return desc;
}

runtime::Entity execute_primitive_spawn(EditorPrimitive primitive) noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }

  const PrimitiveSpawnDesc desc = primitive_spawn_desc(primitive);
  if ((desc.name == nullptr) || (desc.builtinPath == nullptr)) {
    return runtime::kInvalidEntity;
  }

  auto *command = new (std::nothrow) EntityCreateCommand();
  if (command == nullptr) {
    return runtime::kInvalidEntity;
  }

  const renderer::CameraState cam =
      editor_camera_state(editor_session().editorCamera);
  command->transform.position =
      math::Vec3(cam.target.x, desc.groundY, cam.target.z);
  std::snprintf(command->name.name, sizeof(command->name.name), "%s",
                desc.name);
  command->hasMesh = true;
  command->mesh.meshAssetId =
      renderer::make_asset_id_from_path(desc.builtinPath);
  command->hasCollider = true;
  command->colliderComponent.shape = desc.fallbackShape;
  command->colliderComponent.halfExtents = desc.halfExtents;
  command->colliderComponent.localPosition = desc.colliderLocalPosition;
  physics::ConvexHullData hull{};
  bool hullBuilt = false;
  if (desc.hullSource == math::HullSource::Cylinder) {
    hullBuilt = physics::build_cylinder_hull(&hull);
  } else if (desc.hullSource == math::HullSource::Pyramid) {
    hullBuilt = physics::build_pyramid_hull(&hull);
  }
  if (hullBuilt) {
    command->colliderComponent.shape = math::ColliderShape::ConvexHull;
    command->colliderComponent.hullSource = desc.hullSource;
    command->colliderComponent.halfExtents = hull.localHalfExtents;
  }
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->persistentId);
}

EntityDeleteCommand *
build_entity_delete_command(runtime::Entity entity) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world->is_alive(entity)) {
    return nullptr;
  }
  const std::size_t capacity = world->alive_entity_count();
  std::unique_ptr<runtime::Entity[]> members(
      new (std::nothrow) runtime::Entity[capacity]);
  std::unique_ptr<bool[]> visited(
      new (std::nothrow) bool[runtime::World::kMaxEntities + 1U]());
  if ((members == nullptr) || (visited == nullptr)) {
    return nullptr;
  }
  const std::size_t count = collect_subtree_members(
      *world, entity, members.get(), capacity, visited.get());
  if (count == 0U) {
    return nullptr;
  }
  auto *command = new (std::nothrow) EntityDeleteCommand();
  if (command == nullptr) {
    return nullptr;
  }
  command->records.reset(new (std::nothrow) EntityDeleteRecord[count]);
  if (command->records == nullptr) {
    delete command;
    return nullptr;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    EntityDeleteRecord &record = command->records[i];
    record.persistentId = world->persistent_id(members[i]);
    for (std::size_t typeIndex = 0U; typeIndex < kComponentEditTypeCount;
         ++typeIndex) {
      record.present[typeIndex] = capture_component_snapshot(
          static_cast<ComponentEditType>(typeIndex), members[i],
          &record.components);
    }
  }
  command->recordCount = count;
  return command;
}

bool execute_entity_delete(runtime::Entity entity) noexcept {
  inspector_commit_pending_edit();
  EntityDeleteCommand *const command = build_entity_delete_command(entity);
  if (command == nullptr) {
    return (editor_session().world != nullptr) &&
           editor_session().world->destroy_entity(entity);
  }
  return editor_session().commandHistory.execute(command);
}

ComponentEditSnapshot default_component_snapshot(
    runtime::Entity entity, ComponentEditType type) noexcept {
  ComponentEditSnapshot snapshot{};
  switch (type) {
  case ComponentEditType::Name:
    make_default_entity_name(entity.index, &snapshot.name);
    break;
  case ComponentEditType::RigidBody:
    snapshot.rigidBody.inverseMass = 1.0F;
    break;
  case ComponentEditType::Collider:
    snapshot.collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    break;
  case ComponentEditType::Mesh:
    snapshot.mesh.albedo = math::Vec3(1.0F, 1.0F, 1.0F);
    break;
  case ComponentEditType::FoliagePatch: {
    snapshot.foliagePatch.instanceCount = 16U;
    snapshot.foliagePatch.density = 1.0F;
    snapshot.foliagePatch.albedo = math::Vec3(0.22F, 0.62F, 0.24F);
    runtime::MeshComponent sourceMesh{};
    if ((editor_session().world != nullptr) &&
        editor_session().world->get_mesh_component(entity, &sourceMesh)) {
      snapshot.foliagePatch.meshAssetIds[0] = sourceMesh.meshAssetId;
      snapshot.foliagePatch.meshAssetIds[1] = sourceMesh.meshAssetId;
    }
    for (std::uint32_t i = 0U; i < snapshot.foliagePatch.instanceCount; ++i) {
      const std::uint32_t x = i % 4U;
      const std::uint32_t z = i / 4U;
      runtime::FoliageInstance &instance =
          snapshot.foliagePatch.instances[i];
      instance.offset = math::Vec3((static_cast<float>(x) - 1.5F) * 0.9F,
                                   0.0F,
                                   (static_cast<float>(z) - 1.5F) * 0.9F);
      instance.scale = 0.55F + (static_cast<float>(i % 3U) * 0.08F);
      instance.phase = static_cast<float>(i) * 0.41F;
      instance.lodIndex = (i >= 12U) ? 1U : 0U;
    }
    break;
  }
  case ComponentEditType::Transform:
  case ComponentEditType::Light:
  case ComponentEditType::Script:
  case ComponentEditType::ReflectionProbe:
  case ComponentEditType::PointLight:
  case ComponentEditType::SpotLight:
  case ComponentEditType::SpringArm:
  case ComponentEditType::SceneCapture:
  case ComponentEditType::Animation:
    break;
  }
  return snapshot;
}


void make_default_entity_name(std::uint32_t entityIndex,
                              runtime::NameComponent *outName) noexcept {
  if (outName == nullptr) {
    return;
  }

  std::snprintf(outName->name, sizeof(outName->name), "Entity_%u", entityIndex);
  outName->name[sizeof(outName->name) - 1U] = '\0';
}


} // namespace engine::editor
