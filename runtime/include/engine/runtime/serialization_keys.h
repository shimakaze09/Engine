#pragma once

// Shared JSON keys for component serialization.
// Used by scene_serializer.cpp and prefab_serializer.cpp.
// Keep in sync — divergence between serializers is a data-loss bug.

namespace engine::runtime {

inline constexpr const char *kJsonKeyTransform = "Transform";
inline constexpr const char *kJsonKeyRigidBody = "RigidBody";
inline constexpr const char *kJsonKeyCollider = "Collider";
inline constexpr const char *kJsonKeyLightComponent = "LightComponent";
inline constexpr const char *kJsonKeyNameComponent = "NameComponent";
inline constexpr const char *kJsonKeyScriptComponent = "ScriptComponent";
inline constexpr const char *kJsonKeyCameraComponent = "CameraComponent";

} // namespace engine::runtime
