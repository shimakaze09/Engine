// Implements the editor's undoable component, reparent and gizmo edit
// commands, the Inspector's pending-edit gesture and the command
// allocation seam tests inject failures through. Entity create, delete
// and duplicate live in editor_entity_commands.cpp.

#include "editor_commands.h"

#include "editor_material_edit.h"
#include "editor_transform_util.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "engine/content/asset_metadata.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/primitive_collider.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"

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

/// Outstanding injected allocation failures.
static std::size_t g_injectedAllocationFailures = 0U;

bool editor_command_allocation_allowed() noexcept {
  if (g_injectedAllocationFailures > 0U) {
    --g_injectedAllocationFailures;
    return false;
  }
  return true;
}

void editor_commands_inject_allocation_failures(std::size_t count) noexcept {
  g_injectedAllocationFailures = count;
}

/// Open viewport gizmo drag: the target's identity and pre-drag transform.
struct GizmoGesture final {
  bool active = false;
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  runtime::Transform start{};
};

/// Process-wide gizmo gesture behind gizmo_track_gesture.
static GizmoGesture g_gizmoGesture{};

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
  if (type == ComponentEditType::RigidBody) {
    // Typing a tensor is authoring it; unticking "authored" hands it back
    // to the derivation.
    const math::Vec3 &was = before.rigidBody.inverseInertia;
    const math::Vec3 &now = after->rigidBody.inverseInertia;
    if ((was.x != now.x) || (was.y != now.y) || (was.z != now.z)) {
      after->rigidBody.inertiaAuthored = true;
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
  auto *cmd = allocate_command<ComponentEditCommand>();
  if (cmd == nullptr) {
    // The gesture already reached the world frame by frame; without a
    // command the history cannot account for it, so the document tracks
    // it as dirty by hand and says so.
    editor_session().document.unrecordedEdit = true;
    core::log_message(core::LogLevel::Error, "editor",
                      "inspector edit could not be recorded for undo (out "
                      "of memory); the scene stays unsaved until saved or "
                      "reloaded");
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

  auto *cmd = allocate_command<ComponentEditCommand>();
  if (cmd == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "component add refused: it could not be recorded for "
                      "undo (out of memory)");
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

  auto *cmd = allocate_command<ComponentEditCommand>();
  if (cmd == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "component remove refused: it could not be recorded "
                      "for undo (out of memory)");
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

/// Writes `transform` (parent included) onto the child.
static bool
apply_child_transform(runtime::Entity child,
                      const runtime::Transform &transform) noexcept {
  runtime::World *world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity resolved = world->find_entity_by_index(child.index);
  if ((resolved == runtime::kInvalidEntity) ||
      (resolved.generation != child.generation)) {
    return false;
  }
  return world->add_transform(resolved, transform);
}

bool ReparentCommand::execute() noexcept {
  return apply_child_transform(resolve_command_target(child, childPersistentId),
                               after);
}

bool ReparentCommand::undo() noexcept {
  return apply_child_transform(resolve_command_target(child, childPersistentId),
                               before);
}

namespace {

/// The child's local transform under `newParent` (kInvalidEntity for the
/// root) that keeps its world pose. False when a matrix cannot be
/// inverted or decomposed (a zero scale).
bool local_keeping_world_pose(const runtime::World &world,
                              runtime::Entity child, runtime::Entity newParent,
                              const runtime::Transform &before,
                              runtime::Transform *out) noexcept {
  physics::PhysicsTransform childWorld{};
  if (!world.get_physics_transform(child, &childWorld)) {
    return false;
  }
  physics::PhysicsTransform parentWorld{};
  const bool hasParent = newParent != runtime::kInvalidEntity;
  if (hasParent && !world.get_physics_transform(newParent, &parentWorld)) {
    return false;
  }
  if (!local_transform_keeping_world_pose(
          childWorld, hasParent ? &parentWorld : nullptr, before, out)) {
    return false;
  }
  // A non-uniform scale on a rotated parent shears the child, which no
  // local TRS can hold: the decomposition keeps position and rotation and
  // approximates the scale, and the author is told.
  const math::Mat4 local =
      math::compose_trs(out->position, out->rotation, out->scale);
  const math::Mat4 recomposed =
      hasParent ? math::mul(parentWorld.matrix, local) : local;
  float largest = 0.0F;
  float deviation = 0.0F;
  for (int c = 0; c < 3; ++c) {
    const math::Vec4 &a = childWorld.matrix.columns[c];
    const math::Vec4 &b = recomposed.columns[c];
    largest =
        std::max({largest, std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)});
    deviation = std::max({deviation, std::fabs(a.x - b.x), std::fabs(a.y - b.y),
                          std::fabs(a.z - b.z)});
  }
  // float32 round-off of an inverse, a product and a decomposition stays
  // within a few hundred ulps of the basis length; shear is far larger.
  if (deviation > (largest * 1.0e-4F)) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "reparent: the new parent's non-uniform scale shears "
                      "the child, which a local transform cannot hold; its "
                      "position and rotation are kept, its scale is "
                      "approximate");
  }
  return true;
}

} // namespace

