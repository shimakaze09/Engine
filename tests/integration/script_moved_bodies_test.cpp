// Script-moved bodies keep colliding (#980), through the production
// pipeline and Lua at 1, 2 and 8 workers. engine.set_position used to take
// whatever it moved out of the simulation: a dynamic body stopped
// integrating and every collider it owned left pair generation, so balls
// fell through a body a script pinned or a door a script slid, and no
// collision callback fired. A teleport now keeps the body's type, and a
// body meant to be driven says so with Body Type Kinematic.
//
// One scene, each fixture well apart from the others:
//  - Pinned: a heavy dynamic plate a script re-places every fixed step;
//    Ball A drops onto it.
//  - Door: a collider with no rigid body a script slides a little every
//    step; Ball B drops onto it.
//  - Crane: a dynamic root with no collider of its own, pinned by script,
//    whose child Hook carries the collider; Ball C drops onto the hook.
//  - Lift: a kinematic platform a script drives by velocity; a Rider
//    stands on it and a Block floats in its path.
// Each ball must come to rest on its support and have its pair reported to
// a handler a script registered with engine.on_collision_handler. The lift must
// move exactly as its velocity says, feel no gravity, carry the rider and push
// the block. The three runs must end on the same World::state_hash.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

namespace {

constexpr std::uint32_t kFrameCount = 180U;
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr float kLiftSpeed = 0.5F;
constexpr const char *kRefereeScriptPath = "script_moved_bodies_referee.lua";
constexpr const char *kPinScriptPath = "script_moved_bodies_pin.lua";
constexpr const char *kDoorScriptPath = "script_moved_bodies_door.lua";
constexpr const char *kLiftScriptPath = "script_moved_bodies_lift.lua";

int g_failures = 0;

void check(bool condition, const char *message) noexcept {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++g_failures;
  }
}

bool write_file(const char *path, const char *text) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(text);
  const bool ok = (std::fwrite(text, 1U, length, file) == length);
  return (std::fclose(file) == 0) && ok;
}

// The referee module registers a collision handler that records, once per
// pair of names, that the pair was reported, and publishes the sorted set
// as the game state for the test to read.
constexpr const char *kRefereeScript =
    "local M = {}\n"
    "local seen = {}\n"
    "local function note(a, b)\n"
    "    local na, nb = engine.get_name(a), engine.get_name(b)\n"
    "    if na == nil or nb == nil then return end\n"
    "    if na > nb then na, nb = nb, na end\n"
    "    local key = na .. '+' .. nb\n"
    "    if seen[key] then return end\n"
    "    seen[key] = true\n"
    "    local keys = {}\n"
    "    for k in pairs(seen) do keys[#keys + 1] = k end\n"
    "    table.sort(keys)\n"
    "    engine.set_game_state(table.concat(keys, ' '))\n"
    "end\n"
    "function M.on_begin_play(self)\n"
    "    engine.on_collision_handler(note)\n"
    "end\n"
    "return M\n";

// Re-places its entity where it started on every fixed step: the pattern
// that used to make the body a ghost.
constexpr const char *kPinScript =
    "local M = {}\n"
    "local home = {}\n"
    "function M.on_begin_play(self)\n"
    "    local x, y, z = engine.get_position(self)\n"
    "    home[self] = {x, y, z}\n"
    "end\n"
    "function M.on_fixed_tick(self, dt)\n"
    "    local h = home[self]\n"
    "    engine.set_position(self, h[1], h[2], h[3])\n"
    "end\n"
    "return M\n";

// Slides its collider-only entity along x by a millimetre a step.
constexpr const char *kDoorScript =
    "local M = {}\n"
    "function M.on_fixed_tick(self, dt)\n"
    "    local x, y, z = engine.get_position(self)\n"
    "    engine.set_position(self, x + 0.001, y, z)\n"
    "end\n"
    "return M\n";

// Makes its body kinematic and drives it along +x by velocity.
constexpr const char *kLiftScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    if not engine.set_body_type(self, 'kinematic') then\n"
    "        error('set_body_type refused')\n"
    "    end\n"
    "    if engine.get_body_type(self) ~= 'kinematic' then\n"
    "        error('get_body_type disagrees')\n"
    "    end\n"
    "    engine.set_velocity(self, 0.5, 0.0, 0.0)\n"
    "end\n"
    "return M\n";

