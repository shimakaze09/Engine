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
