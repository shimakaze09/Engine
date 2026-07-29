// Regression suite for resting-contact behavior: a driven box that collides
// with a dynamic ball must come to rest ON its support (no hovering, no
// corner-balancing), a tilted box must settle flat, and a box overhanging a
// ledge must tip and fall like real physics.

#include <cmath>
#include <memory>
#include <new>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

constexpr float kDt = 1.0F / 60.0F;

/// Advances one fixed step in the runtime pipeline's phase order.
bool step_world(engine::runtime::World &world) {
  world.begin_update_phase();
  const bool ok = engine::runtime::step_physics(world, kDt) &&
                  engine::runtime::resolve_collisions(world, kDt);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  return ok;
}

/// Lowest world-space corner height of a rotated box transform.
float lowest_corner_y(const engine::runtime::Transform &transform,
                      const engine::math::Vec3 &halfExtents) {
  float lowest = 1.0e30F;
  for (int corner = 0; corner < 8; ++corner) {
    const engine::math::Vec3 local(
        ((corner & 1) != 0) ? halfExtents.x : -halfExtents.x,
        ((corner & 2) != 0) ? halfExtents.y : -halfExtents.y,
        ((corner & 4) != 0) ? halfExtents.z : -halfExtents.z);
    const engine::math::Vec3 world = engine::math::add(
        engine::math::rotate_vector(local, transform.rotation),
        transform.position);
    if (world.y < lowest) {
      lowest = world.y;
    }
  }
  return lowest;
}

/// Tilt angle in degrees between the box's up axis and world up.
float tilt_degrees(const engine::runtime::Transform &transform) {
  const engine::math::Vec3 up = engine::math::rotate_vector(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), transform.rotation);
  const float c = std::fmax(-1.0F, std::fmin(1.0F, up.y));
  return std::acos(c) * 57.2957795F;
}

/// Degrees between world up and the box axis best aligned with it: zero when
/// the box rests flat on any of its six faces, regardless of which one.
float face_flat_degrees(const engine::runtime::Transform &transform) {
  float bestAlignment = 0.0F;
  const engine::math::Vec3 axes[3] = {engine::math::Vec3(1.0F, 0.0F, 0.0F),
                                      engine::math::Vec3(0.0F, 1.0F, 0.0F),
                                      engine::math::Vec3(0.0F, 0.0F, 1.0F)};
  for (const engine::math::Vec3 &axis : axes) {
    const engine::math::Vec3 world =
        engine::math::rotate_vector(axis, transform.rotation);
    const float alignment = std::fabs(world.y);
    if (alignment > bestAlignment) {
      bestAlignment = alignment;
    }
  }
  return std::acos(std::fmax(-1.0F, std::fmin(1.0F, bestAlignment))) *
         57.2957795F;
}

/// Builds the demo-scene trio: static ground slab, player cube, dynamic ball.
struct DemoScene {
  std::unique_ptr<engine::runtime::World> world;
  engine::runtime::Entity ground{};
  engine::runtime::Entity cube{};
  engine::runtime::Entity ball{};
};

bool build_demo_scene(DemoScene *outScene) {
  outScene->world.reset(new (std::nothrow) engine::runtime::World());
  if (outScene->world == nullptr) {
    return false;
  }
  engine::runtime::World &world = *outScene->world;
  world.end_frame_phase();

  engine::runtime::Transform groundTransform{};
  groundTransform.position = engine::math::Vec3(0.0F, -0.5F, 0.0F);
  outScene->ground = world.create_scene_object(groundTransform);
  engine::runtime::Collider groundCollider{};
  groundCollider.shape = engine::runtime::ColliderShape::AABB;
  groundCollider.halfExtents = engine::math::Vec3(20.0F, 0.5F, 20.0F);
  groundCollider.staticFriction = 0.9F;
  groundCollider.dynamicFriction = 0.7F;
  groundCollider.restitution = 0.1F;
  if ((outScene->ground == engine::runtime::kInvalidEntity) ||
      !world.add_collider(outScene->ground, groundCollider)) {
    return false;
  }

  engine::runtime::Transform cubeTransform{};
  cubeTransform.position = engine::math::Vec3(0.0F, 3.0F, 0.0F);
  outScene->cube = world.create_scene_object(cubeTransform);
  engine::runtime::Collider cubeCollider{};
  cubeCollider.shape = engine::runtime::ColliderShape::AABB;
  cubeCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  cubeCollider.staticFriction = 0.9F;
  cubeCollider.dynamicFriction = 0.7F;
  cubeCollider.restitution = 0.05F;
  engine::runtime::RigidBody cubeBody{};
  cubeBody.inverseMass = 1.0F;
  cubeBody.acceleration = engine::math::Vec3(0.0F, -9.8F, 0.0F);
  if ((outScene->cube == engine::runtime::kInvalidEntity) ||
      !world.add_collider(outScene->cube, cubeCollider) ||
      !world.add_rigid_body(outScene->cube, cubeBody)) {
    return false;
  }

  engine::runtime::Transform ballTransform{};
  ballTransform.position = engine::math::Vec3(3.0F, 2.0F, 0.0F);
  outScene->ball = world.create_scene_object(ballTransform);
  engine::runtime::Collider ballCollider{};
  ballCollider.shape = engine::runtime::ColliderShape::Sphere;
  ballCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  ballCollider.staticFriction = 0.6F;
  ballCollider.dynamicFriction = 0.4F;
  ballCollider.restitution = 0.5F;
  engine::runtime::RigidBody ballBody{};
  ballBody.inverseMass = 1.0F;
  ballBody.acceleration = engine::math::Vec3(0.0F, -9.8F, 0.0F);
  if ((outScene->ball == engine::runtime::kInvalidEntity) ||
      !world.add_collider(outScene->ball, ballCollider) ||
      !world.add_rigid_body(outScene->ball, ballBody)) {
    return false;
  }
  return true;
}