struct Fixture final {
  engine::runtime::Entity ballA = engine::runtime::kInvalidEntity;
  engine::runtime::Entity ballB = engine::runtime::kInvalidEntity;
  engine::runtime::Entity ballC = engine::runtime::kInvalidEntity;
  engine::runtime::Entity lift = engine::runtime::kInvalidEntity;
  engine::runtime::Entity rider = engine::runtime::kInvalidEntity;
  engine::runtime::Entity block = engine::runtime::kInvalidEntity;
};

engine::runtime::Entity
add_object(engine::runtime::World &world, const char *name,
           const engine::math::Vec3 &position) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  if (entity == engine::runtime::kInvalidEntity) {
    return entity;
  }
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  return world.add_name_component(entity, nameComponent)
             ? entity
             : engine::runtime::kInvalidEntity;
}

bool add_box(engine::runtime::World &world, engine::runtime::Entity entity,
             const engine::math::Vec3 &halfExtents) noexcept {
  engine::runtime::Collider collider{};
  collider.shape = engine::runtime::ColliderShape::AABB;
  collider.halfExtents = halfExtents;
  return world.add_collider(entity, collider);
}

bool add_script(engine::runtime::World &world, engine::runtime::Entity entity,
                const char *path) noexcept {
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s", path);
  return world.add_script_component(entity, script);
}

bool add_body(engine::runtime::World &world, engine::runtime::Entity entity,
              float inverseMass, float gravityScale) noexcept {
  engine::runtime::RigidBody body{};
  body.inverseMass = inverseMass;
  body.gravityScale = gravityScale;
  return world.add_rigid_body(entity, body);
}

/// A dynamic ball of radius 0.25 at `position`, falling under gravity.
engine::runtime::Entity add_ball(engine::runtime::World &world,
                                 const char *name,
                                 const engine::math::Vec3 &position) noexcept {
  const engine::runtime::Entity ball = add_object(world, name, position);
  engine::runtime::Collider collider{};
  collider.shape = engine::runtime::ColliderShape::Sphere;
  collider.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);
  collider.restitution = 0.0F;
  if ((ball == engine::runtime::kInvalidEntity) ||
      !world.add_collider(ball, collider) ||
      !add_body(world, ball, 1.0F, 1.0F)) {
    return engine::runtime::kInvalidEntity;
  }
  return ball;
}

