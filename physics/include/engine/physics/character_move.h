// Declares the kinematic character move (Unity's CharacterController.Move,
// Godot's CharacterBody3D.move_and_slide): an upright capsule carried by a
// displacement, sliding along what it meets, climbing steps no higher than
// its step offset, refusing slopes steeper than its limit and reporting
// where it was touched and whether it stands on walkable ground. It is a
// query over the world, not a body: it never changes the world itself and
// the caller applies the translation it returns. The exact closest points
// between a segment and a convex shape it is built on are declared here
// too.

#pragma once

#include <cstdint>

#include "engine/core/entity.h"
#include "engine/math/component_types.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics_types.h"

namespace engine::physics {

class PhysicsWorldView;

/// The closest points between a segment and a convex shape.
struct SegmentConvexDistance final {
  /// True when the segment touches or passes through the shape; the
  /// distance is then 0 and the points coincide.
  bool intersecting = false;
  float distance = 0.0F;
  math::Vec3 onSegment{};
  math::Vec3 onShape{};
};

/// Finds the closest points between segment [a, b] and `shape` with GJK,
/// exact for boxes and hulls and within 1e-5 relative for curved shapes.
/// False only for a non-finite input.
bool segment_convex_distance(const math::Vec3 &a, const math::Vec3 &b,
                             const ColliderWorldGeometry &shape,
                             SegmentConvexDistance *out) noexcept;

/// An upright capsule in world space: the centers of its two hemispheres
/// and its radius.
struct CharacterCapsule final {
  math::Vec3 bottom{};
  math::Vec3 top{};
  float radius = 0.0F;
};

/// Where a move was touched, as Unity's CollisionFlags: the lower
/// hemisphere, the cylinder between, the upper hemisphere.
inline constexpr std::uint32_t kCharacterCollidedBelow = 1U;
inline constexpr std::uint32_t kCharacterCollidedSides = 2U;
inline constexpr std::uint32_t kCharacterCollidedAbove = 4U;

struct CharacterMoveSettings final {
  /// Cosine of the steepest walkable slope: ground whose normal has at
  /// least this much up is walkable.
  float slopeLimitCos = 0.70710678F;
  /// The highest step climbed without jumping.
  float stepOffset = 0.3F;
  /// The gap kept between the capsule and what it touches.
  float skinWidth = 0.02F;
  /// Whether the last move ended on walkable ground: a grounded character
  /// climbs steps and stays on the ground walking down a slope or a step.
  bool wasGrounded = false;
  /// The character's own entity and collider. Its collider, and any
  /// collider its entity owns, never blocks it; any other collider blocks
  /// it when the two would collide (layers, masks and the layer matrix) and
  /// it is not a trigger.
  Entity self = kInvalidEntity;
  math::Collider selfCollider{};
};

struct CharacterMoveResult final {
  /// What to add to the capsule's position.
  math::Vec3 translation{};
  /// kCharacterCollided* bits for everything the move touched.
  std::uint32_t flags = 0U;
  /// Whether the move ended on walkable ground, and on what.
  bool grounded = false;
  Entity ground = kInvalidEntity;
  math::Vec3 groundNormal{};
};

/// Moves `capsule` by `displacement` through `world`. It first pushes the
/// capsule out of anything it overlaps, then sweeps it, sliding along each
/// surface it meets (up to four), stepping up onto a ledge no higher than
/// the step offset while grounded, and treating a slope steeper than the
/// limit as a wall while grounded; then it looks for ground below and, when
/// it started grounded and is not moving up, keeps it on the ground. The
/// same world and inputs always give the same result. False, with `out`
/// cleared, for a non-finite input or a capsule without radius.
bool move_character(const PhysicsWorldView &world,
                    const CharacterCapsule &capsule,
                    const math::Vec3 &displacement,
                    const CharacterMoveSettings &settings,
                    CharacterMoveResult *out) noexcept;

} // namespace engine::physics
