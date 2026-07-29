// Verifies a driven box stays vertically stable against a static capsule.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

// Mirrors the demo scene: ground plane, embedded upright capsule prop, and a
// velocity-driven player box pressed into the capsule side. The press must
// never ratchet the box upward or leave it oscillating vertically. Like the
// demo player, the box is rotation-locked (inverseInertia 0) — a driven box
// with free rotation legitimately trips over its contact friction and vaults
// obstacles, which is real tumbling physics, not the controller feel this
// test protects.
int verify_driven_box_against_capsule() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 10;
  }

  // Ground plane: static box top at y = 0.1.
  engine::runtime::Transform groundTransform{};
  const engine::runtime::Entity ground =
      world->create_scene_object(groundTransform);
  engine::runtime::Collider groundCollider{};
  groundCollider.halfExtents = engine::math::Vec3(5.0F, 0.1F, 5.0F);
  engine::runtime::RigidBody staticBody{};
  staticBody.inverseMass = 0.0F;
  if ((ground == engine::runtime::kInvalidEntity) ||
      !world->add_collider(ground, groundCollider) ||
      !world->add_rigid_body(ground, staticBody)) {
    return 11;
  }

  // Static upright capsule prop (radius 0.5, half height 0.5) at x = 2.
  engine::runtime::Transform capsuleTransform{};
  capsuleTransform.position = engine::math::Vec3(2.0F, 0.5F, 0.0F);
  const engine::runtime::Entity capsule =
      world->create_scene_object(capsuleTransform);
  engine::runtime::Collider capsuleCollider{};
  capsuleCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  capsuleCollider.shape = engine::runtime::ColliderShape::Capsule;
  if ((capsule == engine::runtime::kInvalidEntity) ||
      !world->add_collider(capsule, capsuleCollider) ||
      !world->add_rigid_body(capsule, staticBody)) {
    return 12;
  }

  // Player box resting on the plane next to the capsule, matching the demo
  // player's friction/restitution material.
  engine::runtime::Transform playerTransform{};
  playerTransform.position = engine::math::Vec3(0.0F, 0.6F, 0.0F);
  const engine::runtime::Entity player =
      world->create_scene_object(playerTransform);
  engine::runtime::Collider playerCollider{};
  playerCollider.restitution = 0.05F;
  playerCollider.staticFriction = 0.9F;
  playerCollider.dynamicFriction = 0.7F;
  engine::runtime::RigidBody playerBody{};
  playerBody.inverseMass = 1.0F;
  playerBody.inverseInertia = 0.0F;
  if ((player == engine::runtime::kInvalidEntity) ||
      !world->add_collider(player, playerCollider) ||
      !world->add_rigid_body(player, playerBody)) {
    return 13;
  }

  const float dt = 1.0F / 60.0F;
  float maxY = playerTransform.position.y;
  float finalY = playerTransform.position.y;

  for (int step = 0; step < 300; ++step) {
    // The demo controller sets horizontal velocity every tick and keeps the
    // physics-owned vertical velocity.
    engine::runtime::RigidBody *body = world->get_rigid_body_ptr(player);
    if (body == nullptr) {
      return 14;
    }
    body->velocity.x = 5.0F;
    body->velocity.z = 0.0F;
    body->sleeping = false;
    body->sleepFrameCount = 0U;

    world->begin_update_phase();
    if (!engine::runtime::step_physics(*world, dt) ||
        !engine::runtime::resolve_collisions(*world, dt)) {
      world->commit_update_phase();
      return 15;
    }
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->begin_render_phase();
    world->end_frame_phase();

    engine::runtime::Transform playerNow{};
    if (!world->get_transform(player, &playerNow)) {
      return 16;
    }
    if (playerNow.position.y > maxY) {
      maxY = playerNow.position.y;
    }
    finalY = playerNow.position.y;
#ifdef CAPSULE_STABILITY_TRACE
    std::fprintf(stderr, "step=%d x=%.4f y=%.4f vy=%.4f\n", step,
                 playerNow.position.x, playerNow.position.y,
                 world->get_rigid_body_ptr(player)->velocity.y);
#endif
  }

  // Resting height on the plane is 0.6 (plane top 0.1 + half extent 0.5).
  // Pressing into the capsule must never lift the box above the capsule
  // contact band: the box may not climb (max) nor end elevated (final).
  if (maxY > 0.7F) {
    std::fprintf(stderr, "box climbed: maxY=%.4f\n", maxY);
    return 17;
  }
  if (finalY > 0.7F) {
    std::fprintf(stderr, "box ended elevated: finalY=%.4f\n", finalY);
    return 18;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() { return verify_driven_box_against_capsule(); }