bool populate(engine::runtime::World &world, Fixture *out) noexcept {
  const engine::math::Vec3 plate(1.0F, 0.1F, 1.0F);
  const engine::runtime::Entity referee =
      add_object(world, "Referee", engine::math::Vec3(0.0F, 50.0F, 0.0F));
  if ((referee == engine::runtime::kInvalidEntity) ||
      !add_script(world, referee, kRefereeScriptPath)) {
    return false;
  }

  // A heavy plate so the ball resting on it barely nudges it between the
  // script's re-placements; no gravity, so the pin holds it by itself.
  const engine::runtime::Entity pinned =
      add_object(world, "Pinned", engine::math::Vec3(0.0F, 0.0F, 0.0F));
  bool ok = (pinned != engine::runtime::kInvalidEntity) &&
            add_box(world, pinned, plate) &&
            add_body(world, pinned, 0.001F, 0.0F) &&
            add_script(world, pinned, kPinScriptPath);
  out->ballA = add_ball(world, "BallA", engine::math::Vec3(0.0F, 1.0F, 0.0F));

  const engine::runtime::Entity door =
      add_object(world, "Door", engine::math::Vec3(10.0F, 0.0F, 0.0F));
  ok = ok && (door != engine::runtime::kInvalidEntity) &&
       add_box(world, door, plate) && add_script(world, door, kDoorScriptPath);
  out->ballB = add_ball(world, "BallB", engine::math::Vec3(10.0F, 1.0F, 0.0F));

  // The collider is the child's; the body, and so the pair's owner, is
  // the root the script pins.
  const engine::runtime::Entity crane =
      add_object(world, "Crane", engine::math::Vec3(20.0F, 0.0F, 0.0F));
  ok = ok && (crane != engine::runtime::kInvalidEntity) &&
       add_body(world, crane, 0.001F, 0.0F) &&
       add_script(world, crane, kPinScriptPath);
  const engine::runtime::Entity hook =
      add_object(world, "Hook", engine::math::Vec3(0.0F, 0.0F, 0.0F));
  engine::runtime::Transform hookTransform{};
  ok = ok && (hook != engine::runtime::kInvalidEntity) &&
       world.get_transform(hook, &hookTransform);
  hookTransform.parentId = world.persistent_id(crane);
  ok = ok && world.add_transform(hook, hookTransform) &&
       add_box(world, hook, plate);
  out->ballC = add_ball(world, "BallC", engine::math::Vec3(20.0F, 1.0F, 0.0F));

  out->lift = add_object(world, "Lift", engine::math::Vec3(30.0F, 0.0F, 0.0F));
  ok = ok && (out->lift != engine::runtime::kInvalidEntity) &&
       add_box(world, out->lift, plate) &&
       add_body(world, out->lift, 1.0F, 1.0F) &&
       add_script(world, out->lift, kLiftScriptPath);
  out->rider =
      add_object(world, "Rider", engine::math::Vec3(30.0F, 0.3F, 0.0F));
  ok = ok && (out->rider != engine::runtime::kInvalidEntity) &&
       add_box(world, out->rider, engine::math::Vec3(0.2F, 0.2F, 0.2F)) &&
       add_body(world, out->rider, 1.0F, 1.0F);
  // Floats (no gravity) level with the plate, 0.3 m past its leading
  // edge, so the lift reaches it after 0.6 s and pushes it from there.
  out->block =
      add_object(world, "Block", engine::math::Vec3(31.5F, 0.0F, 0.0F));
  ok = ok && (out->block != engine::runtime::kInvalidEntity) &&
       add_box(world, out->block, engine::math::Vec3(0.2F, 0.1F, 0.2F)) &&
       add_body(world, out->block, 1.0F, 0.0F);

  return ok && (out->ballA != engine::runtime::kInvalidEntity) &&
         (out->ballB != engine::runtime::kInvalidEntity) &&
         (out->ballC != engine::runtime::kInvalidEntity);
}

struct Outcome final {
  std::uint64_t hash = 0U;
  engine::math::Vec3 ballA{};
  engine::math::Vec3 ballB{};
  engine::math::Vec3 ballC{};
  engine::math::Vec3 lift{};
  engine::math::Vec3 rider{};
  engine::math::Vec3 block{};
  char pairs[128] = {};
};

bool position_of(engine::runtime::World &world, engine::runtime::Entity entity,
                 engine::math::Vec3 *out) noexcept {
  engine::runtime::Transform transform{};
  if (!world.get_transform(entity, &transform)) {
    return false;
  }
  *out = transform.position;
  return true;
}

bool run(std::uint32_t workers, Outcome *out) noexcept {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = workers;
  if (!engine::bootstrap(config)) {
    std::printf("FAIL: bootstrap with %u workers\n", workers);
    return false;
  }
  bool ok = false;
  {
    engine::EnginePipeline pipeline;
    engine::runtime::World *world = nullptr;
    Fixture fixture{};
    if (pipeline.initialize(0U) && ((world = pipeline.world()) != nullptr)) {
      engine::runtime::reset_world(*world);
      ok = populate(*world, &fixture) &&
           pipeline.set_frame_delta_override(kFrameSeconds);
    }
    for (std::uint32_t frame = 0U; ok && (frame < kFrameCount); ++frame) {
      ok = pipeline.execute_frame();
    }
    ok = ok && position_of(*world, fixture.ballA, &out->ballA) &&
         position_of(*world, fixture.ballB, &out->ballB) &&
         position_of(*world, fixture.ballC, &out->ballC) &&
         position_of(*world, fixture.lift, &out->lift) &&
         position_of(*world, fixture.rider, &out->rider) &&
         position_of(*world, fixture.block, &out->block);
    if (ok) {
      out->hash = world->state_hash(nullptr);
      const char *state = engine::scripting::bindable_get_game_state();
      std::snprintf(out->pairs, sizeof(out->pairs), "%s",
                    (state != nullptr) ? state : "");
    }
    pipeline.teardown();
  }
  engine::shutdown();
  return ok;
}

