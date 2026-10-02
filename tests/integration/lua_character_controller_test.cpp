// The character controller end to end: a real engine::bootstrap() and
// EnginePipeline in player mode load an authored scene through the
// production serializer, with a floor, a 0.2 m step from x = 3 and a wall
// whose face is at x = 7.5. The character, a Capsule Collider with a
// CharacterControllerComponent, starts a metre up; its Lua on_fixed_tick
// applies gravity itself and walks it along +x with engine.move_character.
// The script checks the API surface (the settings read back, an
// out-of-range setting and a non-finite move are refused), and the run
// checks the character fell to the floor, climbed the step, stopped its
// skin short of the wall standing on the step, and reported each. Two runs,
// at 1 and 4 workers, end on the same bits.

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace {

constexpr const char *kScriptPath = "lua_character_controller.lua";
constexpr const char *kScenePath = "lua_character_controller.scene";
// One letter per check, a to i in the script's order: the game state
// holds at most 63 characters.
constexpr const char *kExpected = "abcdefghi";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

// The character walks at 3 m/s and falls under 9.8 m/s^2 it applies
// itself; after 240 fixed steps (4 s, 12 m of intended walking) it is
// against the wall, on the step.
constexpr const char *kScript =
    "local M = {}\n"
    "local passed = {}\n"
    "local function expect(ok, letter)\n"
    "    if ok then passed[#passed + 1] = letter end\n"
    "end\n"
    "local vy = 0.0\n"
    "local ticks = 0\n"
    "local landed = false\n"
    "local climbed = false\n"
    "local last_flags = 0\n"
    "local last_ground = nil\n"
    "function M.on_begin_play(self)\n"
    "    local slope, step, skin = engine.get_character_controller(self)\n"
    "    expect(slope == 45 and math.abs(step - 0.3) < 1e-6 and\n"
    "           math.abs(skin - 0.02) < 1e-6, \"a\")\n"
    "    expect(engine.add_character_controller(self, 50) and\n"
    "           engine.get_character_controller(self) == 50 and\n"
    "           engine.add_character_controller(self, 45), \"b\")\n"
    "    expect(engine.add_character_controller(self, 95) == false and\n"
    "           engine.get_character_controller(self) == 45, \"c\")\n"
    "    expect(engine.move_character(self, 0 / 0, 0, 0) == nil, \"d\")\n"
    "end\n"
    "function M.on_fixed_tick(self, dt)\n"
    "    ticks = ticks + 1\n"
    "    if ticks > 240 then return end\n"
    "    vy = vy - 9.8 * dt\n"
    "    local grounded, flags, ground =\n"
    "        engine.move_character(self, 3.0 * dt, vy * dt, 0.0)\n"
    "    if grounded then\n"
    "        vy = 0.0\n"
    "        landed = true\n"
    "    end\n"
    "    local _, y = engine.get_position(self)\n"
    "    if y > 0.15 then climbed = true end\n"
    "    last_flags = flags\n"
    "    last_ground = ground\n"
    "    if ticks == 240 then\n"
    "        local x\n"
    "        x, y = engine.get_position(self)\n"
    "        expect(landed and engine.is_grounded(self), \"e\")\n"
    "        expect(climbed and math.abs(y - 0.22) < 0.005, \"f\")\n"
    "        expect(math.abs(x - 7.18) < 0.01, \"g\")\n"
    "        expect((last_flags & engine.COLLIDED_SIDES) ~= 0 and\n"
    "               (last_flags & engine.COLLIDED_BELOW) ~= 0, \"h\")\n"
    "        expect(last_ground ~= nil and\n"
    "               engine.get_name(last_ground) == \"Step\", \"i\")\n"
    "        engine.set_game_state(table.concat(passed))\n"
    "    end\n"
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

/// Adds a named static box.
bool add_box(engine::runtime::World &world, const char *name,
             const engine::math::Vec3 &position,
             const engine::math::Vec3 &halfExtents) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  engine::runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_name_component(entity, nameComponent) &&
         world.add_collider(entity, collider);
}

/// Authors the floor (top at y = 0), the step (top at 0.2 from x = 3), the
/// wall (face at x = 7.5) and the character, its feet at (0, 1, 0).
bool write_scene() noexcept {
  std::unique_ptr<engine::runtime::World> author(new (std::nothrow)
                                                     engine::runtime::World());
  if (author == nullptr) {
    return false;
  }
  using engine::math::Vec3;
  engine::runtime::Transform feet{};
  feet.position = Vec3(0.0F, 1.0F, 0.0F);
  const engine::runtime::Entity hero = author->create_scene_object(feet);
  engine::runtime::Collider capsule{};
  capsule.shape = engine::runtime::ColliderShape::Capsule;
  capsule.halfExtents = Vec3(0.3F, 0.6F, 0.3F);
  capsule.localPosition = Vec3(0.0F, 0.9F, 0.0F);
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                kScriptPath);
  return add_box(*author, "Floor", Vec3(0.0F, -0.5F, 0.0F),
                 Vec3(30.0F, 0.5F, 30.0F)) &&
         add_box(*author, "Step", Vec3(10.0F, 0.1F, 0.0F),
                 Vec3(7.0F, 0.1F, 5.0F)) &&
         add_box(*author, "Wall", Vec3(8.0F, 2.0F, 0.0F),
                 Vec3(0.5F, 2.0F, 5.0F)) &&
         (hero != engine::runtime::kInvalidEntity) &&
         author->add_collider(hero, capsule) &&
         author->add_character_controller(
             hero, engine::runtime::CharacterControllerComponent{}) &&
         author->add_script_component(hero, script) &&
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

struct RunResult final {
  bool ran = false;
  std::string state;
  engine::math::Vec3 position{};
};

/// Runs the scene for 250 frames at `workers` workers.
RunResult run(std::uint32_t workers) noexcept {
  RunResult result{};
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = workers;
  config.editorScenePath = kScenePath;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap at %u workers\n", workers);
    return result;
  }
  {
    engine::EnginePipeline pipeline;
    if (pipeline.initialize(0U) &&
        pipeline.set_frame_delta_override(1.0 / 60.0)) {
      result.ran = true;
      for (int frame = 0; frame < 250; ++frame) {
        result.ran = pipeline.execute_frame() && result.ran;
      }
      result.state = game_state();
      engine::runtime::World *world = pipeline.world();
      if (world != nullptr) {
        world->for_each_alive([&](engine::runtime::Entity entity) {
          if (world->has_character_controller(entity)) {
            engine::runtime::Transform transform{};
            static_cast<void>(world->get_transform(entity, &transform));
            result.position = transform.position;
          }
        });
      }
    }
    pipeline.teardown();
  }
  engine::shutdown();
  return result;
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

  const RunResult one = run(1U);
  const RunResult four = run(4U);
  CHECK(one.ran && four.ran, "both runs execute every frame");
  CHECK(one.state == kExpected,
        "every character check in the script passed (1 worker)");
  if (one.state != kExpected) {
    std::fprintf(stderr, "  passed: \"%s\" at %f %f %f\n", one.state.c_str(),
                 one.position.x, one.position.y, one.position.z);
  }
  CHECK(std::memcmp(&one.position, &four.position,
                    sizeof(engine::math::Vec3)) == 0,
        "1 and 4 workers end the character on the same bits");
  CHECK(four.state == one.state, "both runs report the same checks");
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_character_controller_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("lua_character_controller_test: all checks passed\n");
  return 0;
}