bool execute_reparent(runtime::Entity child,
                      runtime::Entity newParent) noexcept {
  runtime::World *world = editor_session().world;
  if ((world == nullptr) || (child == runtime::kInvalidEntity) ||
      (child == newParent)) {
    return false;
  }
  inspector_commit_pending_edit();

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

  runtime::Transform after{};
  if (!local_keeping_world_pose(*world, child, newParent, before, &after)) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "reparent refused: the child's place in the world "
                      "cannot be expressed under the new parent (a zero "
                      "scale)");
    return false;
  }
  after.parentId = afterId;

  // Prove the reparent is legal (add_transform enforces the
  // dynamic-body-root rule) before recording it, then revert and route
  // the real application through the command history.
  if (!apply_child_transform(child, after)) {
    return false;
  }
  runtime::Transform applied{};
  if (!world->get_transform(child, &applied) ||
      (applied.parentId != afterId)) {
    return false;
  }
  static_cast<void>(apply_child_transform(child, before));

  auto *command = allocate_command<ReparentCommand>();
  if (command == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "reparent refused: it could not be recorded for undo "
                      "(out of memory)");
    return false;
  }
  command->child = child;
  command->childPersistentId = world->persistent_id(child);
  command->before = before;
  command->after = after;
  return editor_session().commandHistory.execute(command);
}

bool execute_transform_edit(runtime::Entity entity,
                            const runtime::Transform &before,
                            const runtime::Transform &after) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }
  inspector_commit_pending_edit();
  auto *cmd = allocate_command<TransformEditCommand>();
  if (cmd == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "transform edit refused: it could not be recorded for "
                      "undo (out of memory)");
    return false;
  }
  cmd->entity = entity;
  cmd->persistentId = world->persistent_id(entity);
  cmd->oldTransform = before;
  cmd->newTransform = after;
  return editor_session().commandHistory.execute(cmd);
}

void gizmo_commit_gesture() noexcept {
  GizmoGesture &gesture = g_gizmoGesture;
  if (!gesture.active) {
    return;
  }
  gesture.active = false;
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return;
  }
  const runtime::Entity target =
      resolve_command_target(gesture.entity, gesture.persistentId);
  runtime::Transform current{};
  if (!world->is_alive(target) || !world->get_transform(target, &current)) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "gizmo edit dropped: its target no longer exists");
    return;
  }
  // The drag wrote the world frame by frame; recording the gesture
  // against the recorded target is what makes it undoable. A refused
  // record leaves the applied drag as an unrecorded edit.
  if (!execute_transform_edit(target, gesture.start, current)) {
    editor_session().document.unrecordedEdit = true;
  }
}

void gizmo_track_gesture(runtime::Entity target, bool manipulating,
                         const runtime::Transform &current) noexcept {
  GizmoGesture &gesture = g_gizmoGesture;
  if (gesture.active && (!manipulating || (target != gesture.entity))) {
    gizmo_commit_gesture();
  }
  if (manipulating && !gesture.active) {
    runtime::World *const world = editor_session().world;
    if ((world == nullptr) || !world->is_alive(target)) {
      return;
    }
    gesture.active = true;
    gesture.entity = target;
    gesture.persistentId = world->persistent_id(target);
    gesture.start = current;
  }
}

void gizmo_abandon_gesture() noexcept { g_gizmoGesture.active = false; }

bool gizmo_has_gesture() noexcept { return g_gizmoGesture.active; }

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
      snapshot.foliagePatch.meshRefs[0] = sourceMesh.meshRef;
      snapshot.foliagePatch.meshRefs[1] = sourceMesh.meshRef;
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
  case ComponentEditType::Camera:
  case ComponentEditType::SkyLight:
  case ComponentEditType::Tags:
  case ComponentEditType::CharacterController:
  case ComponentEditType::NavMeshSurface:
  case ComponentEditType::NavAgent:
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
