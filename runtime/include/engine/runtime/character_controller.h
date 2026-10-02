// Declares the character controller's move: a script's displacement applied
// to an entity's CharacterControllerComponent through the physics
// collide-and-slide (physics/character_move.h), as Unity's
// CharacterController.Move. The move happens at once, in the Input phase,
// so a script reads its result straight back.

#pragma once

#include <cstdint>

#include "engine/math/vec3.h"
#include "engine/runtime/world_component_types.h"

namespace engine::runtime {

class World;

/// What a character's move found.
struct CharacterMoveOutcome final {
  /// Whether it ended on walkable ground, and on what.
  bool grounded = false;
  Entity ground = kInvalidEntity;
  math::Vec3 groundNormal{};
  /// physics::kCharacterCollided* bits for everything it touched.
  std::uint32_t flags = 0U;
};

/// Moves `entity` by `displacement`, sliding along what it meets, and
/// records grounded and the collision flags on its controller. The capsule
/// is the entity's own Capsule Collider, kept upright as Unity's is, so the
/// entity's yaw turns it and nothing tilts it. Refused, with a logged
/// reason and nothing changed, outside the Input phase, for a non-finite
/// displacement, or for an entity without a controller, without an
/// unrotated Capsule Collider, that is not a transform root, whose scale is
/// not uniform, or whose rigid body (if any) is not kinematic.
bool move_character(World &world, Entity entity, const math::Vec3 &displacement,
                    CharacterMoveOutcome *out) noexcept;

} // namespace engine::runtime
