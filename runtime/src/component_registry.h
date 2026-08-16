// Authoritative persistent-component registry (audit N-16). One X-macro row
// per serialized component type binds the C++ type, its stable serialized
// name, and the World get/add/remove accessor triple. world_component_counts_match
// and copy_world_contents in scene_serializer.cpp and the codec
// cross-validation suite (tests/unit/component_registry_test.cpp) expand this
// table, and the static_asserts below fail the build whenever the table and
// World::PersistentComponentTypes disagree in either direction — a component
// type cannot join the World's serializable set without a registry row, and a
// row cannot outlive its World type. Issue #156 also expands this table to
// generate the editor Inspector's component-edit type/snapshot/dispatch code
// (editor_component_registry.h), so RemoveFn keeps the row a complete
// get/add/remove triple instead of requiring a second lookup elsewhere.

#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "engine/runtime/serialization_keys.h"
#include "engine/runtime/world.h"

// Row format: X(Type, serializedNameConstant, getAccessor, addAccessor,
// removeAccessor). Row order matches World::PersistentComponentTypes exactly
// (asserted below).
#define ENGINE_PERSISTENT_COMPONENT_TABLE(X)                                   \
  X(Transform, kJsonKeyTransform, get_transform, add_transform,                \
    remove_transform)                                                          \
  X(RigidBody, kJsonKeyRigidBody, get_rigid_body, add_rigid_body,              \
    remove_rigid_body)                                                         \
  X(Collider, kJsonKeyCollider, get_collider, add_collider, remove_collider)   \
  X(MeshComponent, kJsonKeyMeshComponent, get_mesh_component,                  \
    add_mesh_component, remove_mesh_component)                                 \
  X(NameComponent, kJsonKeyNameComponent, get_name_component,                  \
    add_name_component, remove_name_component)                                 \
  X(LightComponent, kJsonKeyLightComponent, get_light_component,               \
    add_light_component, remove_light_component)                              \
  X(ScriptComponent, kJsonKeyScriptComponent, get_script_component,            \
    add_script_component, remove_script_component)                            \
  X(SpringArmComponent, kJsonKeySpringArmComponent, get_spring_arm,            \
    add_spring_arm, remove_spring_arm)                                        \
  X(PointLightComponent, kJsonKeyPointLightComponent,                          \
    get_point_light_component, add_point_light_component,                     \
    remove_point_light_component)                                             \
  X(SpotLightComponent, kJsonKeySpotLightComponent, get_spot_light_component,  \
    add_spot_light_component, remove_spot_light_component)                    \
  X(ReflectionProbeComponent, kJsonKeyReflectionProbeComponent,                \
    get_reflection_probe_component, add_reflection_probe_component,            \
    remove_reflection_probe_component)                                        \
  X(SceneCaptureComponent, kJsonKeySceneCaptureComponent,                      \
    get_scene_capture_component, add_scene_capture_component,                  \
    remove_scene_capture_component)                                           \
  X(FoliagePatchComponent, kJsonKeyFoliagePatchComponent,                      \
    get_foliage_patch_component, add_foliage_patch_component,                  \
    remove_foliage_patch_component)                                           \
  X(AnimationComponent, kJsonKeyAnimationComponent, get_animation_component,   \
    add_animation_component, remove_animation_component)                     \
  X(CameraComponent, kJsonKeyCameraComponent, get_camera_component,           \
    add_camera_component, remove_camera_component)

namespace engine::runtime {

/// Registry rows as a type list for compile-time comparison with the World.
#define ENGINE_PCR_TUPLE_ELEM(Type, Key, GetFn, AddFn, RemoveFn)               \
  std::declval<std::tuple<Type>>(),
using RegistryComponentTypes = decltype(std::tuple_cat(
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_TUPLE_ELEM)
        std::declval<std::tuple<>>()));
#undef ENGINE_PCR_TUPLE_ELEM

/// Number of registry rows; the single source for persistent-type counts.
inline constexpr std::size_t kPersistentComponentTypeCount =
    std::tuple_size_v<RegistryComponentTypes>;

// Both directions of the registry <-> World contract, checked at compile
// time: identical type lists (same types, same order) implies identical
// counts, so neither side can gain or lose a type silently.
static_assert(
    std::is_same_v<RegistryComponentTypes, World::PersistentComponentTypes>,
    "component_registry.h must list exactly World::PersistentComponentTypes, "
    "in order");
static_assert(kPersistentComponentTypeCount ==
                  World::kPersistentComponentTypeCount,
              "registry row count must match the World's persistent count");

// Every row's accessor triple must keep the canonical get/add/remove
// signatures the generated copy, count-check, round-trip, and editor
// dispatch code depends on.
#define ENGINE_PCR_CHECK_ROW(Type, Key, GetFn, AddFn, RemoveFn)                \
  static_assert(std::is_same_v<decltype(&World::GetFn),                        \
                               bool (World::*)(Entity, Type *)                 \
                                   const noexcept>,                            \
                #GetFn " must be bool(Entity, " #Type "*) const noexcept");    \
  static_assert(std::is_same_v<decltype(&World::AddFn),                        \
                               bool (World::*)(Entity, const Type &)           \
                                   noexcept>,                                  \
                #AddFn " must be bool(Entity, const " #Type "&) noexcept");    \
  static_assert(std::is_same_v<decltype(&World::RemoveFn),                     \
                               bool (World::*)(Entity) noexcept>,              \
                #RemoveFn " must be bool(Entity) noexcept");
ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_CHECK_ROW)
#undef ENGINE_PCR_CHECK_ROW

} // namespace engine::runtime
