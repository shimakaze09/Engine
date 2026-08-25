// Implements reflect types behavior for the Engine runtime world.

#include "engine/runtime/reflect_types.h"
#include "engine/core/reflect.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

void ensure_runtime_reflection_registered() noexcept {
  // Intentionally empty: calling this symbol forces this translation unit
  // to link so static registration blocks execute.
}

} // namespace engine::runtime

REFLECT_TYPE(engine::runtime::Transform)
REFLECT_FIELD(position, Vec3)
REFLECT_FIELD(rotation, Quat)
REFLECT_FIELD(scale, Vec3)
REFLECT_FIELD(parentId, Uint32)
REFLECT_END()

REFLECT_TYPE(engine::runtime::RigidBody)
REFLECT_FIELD(velocity, Vec3)
REFLECT_FIELD(acceleration, Vec3)
REFLECT_FIELD(angularVelocity, Vec3)
REFLECT_FIELD(inverseMass, Float)
REFLECT_FIELD(inverseInertia, Float)
REFLECT_FIELD(sleeping, Bool)
REFLECT_END()

REFLECT_TYPE(engine::runtime::Collider)
REFLECT_FIELD(localPosition, Vec3)
REFLECT_FIELD(localRotation, Quat)
REFLECT_FIELD(halfExtents, Vec3)
REFLECT_FIELD(restitution, Float)
REFLECT_FIELD(staticFriction, Float)
REFLECT_FIELD(dynamicFriction, Float)
REFLECT_FIELD(density, Float)
REFLECT_FIELD(collisionLayer, Uint32)
REFLECT_FIELD(collisionMask, Uint32)
REFLECT_END()

REFLECT_TYPE(engine::runtime::NameComponent)
static_cast<void>(desc);
// Intentionally registers a zero-field descriptor. NameComponent::name is a
// fixed char array and is serialized/displayed manually rather than through
// REFLECT_FIELD metadata.
REFLECT_END()

REFLECT_TYPE(engine::runtime::ScriptComponent)
static_cast<void>(desc);
// Intentionally registers a zero-field descriptor. ScriptComponent::scriptPath
// is a fixed char array serialized manually.
REFLECT_END()

REFLECT_TYPE(engine::runtime::AnimationComponent)
static_cast<void>(desc);
// Intentionally registers a zero-field descriptor. The controller path is
// a fixed char array, which reflection has no field kind for, so the whole
// authored set -- path, playing, playbackSpeed -- is serialized manually by
// write_animation_component/read_animation_component; the remaining members
// are runtime state and are never persisted.
REFLECT_END()

REFLECT_TYPE(engine::runtime::SpringArmComponent)
REFLECT_FIELD(armLength, Float)
REFLECT_FIELD(currentLength, Float)
REFLECT_FIELD(offset, Vec3)
REFLECT_FIELD(lagSpeed, Float)
REFLECT_FIELD(collisionRadius, Float)
REFLECT_FIELD(collisionEnabled, Bool)
REFLECT_END()

REFLECT_TYPE(engine::runtime::PointLightComponent)
REFLECT_FIELD(color, Vec3)
REFLECT_FIELD(intensity, Float)
REFLECT_FIELD(radius, Float)
REFLECT_END()

REFLECT_TYPE(engine::runtime::SpotLightComponent)
REFLECT_FIELD(color, Vec3)
REFLECT_FIELD(direction, Vec3)
REFLECT_FIELD(intensity, Float)
REFLECT_FIELD(radius, Float)
REFLECT_FIELD(innerConeAngle, Float)
REFLECT_FIELD(outerConeAngle, Float)
REFLECT_END()

REFLECT_TYPE(engine::runtime::ReflectionProbeComponent)
REFLECT_FIELD(boxExtents, Vec3)
REFLECT_FIELD(radius, Float)
REFLECT_FIELD(intensity, Float)
REFLECT_FIELD(prefilteredResolution, Uint32)
REFLECT_FIELD(irradianceResolution, Uint32)
REFLECT_FIELD(brdfLutResolution, Uint32)
REFLECT_FIELD(mipLevels, Uint32)
REFLECT_FIELD(boxProjection, Bool)
REFLECT_FIELD(needsBake, Bool)
REFLECT_END()

REFLECT_TYPE(engine::runtime::SceneCaptureComponent)
REFLECT_FIELD(width, Uint32)
REFLECT_FIELD(height, Uint32)
REFLECT_FIELD(fovRadians, Float)
REFLECT_FIELD(nearPlane, Float)
REFLECT_FIELD(farPlane, Float)
REFLECT_FIELD(enabled, Bool)
REFLECT_END()

REFLECT_TYPE(engine::runtime::CameraComponent)
REFLECT_FIELD(projection, Uint32)
REFLECT_FIELD(fovRadians, Float)
REFLECT_FIELD(orthographicSize, Float)
REFLECT_FIELD(nearPlane, Float)
REFLECT_FIELD(farPlane, Float)
REFLECT_FIELD(priority, Float)
REFLECT_FIELD(blendSpeed, Float)
REFLECT_FIELD(active, Bool)
REFLECT_END()

REFLECT_TYPE(engine::runtime::FoliagePatchComponent)
static_cast<void>(desc);
// Fixed instance and LOD arrays are edited/serialized manually.
REFLECT_END()

REFLECT_TYPE(engine::runtime::MeshComponent)
static_cast<void>(desc);
// Intentionally registers a zero-field descriptor (issue #156): the asset-id
// fields are 64-bit and TypeField::Kind has no Uint64 case, and albedo/
// roughness/metallic/opacity/sceneCaptureSourceId are edited through the
// editor's typed asset/entity pickers rather than the generic field loop.
REFLECT_END()

REFLECT_TYPE(engine::runtime::LightComponent)
REFLECT_FIELD(color, Vec3)
REFLECT_FIELD(direction, Vec3)
REFLECT_FIELD(intensity, Float)
// `type` (LightType) is not reflected: Kind has no enum case, so the editor
// draws it with a small dedicated combo box ahead of the generic fields.
REFLECT_END()
