// Declares serialization keys types and APIs for the Engine runtime world.

#pragma once

// Shared JSON keys for component serialization.
// Used by scene_serializer.cpp and prefab_serializer.cpp, and bound to
// component types by the persistent-component registry
// (runtime/src/component_registry.h). Divergence between serializers is a
// data-loss bug.

namespace engine::runtime {

inline constexpr const char *kJsonKeyTransform = "Transform";
inline constexpr const char *kJsonKeyRigidBody = "RigidBody";
inline constexpr const char *kJsonKeyCollider = "Collider";
inline constexpr const char *kJsonKeyMeshComponent = "MeshComponent";
inline constexpr const char *kJsonKeySpringArmComponent = "SpringArmComponent";
inline constexpr const char *kJsonKeyPointLightComponent =
    "PointLightComponent";
inline constexpr const char *kJsonKeySpotLightComponent = "SpotLightComponent";
inline constexpr const char *kJsonKeyLightComponent = "LightComponent";
inline constexpr const char *kJsonKeyReflectionProbeComponent =
    "ReflectionProbeComponent";
inline constexpr const char *kJsonKeyFoliagePatchComponent =
    "FoliagePatchComponent";
inline constexpr const char *kJsonKeySceneCaptureComponent =
    "SceneCaptureComponent";
inline constexpr const char *kJsonKeyNameComponent = "NameComponent";
inline constexpr const char *kJsonKeyScriptComponent = "ScriptComponent";
inline constexpr const char *kJsonKeyAnimationComponent =
    "AnimationComponent";
inline constexpr const char *kJsonKeyCameraComponent = "CameraComponent";

} // namespace engine::runtime