/// Mimics assets/scripts/player.lua: horizontal velocity is set every tick,
/// the vertical component is preserved from physics.
void drive_player(engine::runtime::World &world, engine::runtime::Entity cube,
                  float vx) {
  engine::runtime::RigidBody *body = world.get_rigid_body_ptr(cube);
  if (body != nullptr) {
    body->velocity = engine::math::Vec3(vx, body->velocity.y, 0.0F);
    body->sleeping = false;
    body->sleepFrameCount = 0U;
  }
}

/// The demo scenario that produced the floating-cube report: settle, drive
/// the player cube into the dynamic ball, release, and require the cube to
/// rest with its lowest corner on the ground, flat on one of its faces
/// (ramming the ball can legitimately trip it onto a different face), with
/// no residual spin.
int check_cube_rests_after_ball_contact() {
  DemoScene scene{};
  if (!build_demo_scene(&scene)) {
    return 960;
  }
  engine::runtime::World &world = *scene.world;

  for (int i = 0; i < 180; ++i) {
    if (!step_world(world)) {
      return 961;
    }
  }
  for (int i = 0; i < 60; ++i) {
    drive_player(world, scene.cube, 5.0F);
    if (!step_world(world)) {
      return 962;
    }
  }
  for (int i = 0; i < 600; ++i) {
    drive_player(world, scene.cube, 0.0F);
    if (!step_world(world)) {
      return 963;
    }
  }

  engine::runtime::Transform finalT{};
  if (!world.get_transform(scene.cube, &finalT)) {
    return 964;
  }
  const float lowest =
      lowest_corner_y(finalT, engine::math::Vec3(0.5F, 0.5F, 0.5F));
  if (std::fabs(lowest) > 0.005F) {
    return 965;
  }
  if (face_flat_degrees(finalT) > 1.0F) {
    return 966;
  }
  if ((finalT.position.y < 0.49F) || (finalT.position.y > 0.52F)) {
    return 967;
  }
  const engine::runtime::RigidBody *body =
      world.get_rigid_body_ptr(scene.cube);
  if ((body == nullptr) ||
      (engine::math::length_sq(body->angularVelocity) > 1.0e-4F)) {
    return 968;
  }
  return 0;
}

/// A cube dropped with an initial 20-degree tilt must receive torque from
/// the static ground, settle flat on a face, and go to sleep — not balance
/// forever on one corner (the floating-cube root cause).
int check_tilted_drop_settles_flat() {
  DemoScene scene{};
  if (!build_demo_scene(&scene)) {
    return 970;
  }
  engine::runtime::World &world = *scene.world;

  world.begin_update_phase();
  engine::runtime::Transform *tilt = world.get_transform_write_ptr(
      scene.cube, world.simulation_access_token());
  if (tilt == nullptr) {
    return 971;
  }
  tilt->position = engine::math::Vec3(-3.0F, 1.2F, 0.0F);
  tilt->rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 0.0F, 1.0F), 20.0F * 0.01745329F);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();

  for (int i = 0; i < 600; ++i) {
    if (!step_world(world)) {
      return 972;
    }
  }

  engine::runtime::Transform finalT{};
  if (!world.get_transform(scene.cube, &finalT)) {
    return 973;
  }
  if (tilt_degrees(finalT) > 0.5F) {
    return 974;
  }
  const float lowest =
      lowest_corner_y(finalT, engine::math::Vec3(0.5F, 0.5F, 0.5F));
  if (std::fabs(lowest) > 0.005F) {
    return 975;
  }
  if (std::fabs(finalT.position.y - 0.5F) > 0.01F) {
    return 976;
  }
  const engine::runtime::RigidBody *body =
      world.get_rigid_body_ptr(scene.cube);
  if ((body == nullptr) || !body->sleeping) {
    return 977;
  }
  return 0;
}

