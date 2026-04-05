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
REFLECT_FIELD(inverseMass, Float)
REFLECT_END()

REFLECT_TYPE(engine::runtime::Collider)
REFLECT_FIELD(halfExtents, Vec3)
REFLECT_END()

REFLECT_TYPE(engine::runtime::NameComponent)
static_cast<void>(desc);
// Intentionally registers a zero-field descriptor. NameComponent::name is a
// fixed char array and is serialized/displayed manually rather than through
// REFLECT_FIELD metadata.
REFLECT_END()
