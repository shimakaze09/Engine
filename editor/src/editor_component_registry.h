// Declares the editor's ComponentEditType/ComponentEditSnapshot pair,
// generated from the runtime's authoritative persistent-component registry.

#pragma once

#include <cstdint>

#include "component_registry.h"
#include "engine/runtime/world.h"

namespace engine::editor {

// Every enumerator and struct member below is generated from
// ENGINE_PERSISTENT_COMPONENT_TABLE (runtime/src/component_registry.h) so a
// new persistent component type automatically gains an inspector edit slot;
// forgetting the ENGINE_ICR_ALIAS_<Type>/ENGINE_ICR_MEMBER_<Type> pair for a
// new row is a compile error (unresolved token), not a silent gap (issue
// #156). The alias tables exist only to keep the pre-existing, already
// call-site-stable enumerator/member names (Mesh, Light, springArm, ...)
// instead of forcing every reference to the full C++ type name.
#define ENGINE_ICR_ALIAS(Type) ENGINE_ICR_ALIAS_##Type
#define ENGINE_ICR_ALIAS_Transform Transform
#define ENGINE_ICR_ALIAS_RigidBody RigidBody
#define ENGINE_ICR_ALIAS_Collider Collider
#define ENGINE_ICR_ALIAS_MeshComponent Mesh
#define ENGINE_ICR_ALIAS_NameComponent Name
#define ENGINE_ICR_ALIAS_LightComponent Light
#define ENGINE_ICR_ALIAS_ScriptComponent Script
#define ENGINE_ICR_ALIAS_SpringArmComponent SpringArm
#define ENGINE_ICR_ALIAS_PointLightComponent PointLight
#define ENGINE_ICR_ALIAS_SpotLightComponent SpotLight
#define ENGINE_ICR_ALIAS_ReflectionProbeComponent ReflectionProbe
#define ENGINE_ICR_ALIAS_SceneCaptureComponent SceneCapture
#define ENGINE_ICR_ALIAS_FoliagePatchComponent FoliagePatch
#define ENGINE_ICR_ALIAS_AnimationComponent Animation

#define ENGINE_ICR_MEMBER(Type) ENGINE_ICR_MEMBER_##Type
#define ENGINE_ICR_MEMBER_Transform transform
#define ENGINE_ICR_MEMBER_RigidBody rigidBody
#define ENGINE_ICR_MEMBER_Collider collider
#define ENGINE_ICR_MEMBER_MeshComponent mesh
#define ENGINE_ICR_MEMBER_NameComponent name
#define ENGINE_ICR_MEMBER_LightComponent light
#define ENGINE_ICR_MEMBER_ScriptComponent script
#define ENGINE_ICR_MEMBER_SpringArmComponent springArm
#define ENGINE_ICR_MEMBER_PointLightComponent pointLight
#define ENGINE_ICR_MEMBER_SpotLightComponent spotLight
#define ENGINE_ICR_MEMBER_ReflectionProbeComponent reflectionProbe
#define ENGINE_ICR_MEMBER_SceneCaptureComponent sceneCapture
#define ENGINE_ICR_MEMBER_FoliagePatchComponent foliagePatch
#define ENGINE_ICR_MEMBER_AnimationComponent animation

/// Enumerates component edit type values, one per persistent-component
/// registry row (order follows the registry, not historical declaration
/// order).
#define ENGINE_ICR_ENUM_ROW(Type, Key, GetFn, AddFn, RemoveFn)                 \
  ENGINE_ICR_ALIAS(Type),
enum class ComponentEditType : std::uint8_t {
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_ENUM_ROW)
};
#undef ENGINE_ICR_ENUM_ROW

/// Number of ComponentEditType values (used to size per-type flag arrays).
inline constexpr std::size_t kComponentEditTypeCount =
    runtime::kPersistentComponentTypeCount;

/// Union-of-components value captured before/after an inspector edit; one
/// member per registry row, generated so a new persistent component cannot
/// be added without also gaining a snapshot slot.
#define ENGINE_ICR_MEMBER_ROW(Type, Key, GetFn, AddFn, RemoveFn)               \
  runtime::Type ENGINE_ICR_MEMBER(Type){};
struct ComponentEditSnapshot final {
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_MEMBER_ROW)
};
#undef ENGINE_ICR_MEMBER_ROW

/// Fills a snapshot from the entity's current component of `type`; false
/// when the world is unbound or the component is absent.
bool capture_component_snapshot(ComponentEditType type, runtime::Entity entity,
                                ComponentEditSnapshot *out) noexcept;
/// Applies (or removes, when !exists) the snapshotted component of `type`;
/// false when the exact entity generation is no longer alive.
bool apply_component_snapshot(ComponentEditType type, runtime::Entity entity,
                              bool exists,
                              const ComponentEditSnapshot &snapshot) noexcept;
/// True when the entity currently carries a component of `type` (a
/// presence-only probe built on capture_component_snapshot; the generic
/// replacement for the old per-type get_ptr/has_X existence checks the Add
/// Component menu used to hand-roll for each branch).
bool has_component_of_type(ComponentEditType type,
                           runtime::Entity entity) noexcept;

} // namespace engine::editor
