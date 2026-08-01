// Declares the editor's undoable edit commands and component snapshot
// helpers shared by the inspector, hierarchy, and viewport panels.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "editor_session.h"

namespace engine::editor {

/// Resolves a command's target: by persistent id when one was captured
/// (so undo/redo survives the entity being deleted and re-created), else
/// the recorded handle (stale generations are rejected downstream).
runtime::Entity resolve_command_target(
    runtime::Entity entity, runtime::PersistentId persistentId) noexcept;

/// Undoable transform edit stored as before/after local TRS values.
struct TransformEditCommand final : EditorCommand {
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  runtime::Transform oldTransform{};
  runtime::Transform newTransform{};

  void execute() noexcept override {
    if (editor_session().world != nullptr) {
      static_cast<void>(editor_session().world->add_transform(
          resolve_command_target(entity, persistentId), newTransform));
    }
  }

  void undo() noexcept override {
    if (editor_session().world != nullptr) {
      static_cast<void>(editor_session().world->add_transform(
          resolve_command_target(entity, persistentId), oldTransform));
    }
  }
};

/// Enumerates component edit type values used by the engine.
enum class ComponentEditType : std::uint8_t {
  Name,
  Transform,
  RigidBody,
  Collider,
  Light,
  Mesh,
  FoliagePatch,
  Script,
  ReflectionProbe,
  PointLight,
  SpotLight,
  SpringArm,
  SceneCapture,
  Animation,
};

/// Number of ComponentEditType values (used to size per-type flag arrays).
inline constexpr std::size_t kComponentEditTypeCount = 14U;

/// Union-of-components value captured before/after an inspector edit.
struct ComponentEditSnapshot final {
  runtime::NameComponent name{};
  runtime::Transform transform{};
  runtime::RigidBody rigidBody{};
  runtime::Collider collider{};
  runtime::LightComponent light{};
  runtime::MeshComponent mesh{};
  runtime::FoliagePatchComponent foliagePatch{};
  runtime::ScriptComponent script{};
  runtime::ReflectionProbeComponent reflectionProbe{};
  runtime::PointLightComponent pointLight{};
  runtime::SpotLightComponent spotLight{};
  runtime::SpringArmComponent springArm{};
  runtime::SceneCaptureComponent sceneCapture{};
  runtime::AnimationComponent animation{};
};

/// Fills a snapshot from the entity's current component of `type`; false
/// when the world is unbound or the component is absent.
bool capture_component_snapshot(ComponentEditType type, runtime::Entity entity,
                                ComponentEditSnapshot *out) noexcept;
/// Applies (or removes, when !exists) the snapshotted component of `type`;
/// false when the exact entity generation is no longer alive.
bool apply_component_snapshot(ComponentEditType type, runtime::Entity entity,
                              bool exists,
                              const ComponentEditSnapshot &snapshot) noexcept;
/// Adds the component of `type` with `after`'s values through the command
/// history so the edit is undoable.
void execute_component_add(runtime::Entity entity, ComponentEditType type,
                           const ComponentEditSnapshot &after) noexcept;
/// Removes the component of `type` through the command history.
void execute_component_remove(runtime::Entity entity,
                              ComponentEditType type) noexcept;

/// Undoable add/remove/edit of one component, stored as before/after
/// snapshots.
struct ComponentEditCommand final : EditorCommand {
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  ComponentEditType type = ComponentEditType::Transform;
  bool beforeExists = false;
  bool afterExists = false;
  ComponentEditSnapshot before{};
  ComponentEditSnapshot after{};

  void execute() noexcept override {
    static_cast<void>(apply_component_snapshot(
        type, resolve_command_target(entity, persistentId), afterExists,
        after));
  }

  void undo() noexcept override {
    static_cast<void>(apply_component_snapshot(
        type, resolve_command_target(entity, persistentId), beforeExists,
        before));
  }
};

/// Undoable reparent: rewrites the child transform's parent persistent
/// id (add_transform enforces the dynamic-body-root rule, so an invalid
/// reparent simply fails and the command records no change).
struct ReparentCommand final : EditorCommand {
  runtime::Entity child{};
  runtime::PersistentId childPersistentId = runtime::kInvalidPersistentId;
  runtime::PersistentId beforeParentId = runtime::kInvalidPersistentId;
  runtime::PersistentId afterParentId = runtime::kInvalidPersistentId;

  void execute() noexcept override;
  void undo() noexcept override;
};

/// Reparents through the command history; false when the child has no
/// transform, the parent is invalid, or the reparent would cycle.
bool execute_reparent(runtime::Entity child,
                      runtime::Entity newParent) noexcept;

/// Undoable scene-object creation (optionally carrying an initial mesh
/// and collider for asset/primitive spawns); redo re-creates the entity
/// under its original persistent id so later history entries keep
/// resolving. Hull payloads ride Collider::hullSource — World::add_collider
/// rebuilds them on every execute, so the command carries no hull state.
struct EntityCreateCommand final : EditorCommand {
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  runtime::NameComponent name{};
  runtime::Transform transform{};
  bool hasMesh = false;
  runtime::MeshComponent mesh{};
  bool hasCollider = false;
  runtime::Collider colliderComponent{};

  void execute() noexcept override;
  void undo() noexcept override;
};

/// One captured subtree member of a deleted entity: its persistent id
/// plus every component present at delete time.
struct EntityDeleteRecord final {
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  std::array<bool, kComponentEditTypeCount> present{};
  ComponentEditSnapshot components{};
};

/// Undoable entity deletion backed by parent-before-child subtree records;
/// undo re-creates every member under its original persistent id so parent
/// links and cross-references survive the round trip.
struct EntityDeleteCommand final : EditorCommand {
  std::unique_ptr<EntityDeleteRecord[]> records{};
  std::size_t recordCount = 0U;

  void execute() noexcept override;
  void undo() noexcept override;
};

/// Creates a scene object with a default name through the command history;
/// returns the new entity (kInvalidEntity on failure).
runtime::Entity execute_entity_create() noexcept;
/// Spawns a scene object at `transform` referencing the mesh asset behind
/// `virtualPath` (requesting its async load) through the command history;
/// returns the new entity (kInvalidEntity on failure).
runtime::Entity execute_asset_spawn(const char *virtualPath,
                                    const runtime::Transform &transform) noexcept;

/// Enumerates the built-in blockout primitives the editor can spawn.
enum class EditorPrimitive : std::uint8_t {
  Cube,
  Sphere,
  Cylinder,
  Capsule,
  Pyramid,
  Plane,
};

/// Spawns the built-in primitive as an undoable scene object (mesh plus
/// the matching collider; cylinder/pyramid hull payloads are rebuilt on
/// every execute) resting on the ground plane at the editor camera's
/// focus point; returns the new entity (kInvalidEntity on failure).
runtime::Entity execute_primitive_spawn(EditorPrimitive primitive) noexcept;
/// Captures the entity's transform subtree into a delete command; null on
/// allocation failure or when the entity is not alive.
EntityDeleteCommand *
build_entity_delete_command(runtime::Entity entity) noexcept;
/// Deletes the entity subtree through the command history (falling back to
/// a plain non-undoable destroy when the capture cannot be allocated).
bool execute_entity_delete(runtime::Entity entity) noexcept;

/// Returns the default-valued snapshot used when adding a component.
ComponentEditSnapshot default_component_snapshot(
    runtime::Entity entity, ComponentEditType type) noexcept;
/// Formats the default display name for a newly created entity.
void make_default_entity_name(std::uint32_t entityIndex,
                              runtime::NameComponent *outName) noexcept;

} // namespace engine::editor
