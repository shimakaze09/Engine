// Trigger colliders end to end: a real engine::bootstrap() and
// EnginePipeline in player mode load an authored scene through the
// production serializer, and a Lua script that begins play registers
// engine.on_trigger_handler, turns a solid wall into a trigger with
// engine.set_trigger, and removes a second handler. A ball crossing the
// authored trigger and then the converted wall must deliver exactly
// "Zone:enter;Zone:exit;Wall:enter;Wall:exit;" to the handler, with the
// trigger first and the ball second, and keep its velocity throughout.

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace {

constexpr const char *kScriptPath = "lua_trigger_events.lua";
constexpr const char *kScenePath = "lua_trigger_events.scene";
constexpr const char *kExpected = "Zone:enter;Zone:exit;Wall:enter;Wall:exit;";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

// Begins play by converting the wall, registering the recording handler,
// then registering and removing a second one: if removal failed, the
// second handler (dispatched after the first) would overwrite the log.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    local wall = engine.find_entity_by_name(\"Wall\")\n"
    "    if not engine.set_trigger(wall, true) or\n"
    "       not engine.is_trigger(wall) then\n"
    "        engine.set_game_state(\"set_trigger failed\")\n"
    "        return\n"
    "    end\n"
    "    local log = \"\"\n"
    "    engine.on_trigger_handler(function(trigger, other, phase)\n"
    "        if not engine.is_trigger(trigger) or engine.is_trigger(other) "
    "then\n"
    "            engine.set_game_state(\"wrong roles\")\n"
    "            return\n"
    "        end\n"
    "        log = log .. engine.get_name(trigger) .. \":\" .. phase .. \";\"\n"
    "        engine.set_game_state(log)\n"
    "    end)\n"
    "    local removed = engine.on_trigger_handler(function()\n"
    "        engine.set_game_state(\"removed handler ran\")\n"
    "    end)\n"
    "    engine.remove_trigger_handler(removed)\n"
    "end\n"
    "return M\n";

bool write_script() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(kScript);
  const bool ok = std::fwrite(kScript, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

void remove_fixtures() noexcept {
  static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kScenePath));
}

/// Adds a named collider entity at `x` to the authoring world.
engine::runtime::Entity add_named_box(engine::runtime::World &world,
                                      const char *name, float x,
                                      bool isTrigger) noexcept {
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(x, 0.0F, 0.0F);
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  collider.isTrigger = isTrigger;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_name_component(entity, nameComponent) ||
      !world.add_collider(entity, collider)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// Authors the scene: a trigger "Zone" at the origin, a solid "Wall" at
/// x = 6 the script turns into a trigger, a ball at x = -3.05 moving at
/// 12 m/s (0.2 m per step, below the CCD engagement threshold) under zero
/// gravity, and the scripted entity.
bool write_scene() noexcept {
  std::unique_ptr<engine::runtime::World> author(new (std::nothrow)
                                                     engine::runtime::World());
  if (author == nullptr) {
    return false;
  }
  engine::runtime::set_gravity(*author, 0.0F, 0.0F, 0.0F);
  const engine::runtime::Entity zone =
      add_named_box(*author, "Zone", 0.0F, true);
  const engine::runtime::Entity wall =
      add_named_box(*author, "Wall", 6.0F, false);

  engine::runtime::Transform ballTransform{};
  ballTransform.position = engine::math::Vec3(-3.05F, 0.0F, 0.0F);
  const engine::runtime::Entity ball =
      author->create_scene_object(ballTransform);
  engine::runtime::Collider ballCollider{};
  ballCollider.shape = engine::runtime::ColliderShape::Sphere;
  ballCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  body.velocity = engine::math::Vec3(12.0F, 0.0F, 0.0F);

  const engine::runtime::Entity scripted = author->create_scene_object();
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s",
                kScriptPath);
  return (zone != engine::runtime::kInvalidEntity) &&
         (wall != engine::runtime::kInvalidEntity) &&
         (ball != engine::runtime::kInvalidEntity) &&
         (scripted != engine::runtime::kInvalidEntity) &&
         author->add_collider(ball, ballCollider) &&
         author->add_rigid_body(ball, body) &&
         author->add_script_component(scripted, script) &&
         engine::runtime::save_scene(*author, kScenePath);
}

void set_player_env() noexcept {
#ifdef _WIN32
  static_cast<void>(_putenv_s("ENGINE_CVAR_app_player_mode", "1"));
#else
  static_cast<void>(setenv("ENGINE_CVAR_app_player_mode", "1", 1));
#endif
}

const char *game_state() noexcept {
  const char *state = engine::scripting::bindable_get_game_state();
  return (state != nullptr) ? state : "";
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_script() || !write_scene()) {
    std::fprintf(stderr, "FAIL: write script and scene fixtures\n");
    remove_fixtures();
    return 1;
  }

  set_player_env();
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.editorScenePath = kScenePath;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_fixtures();
    return 2;
  }

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_fixtures();
      return 3;
    }
    // One fixed step per frame, independent of the wall clock.
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0),
          "the frame delta override is accepted");

    // The ball's center reaches x = 8.5 (past the wall's far face plus its
    // radius) after 58 steps; 120 frames leave room for scene load and
    // begin play.
    for (int frame = 0;
         (frame < 120) && (std::strcmp(game_state(), kExpected) != 0);
         ++frame) {
      CHECK(pipeline.execute_frame(), "frame runs");
    }
    std::string finalState = game_state();
    CHECK(finalState == kExpected,
          "the handler saw enter and exit for the authored trigger, then for "
          "the wall the script made a trigger, with roles (trigger, other)");
    if (finalState != kExpected) {
      std::fprintf(stderr, "  game state: \"%s\"\n", finalState.c_str());
    }

    engine::runtime::World *world = pipeline.world();
    bool ballKeptSpeed = false;
    if (world != nullptr) {
      const engine::runtime::Entity *entities = nullptr;
      engine::runtime::RigidBody *bodies = nullptr;
      if ((world->rigid_body_count() == 1U) &&
          world->get_rigid_body_range(0U, 1U, &entities, &bodies)) {
        ballKeptSpeed = (bodies[0].velocity.x == 12.0F);
      }
    }
    CHECK(ballKeptSpeed, "neither trigger slowed the ball");
    pipeline.teardown();
  }
  engine::shutdown();
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_trigger_events_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("lua_trigger_events_test: all checks passed\n");
  return 0;
}
