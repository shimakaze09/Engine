// Declares the editor's undoable edit commands and component snapshot
// helpers shared by the inspector, hierarchy, and viewport panels.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "editor_component_registry.h"
#include "editor_session.h"

namespace engine::editor {

/// Resolves a command's target: by persistent id when one was captured
/// (so undo/redo survives the entity being deleted and re-created) with no
/// raw-handle fallback, because generations reset when a scene load
/// replaces the world's contents; a handle-only command (no id captured)
/// keeps its recorded handle and stale generations are rejected downstream.
runtime::Entity resolve_command_target(
    runtime::Entity entity, runtime::PersistentId persistentId) noexcept;

/// Undoable transform edit stored as before/after local TRS values.
struct TransformEditCommand final : EditorCommand {
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  runtime::Transform oldTransform{};
  runtime::Transform newTransform{};

  bool execute() noexcept override {
    return (editor_session().world != nullptr) &&
           editor_session().world->add_transform(
               resolve_command_target(entity, persistentId), newTransform);
  }

  bool undo() noexcept override {
    return (editor_session().world != nullptr) &&
           editor_session().world->add_transform(
               resolve_command_target(entity, persistentId), oldTransform);
  }
};

// ComponentEditType, ComponentEditSnapshot, capture_component_snapshot, and
// apply_component_snapshot are generated from the persistent-component
// registry in editor_component_registry.h so a new registry row
// automatically gains an inspector edit slot instead of requiring a matching
// hand-written branch in every one of these switches.

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

  bool execute() noexcept override { return apply_state(afterExists, after); }

