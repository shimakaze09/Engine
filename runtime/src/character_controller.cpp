// Implements the character controller's move: it builds the upright capsule
// from the entity's Capsule Collider and transform, runs the physics
// collide-and-slide, writes the new position and keeps what the move found
// on the controller.

#include "engine/runtime/character_controller.h"

#include <cmath>
#include <cstdio>

#include "engine/core/logging.h"
#include "engine/math/quat.h"
#include "engine/math/scalar.h"
#include "engine/physics/character_move.h"
#include "engine/runtime/world.h"

namespace engine::runtime {
namespace {

constexpr float kDegreesToRadians = 3.14159265358979F / 180.0F;

bool refuse(Entity entity, const char *reason) noexcept {
  char message[192] = {};
  std::snprintf(message, sizeof(message), "move_character: entity %u %s",
                entity.index, reason);
  core::log_message(core::LogLevel::Warning, "physics", message);
  return false;
}

bool uniform(const math::Vec3 &scale) noexcept {
  const float largest = std::fmax(
      std::fabs(scale.x), std::fmax(std::fabs(scale.y), std::fabs(scale.z)));
  const float tolerance = 1.0e-4F * largest;
  return (largest > 0.0F) && (std::fabs(scale.x - scale.y) <= tolerance) &&
         (std::fabs(scale.x - scale.z) <= tolerance);
}

} // namespace

bool move_character(World &world, Entity entity, const math::Vec3 &displacement,
                    CharacterMoveOutcome *out) noexcept {
  if (out != nullptr) {
    *out = CharacterMoveOutcome{};
  }
  if (world.current_phase() != WorldPhase::Input) {
    return refuse(entity, "moves only in the Input phase");
  }
  if (!std::isfinite(displacement.x) || !std::isfinite(displacement.y) ||
      !std::isfinite(displacement.z)) {
    return refuse(entity, "was given a non-finite displacement");
  }
  CharacterControllerComponent *controller =
      world.get_character_controller_ptr(entity);
  if (controller == nullptr) {
    return refuse(entity, "has no character controller");
  }
  Collider collider{};
  if (!world.get_collider(entity, &collider) ||
      (collider.shape != ColliderShape::Capsule)) {
    return refuse(entity, "has no Capsule Collider");
  }
  const math::Quat &local = collider.localRotation;
  if ((local.x != 0.0F) || (local.y != 0.0F) || (local.z != 0.0F)) {
    return refuse(entity, "has a rotated Capsule Collider; a character's "
                          "capsule stands upright");
  }
  Transform transform{};
  if (!world.get_transform(entity, &transform)) {
    return refuse(entity, "has no transform");
  }
  if (transform.parentId != kInvalidPersistentId) {
    return refuse(entity, "is parented; a character is a transform root");
  }
  if (!uniform(transform.scale)) {
    return refuse(entity, "is not scaled uniformly");
  }
  RigidBody body{};
  if (world.get_rigid_body_ptr(entity) != nullptr) {
    static_cast<void>(world.get_rigid_body(entity, &body));
    if (body.bodyType != static_cast<std::uint32_t>(BodyType::Kinematic)) {
      return refuse(entity, "has a dynamic or static rigid body; a "
                            "character's body, if any, is kinematic");
    }
  }

  // Upright, as Unity's: the entity's rotation carries the collider's
  // offset round with its yaw but never tilts the capsule.
  const float scale = std::fabs(transform.scale.x);
  const math::Vec3 center =
      math::add(transform.position,
                math::rotate_vector(math::mul(collider.localPosition, scale),
                                    transform.rotation));
  const float halfSegment = collider.halfExtents.y * scale;
  physics::CharacterCapsule capsule{};
  capsule.bottom = math::sub(center, math::Vec3(0.0F, halfSegment, 0.0F));
  capsule.top = math::add(center, math::Vec3(0.0F, halfSegment, 0.0F));
  capsule.radius = collider.halfExtents.x * scale;

  physics::CharacterMoveSettings settings{};
  settings.slopeLimitCos =
      math::det_cos(controller->slopeLimit * kDegreesToRadians);
  settings.stepOffset = controller->stepOffset;
  settings.skinWidth = controller->skinWidth;
  settings.wasGrounded = controller->grounded;
  settings.self = entity;
  settings.selfCollider = collider;
  physics::CharacterMoveResult result{};
  if (!physics::move_character(world, capsule, displacement, settings,
                               &result)) {
    return refuse(entity, "has a capsule the move cannot use");
  }

  transform.position = math::add(transform.position, result.translation);
  if (!world.add_transform(entity, transform)) {
    return refuse(entity, "could not be moved");
  }
  // The controller may have moved in storage when the transform was
  // written; it is found again.
  controller = world.get_character_controller_ptr(entity);
  if (controller != nullptr) {
    controller->grounded = result.grounded;
    controller->collisionFlags = result.flags;
  }
  if (out != nullptr) {
    out->grounded = result.grounded;
    out->ground = result.ground;
    out->groundNormal = result.groundNormal;
    out->flags = result.flags;
  }
  return true;
}

} // namespace engine::runtime
