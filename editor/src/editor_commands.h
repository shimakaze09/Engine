// Declares the editor's undoable edit commands and component snapshot
// helpers shared by the inspector, hierarchy, and viewport panels.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "editor_session.h"

namespace engine::editor {

struct TransformEditCommand final : EditorCommand {
  runtime::Entity entity{};
  runtime::Transform oldTransform{};
  runtime::Transform newTransform{};

  void execute() noexcept override {
    if (editor_session().world != nullptr) {
      static_cast<void>(editor_session().world->add_transform(entity, newTransform));
    }
  }

  void undo() noexcept override {
    if (editor_session().world != nullptr) {
      static_cast<void>(editor_session().world->add_transform(entity, oldTransform));
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
};

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
};

/// Fills a snapshot from the entity's current component of `type`; false
/// when the world is unbound or the component is absent.
bool capture_component_snapshot(ComponentEditType type, runtime::Entity entity,
                                ComponentEditSnapshot *out) noexcept;
/// Applies (or removes, when !exists) the snapshotted component of `type`.
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
  ComponentEditType type = ComponentEditType::Transform;
  bool beforeExists = false;
  bool afterExists = false;
  ComponentEditSnapshot before{};
  ComponentEditSnapshot after{};

  void execute() noexcept override {
    static_cast<void>(
        apply_component_snapshot(type, entity, afterExists, after));
  }

  void undo() noexcept override {
    static_cast<void>(
        apply_component_snapshot(type, entity, beforeExists, before));
  }
};

/// Returns the default-valued snapshot used when adding a component.
ComponentEditSnapshot default_component_snapshot(
    runtime::Entity entity, ComponentEditType type) noexcept;
/// Formats the default display name for a newly created entity.
void make_default_entity_name(std::uint32_t entityIndex,
                              runtime::NameComponent *outName) noexcept;

} // namespace engine::editor