  bool undo() noexcept override { return apply_state(beforeExists, before); }

private:
  /// Applies one endpoint of the edit. Removing a component that is
  /// already absent on the live target counts as applied — the desired
  /// end state holds (e.g. undoing an add after the entity was re-created
  /// without the component).
  bool apply_state(bool exists,
                   const ComponentEditSnapshot &snapshot) noexcept {
    const runtime::Entity target =
        resolve_command_target(entity, persistentId);
    if (apply_component_snapshot(type, target, exists, snapshot)) {
      return true;
    }
    if (exists || (editor_session().world == nullptr) ||
        !editor_session().world->is_alive(target)) {
      return false;
    }
    ComponentEditSnapshot current{};
    return !capture_component_snapshot(type, target, &current);
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

  bool execute() noexcept override;
  bool undo() noexcept override;
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

  bool execute() noexcept override;
  bool undo() noexcept override;
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
/// links and cross-references survive the round trip. The restore is
/// transactional: when any member or component cannot be re-created, every
/// member restored so far is destroyed again and undo reports failure.
struct EntityDeleteCommand final : EditorCommand {
  std::unique_ptr<EntityDeleteRecord[]> records{};
  std::size_t recordCount = 0U;

  bool execute() noexcept override;
  bool undo() noexcept override;
};

/// One member of a duplicated subtree: the components copied from the
/// source member, the persistent id the copy was given on its first
/// execute (so redo re-creates it under the same id, and later history
/// entries keep resolving), and which record holds its parent copy.
struct EntityDuplicateRecord final {
  static constexpr std::size_t kNoParentRecord = static_cast<std::size_t>(-1);

  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  /// Index of this member's parent within the same command; kNoParentRecord
  /// for the duplicated root, whose parent link is copied verbatim and so
  /// still names an entity outside the copy.
  std::size_t parentRecord = kNoParentRecord;
  std::array<bool, kComponentEditTypeCount> present{};
  ComponentEditSnapshot components{};
};

/// Undoable duplication of an entity's transform subtree: every
/// persistent component is copied through the registry, the copies get
/// fresh persistent ids, internal parent links are remapped onto the
/// copies (the root keeps the source's parent) and the root copy gets a
/// name no other entity holds. Parents are created before children, and a
/// member or component that cannot be created destroys what this execute
/// made and reports failure, so history never records a partial copy.
struct EntityDuplicateCommand final : EditorCommand {
  std::unique_ptr<EntityDuplicateRecord[]> records{};
  std::size_t recordCount = 0U;

  bool execute() noexcept override;
  bool undo() noexcept override;
};

/// Captures the entity's transform subtree into a duplicate command; null
/// on allocation failure or when the entity is not alive.
EntityDuplicateCommand *
build_entity_duplicate_command(runtime::Entity entity) noexcept;
/// Duplicates the entity subtree through the command history; returns the
/// root copy (kInvalidEntity on failure).
runtime::Entity execute_entity_duplicate(runtime::Entity entity) noexcept;

/// Creates a scene object with a default name through the command history;
/// returns the new entity (kInvalidEntity on failure).
runtime::Entity execute_entity_create() noexcept;
/// Spawns a scene object at `transform` referencing the mesh asset behind
/// `virtualPath` (requesting its async load) through the command history;
/// returns the new entity (kInvalidEntity on failure).
runtime::Entity execute_asset_spawn(const char *virtualPath,
                                    const runtime::Transform &transform) noexcept;

/// Dispatches the content browser's typed double-click/Open action for
/// `entry` through its production entry point: a mesh spawns at the
/// editor camera's focus point (mirroring execute_primitive_spawn's
/// placement) and becomes the selection; a scene routes through the
/// gated open flow (request_scene_open), which may defer behind the
/// unsaved-change prompt instead of switching immediately; every other
/// kind only updates the browser selection since no dedicated editor
/// exists yet for it. Always updates selectedAssetPath. Returns false
/// only when a SpawnMesh dispatch's actual spawn failed.
bool execute_asset_open(const AssetIndexEntry &entry) noexcept;

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

/// Applies an inspector field edit through World validation immediately
/// and folds it into the pending edit gesture (opened on the first change,
/// keyed by target entity + component type; a different key commits the
/// previous gesture first). Sanitizes state that direct writes could
/// corrupt (a changed animation controller path resets the cached
/// controller binding; transform rotations are renormalized). Returns
/// false when the world rejected the value.
bool inspector_stage_component_edit(
    runtime::Entity entity, ComponentEditType type,
    const ComponentEditSnapshot &before,
    const ComponentEditSnapshot &after) noexcept;
/// Commits the pending edit gesture (if any) to the command history as one
/// undoable command spanning the gesture's opening snapshot to the
/// entity's current component value.
void inspector_commit_pending_edit() noexcept;
/// Drops the pending edit gesture without recording it (world rebind or
/// scene replacement invalidates the captured target).
void inspector_abandon_pending_edit() noexcept;
/// True when an inspector edit gesture is pending.
bool inspector_has_pending_edit() noexcept;

/// Records a completed direct transform write (the viewport gizmo drag)
/// as one undoable command from `before` to `after`; a pending inspector
/// gesture is committed first so no command ever executes across an open
/// gesture. False when the world is unbound or the command could
/// not be recorded, in which case nothing is applied.
bool execute_transform_edit(runtime::Entity entity,
                            const runtime::Transform &before,
                            const runtime::Transform &after) noexcept;

/// Viewport gizmo gesture, fed once per frame with the entity under the
/// gizmo, whether ImGuizmo is manipulating it, and that entity's local
/// transform before this frame's manipulation is applied. Opens on the
/// first manipulating frame, recording the target identity with its
/// pre-drag transform; closes on the first non-manipulating frame (or a
/// target change) as one transform edit against the recorded target, so
/// a start transform can never pair with whatever entity is selected at
/// release.
void gizmo_track_gesture(runtime::Entity target, bool manipulating,
                         const runtime::Transform &current) noexcept;
/// Closes an open gizmo gesture now, recording it against its own target.
void gizmo_commit_gesture() noexcept;
/// Drops the gizmo gesture without recording it: the world's contents are
/// being replaced (scene switch, Play, Stop, world rebind).
void gizmo_abandon_gesture() noexcept;
/// True while a gizmo gesture is open.
bool gizmo_has_gesture() noexcept;

/// Test hook: the next `count` command allocations fail as if out of
/// memory, so the refuse and unrecorded-edit paths are exercisable.
void editor_commands_inject_allocation_failures(std::size_t count) noexcept;

/// Returns the default-valued snapshot used when adding a component.
ComponentEditSnapshot default_component_snapshot(
    runtime::Entity entity, ComponentEditType type) noexcept;
/// Formats the default display name for a newly created entity.
void make_default_entity_name(std::uint32_t entityIndex,
                              runtime::NameComponent *outName) noexcept;

} // namespace engine::editor