/// A box whose center of mass overhangs a ledge edge must rotate over the
/// edge and fall — not hover unsupported on the overlap region.
int check_overhanging_box_tips() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 980;
  }
  world->end_frame_phase();

  engine::runtime::Transform ledgeTransform{};
  ledgeTransform.position = engine::math::Vec3(0.0F, -0.5F, 0.0F);
  const engine::runtime::Entity ledge =
      world->create_scene_object(ledgeTransform);
  engine::runtime::Collider ledgeCollider{};
  ledgeCollider.shape = engine::runtime::ColliderShape::AABB;
  ledgeCollider.halfExtents = engine::math::Vec3(1.0F, 0.5F, 1.0F);
  ledgeCollider.staticFriction = 0.9F;
  ledgeCollider.dynamicFriction = 0.7F;
  ledgeCollider.restitution = 0.0F;
  if ((ledge == engine::runtime::kInvalidEntity) ||
      !world->add_collider(ledge, ledgeCollider)) {
    return 981;
  }

  engine::runtime::Transform boxTransform{};
  boxTransform.position = engine::math::Vec3(1.3F, 0.55F, 0.0F);
  const engine::runtime::Entity box = world->create_scene_object(boxTransform);
  engine::runtime::Collider boxCollider{};
  boxCollider.shape = engine::runtime::ColliderShape::AABB;
  boxCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  boxCollider.staticFriction = 0.9F;
  boxCollider.dynamicFriction = 0.7F;
  boxCollider.restitution = 0.0F;
  engine::runtime::RigidBody boxBody{};
  boxBody.inverseMass = 1.0F;
  boxBody.acceleration = engine::math::Vec3(0.0F, -9.8F, 0.0F);
  if ((box == engine::runtime::kInvalidEntity) ||
      !world->add_collider(box, boxCollider) ||
      !world->add_rigid_body(box, boxBody)) {
    return 982;
  }

  float maxTilt = 0.0F;
  for (int i = 0; i < 360; ++i) {
    if (!step_world(*world)) {
      return 983;
    }
    engine::runtime::Transform t{};
    world->get_transform(box, &t);
    const float tilt = tilt_degrees(t);
    if (tilt > maxTilt) {
      maxTilt = tilt;
    }
  }

  engine::runtime::Transform finalT{};
  if (!world->get_transform(box, &finalT)) {
    return 984;
  }
  if (maxTilt < 45.0F) {
    return 985;
  }
  if (finalT.position.y > -1.0F) {
    return 986;
  }
  if (finalT.position.x <= 1.3F) {
    return 987;
  }
  return 0;
}

/// A box spun about the vertical axis while resting on the ground must be
/// braked by contact friction well before pure angular damping alone would
/// decay it (damping alone leaves ~0.5 rad/s after one second from 3 rad/s).
int check_ground_friction_stops_spin() {
  DemoScene scene{};
  if (!build_demo_scene(&scene)) {
    return 990;
  }
  engine::runtime::World &world = *scene.world;

  for (int i = 0; i < 120; ++i) {
    if (!step_world(world)) {
      return 991;
    }
  }
  engine::runtime::RigidBody *body = world.get_rigid_body_ptr(scene.cube);
  if (body == nullptr) {
    return 992;
  }
  body->angularVelocity = engine::math::Vec3(0.0F, 3.0F, 0.0F);
  body->sleeping = false;
  body->sleepFrameCount = 0U;

  for (int i = 0; i < 60; ++i) {
    if (!step_world(world)) {
      return 993;
    }
  }
  const engine::runtime::RigidBody *after =
      world.get_rigid_body_ptr(scene.cube);
  if ((after == nullptr) ||
      (engine::math::length_sq(after->angularVelocity) > 1.0e-4F)) {
    return 994;
  }
  engine::runtime::Transform finalT{};
  if (!world.get_transform(scene.cube, &finalT)) {
    return 995;
  }
  const float lowest =
      lowest_corner_y(finalT, engine::math::Vec3(0.5F, 0.5F, 0.5F));
  if (std::fabs(lowest) > 0.005F) {
    return 996;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const int restResult = check_cube_rests_after_ball_contact();
  if (restResult != 0) {
    return restResult;
  }
  const int tiltResult = check_tilted_drop_settles_flat();
  if (tiltResult != 0) {
    return tiltResult;
  }
  const int tipResult = check_overhanging_box_tips();
  if (tipResult != 0) {
    return tipResult;
  }
  return check_ground_friction_stops_spin();
}

