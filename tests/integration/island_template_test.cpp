// Loads the real Island Hopper template JSON and verifies its authored
// per-body gravity semantics against the production physics step:
// normal bodies accelerate at exactly world gravity, gravity-disabled
// bodies (MovingPlatform, FallingRock) hold position with zero total
// acceleration (issue #104).

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kTemplatePath = "assets/templates/island_hopper.json";
constexpr float kDt = 1.0F / 60.0F;

/// Selects the nearest working-directory ancestor containing the template.
bool set_working_directory_with_assets() noexcept {
  std::error_code error{};
  const std::filesystem::path original = std::filesystem::current_path(error);
  if (error) {
    return false;
  }

  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    error.clear();
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, error);
    if (error || !std::filesystem::exists(normalized / kTemplatePath, error)) {
      continue;
    }

    std::filesystem::current_path(normalized, error);
    return !error;
  }

  return false;
}

/// Reads one named entity's rigid body; false when either lookup fails.
bool named_rigid_body(const engine::runtime::World &world, const char *name,
                      engine::runtime::Entity *outEntity,
                      engine::runtime::RigidBody *outBody) noexcept {
  const engine::runtime::Entity entity = world.find_entity_by_name(name);
  if (entity == engine::runtime::kInvalidEntity) {
    std::fprintf(stderr, "FAIL: entity %s not found\n", name);
    return false;
  }
  if (!world.get_rigid_body(entity, outBody)) {
    std::fprintf(stderr, "FAIL: %s has no rigid body\n", name);
    return false;
  }
  *outEntity = entity;
  return true;
}

/// Exact three-component comparison for authored/integrated float state.
bool vec3_equals(const engine::math::Vec3 &v, float x, float y,
                 float z) noexcept {
  return (v.x == x) && (v.y == y) && (v.z == z);
}

/// Runs one production physics step with the pipeline's phase walk.
void step_once(engine::runtime::World &world) noexcept {
  world.begin_update_phase();
  static_cast<void>(engine::runtime::step_physics(world, kDt));
  world.end_frame_phase();
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: locate island template\n");
    return 1;
  }

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  if (!engine::runtime::load_scene(*world, kTemplatePath)) {
    std::fprintf(stderr, "FAIL: load island template\n");
    return 1;
  }

  int failures = 0;

  float gravityX = 0.0F;
  float gravityY = 0.0F;
  float gravityZ = 0.0F;
  if (!engine::runtime::get_gravity(*world, &gravityX, &gravityY, &gravityZ)) {
    std::fprintf(stderr, "FAIL: read world gravity\n");
    return 1;
  }

  engine::runtime::Entity player{};
  engine::runtime::Entity platform{};
  engine::runtime::Entity rock{};
  engine::runtime::RigidBody playerBody{};
  engine::runtime::RigidBody platformBody{};
  engine::runtime::RigidBody rockBody{};
  if (!named_rigid_body(*world, "Player", &player, &playerBody) ||
      !named_rigid_body(*world, "MovingPlatform", &platform, &platformBody) ||
      !named_rigid_body(*world, "FallingRock", &rock, &rockBody)) {
    return 1;
  }

  // --- Authored acceleration semantics: additional on top of gravity ---
  {
    std::printf("  %-40s ", "authored acceleration values");
    const bool ok =
        vec3_equals(playerBody.acceleration, 0.0F, 0.0F, 0.0F) &&
        vec3_equals(platformBody.acceleration, 0.0F, -gravityY, 0.0F) &&
        vec3_equals(rockBody.acceleration, 0.0F, -gravityY, 0.0F);
    if (ok) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL (player %g, platform %g, rock %g vs gravity %g)\n",
                  playerBody.acceleration.y, platformBody.acceleration.y,
                  rockBody.acceleration.y, gravityY);
      ++failures;
    }
  }

  engine::runtime::Transform platformBefore{};
  engine::runtime::Transform rockBefore{};
  if (!world->get_transform(platform, &platformBefore) ||
      !world->get_transform(rock, &rockBefore)) {
    return 1;
  }

  step_once(*world);

  // --- One step: a normal body gains exactly gravity * dt ---
  {
    std::printf("  %-40s ", "player accelerates at world gravity");
    engine::runtime::RigidBody body{};
    // The integrator computes v += (acceleration + gravity) * dt in float;
    // with zero additional acceleration the result is exactly gravity * dt.
    const bool ok = world->get_rigid_body(player, &body) &&
                    vec3_equals(body.velocity, 0.0F, gravityY * kDt, 0.0F);
    if (ok) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Thirty steps: gravity-disabled bodies stay armed in place ---
  for (int i = 0; i < 29; ++i) {
    step_once(*world);
  }
  {
    std::printf("  %-40s ", "platform and rock hold position");
    engine::runtime::RigidBody platformAfter{};
    engine::runtime::RigidBody rockAfter{};
    engine::runtime::Transform platformPose{};
    engine::runtime::Transform rockPose{};
    // -9.8 + 9.8 is exactly zero in float, so held bodies never gain
    // velocity and their serialized positions stay bit-identical.
    const bool ok =
        world->get_rigid_body(platform, &platformAfter) &&
        world->get_rigid_body(rock, &rockAfter) &&
        world->get_transform(platform, &platformPose) &&
        world->get_transform(rock, &rockPose) &&
        vec3_equals(platformAfter.velocity, 0.0F, 0.0F, 0.0F) &&
        vec3_equals(rockAfter.velocity, 0.0F, 0.0F, 0.0F) &&
        vec3_equals(platformPose.position, platformBefore.position.x,
                    platformBefore.position.y, platformBefore.position.z) &&
        vec3_equals(rockPose.position, rockBefore.position.x,
                    rockBefore.position.y, rockBefore.position.z);
    if (ok) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL (rock vy %g, platform vy %g)\n", rockAfter.velocity.y,
                  platformAfter.velocity.y);
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