/// A ball of radius 0.25 resting on a plate whose top is at y = 0.1 sits
/// at 0.35; the solver keeps a few millimetres of penetration slop, and a
/// ball that fell through would be metres below.
bool rests_on_plate(const engine::math::Vec3 &ball) noexcept {
  return std::fabs(ball.y - 0.35F) < 0.02F;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root() ||
      !write_file(kRefereeScriptPath, kRefereeScript) ||
      !write_file(kPinScriptPath, kPinScript) ||
      !write_file(kDoorScriptPath, kDoorScript) ||
      !write_file(kLiftScriptPath, kLiftScript)) {
    std::printf("FAIL: test setup\n");
    return 1;
  }

  Outcome outcomes[3] = {};
  const std::uint32_t workerCounts[3] = {1U, 2U, 8U};
  bool ran = true;
  for (std::size_t i = 0U; ran && (i < 3U); ++i) {
    ran = run(workerCounts[i], &outcomes[i]);
  }
  static_cast<void>(std::remove(kRefereeScriptPath));
  static_cast<void>(std::remove(kPinScriptPath));
  static_cast<void>(std::remove(kDoorScriptPath));
  static_cast<void>(std::remove(kLiftScriptPath));
  if (!ran) {
    std::printf("FAIL: pipeline run\n");
    return 1;
  }

  const Outcome &o = outcomes[0];
  std::printf("[script-moved] ballA y=%g ballB y=%g ballC y=%g lift=(%g,%g) "
              "rider x=%g block x=%g pairs='%s'\n",
              static_cast<double>(o.ballA.y), static_cast<double>(o.ballB.y),
              static_cast<double>(o.ballC.y), static_cast<double>(o.lift.x),
              static_cast<double>(o.lift.y), static_cast<double>(o.rider.x),
              static_cast<double>(o.block.x), o.pairs);

  check(rests_on_plate(o.ballA),
        "a ball rests on a dynamic body a script re-places every step");
  check(rests_on_plate(o.ballB),
        "a ball rests on a collider a script slides every step");
  check(rests_on_plate(o.ballC),
        "a ball rests on the child collider of a body a script pins");
  check(std::strstr(o.pairs, "BallA+Pinned") != nullptr,
        "on_collision reports the ball on the pinned body");
  check(std::strstr(o.pairs, "BallB+Door") != nullptr,
        "on_collision reports the ball on the sliding door");
  check(std::strstr(o.pairs, "BallC+Hook") != nullptr,
        "on_collision reports the ball on the crane's hook");

  // 180 steps of 0.5 m/s * (1/60) s is 1.5 m. The sum is accumulated in
  // float one step at a time, about 21 ulp at x = 31 over 180 adds, far
  // inside 1e-3; any contact response on the lift would be centimetres.
  const float liftTravel = kLiftSpeed * static_cast<float>(kFrameCount) *
                           static_cast<float>(kFrameSeconds);
  check(std::fabs(o.lift.x - (30.0F + liftTravel)) < 1e-3F,
        "the kinematic lift moves exactly as its velocity says");
  check(o.lift.y == 0.0F, "the kinematic lift feels no gravity");
  check(o.rider.x > 30.0F + (0.8F * liftTravel),
        "the lift carries the rider standing on it");
  check(std::fabs(o.rider.y - 0.3F) < 0.02F, "the rider stays on the lift");
  // The lift's leading edge ends at 31 + 1.5 = 32.5; the block's near face
  // must be at or past it.
  check(o.block.x - 0.2F > 32.5F - 0.02F,
        "the lift pushes the block in its path");

  for (std::size_t i = 1U; i < 3U; ++i) {
    if (outcomes[i].hash != o.hash) {
      std::printf("FAIL: state hash at %u workers is %llu, at 1 worker %llu\n",
                  workerCounts[i],
                  static_cast<unsigned long long>(outcomes[i].hash),
                  static_cast<unsigned long long>(o.hash));
      ++g_failures;
    }
  }

  if (g_failures != 0) {
    return 1;
  }
  std::printf("PASS: script-moved bodies collide; hash=%llu\n",
              static_cast<unsigned long long>(o.hash));
  return 0;
}
