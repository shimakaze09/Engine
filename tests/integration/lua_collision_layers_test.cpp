// Named collision layers end to end: a real engine::bootstrap() and
// EnginePipeline in player mode run a project whose layers name bit 3
// "Player" and bit 4 "Enemy" and ignore that pair. The pipeline's World
// carries the project's matrix, and a Lua script that begins play checks:
// - engine.layer_mask ORs named bits, engine.layer_bit and layer_name
//   agree, and an unknown name is a Lua error, never a nil mask;
// - engine.raycast takes a mask after skip_entity and raycast_all one
//   before it, so `~engine.layer_mask("Enemy")` sees past the Enemy wall;
// - a query mask that is not an integer fails the query instead of
//   hitting every layer, as it did when such a mask read as all bits;
// - get_collision_layer and get_collision_mask read back what the setters
//   wrote, and a fractional layer is refused, not truncated.

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
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

constexpr const char *kScriptPath = "lua_collision_layers.lua";
constexpr const char *kScenePath = "lua_collision_layers.scene";
// One letter per check, a to l in the script's order: the game state holds
// at most 63 characters.
constexpr const char *kExpected = "abcdefghijkl";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

// Each passing check appends its letter, so the final state names exactly
// which ones failed by their absence.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    local passed = {}\n"
    "    local function expect(ok, name)\n"
    "        if ok then passed[#passed + 1] = name end\n"
    "    end\n"
    "    expect(engine.layer_mask(\"Player\", \"enemy\") == (8 | 16), "
    "\"a\")\n"
    "    expect(engine.layer_bit(\"ENEMY\") == 4, \"b\")\n"
    "    expect(engine.layer_name(3) == \"Player\" and\n"
    "           engine.layer_name(5) == nil, \"c\")\n"
    "    local ok, err = pcall(engine.layer_mask, \"Player\", \"Ghost\")\n"
    "    expect(not ok and string.find(err, \"argument 2\", 1, true) ~= nil,\n"
    "           \"d\")\n"
    "    local hit = engine.raycast(0, 0, 0, 1, 0, 0, 50)\n"
    "    expect(hit ~= nil and engine.get_name(hit) == \"Wall\", \"e\")\n"
    "    local past = engine.raycast(0, 0, 0, 1, 0, 0, 50, nil,\n"
    "                                ~engine.layer_mask(\"Enemy\"))\n"
    "    expect(past ~= nil and engine.get_name(past) == \"Ground\",\n"
    "           \"f\")\n"
    "    local hits = engine.raycast_all(0, 0, 0, 1, 0, 0, 50,\n"
    "                                    engine.layer_mask(\"Enemy\"))\n"
    "    expect(#hits == 1 and engine.get_name(hits[1].entity) == \"Wall\",\n"
    "           \"g\")\n"
    "    expect(#engine.raycast_all(0, 0, 0, 1, 0, 0, 50, \"Enemy\") == 0,\n"
    "           \"h\")\n"
    "    expect(engine.raycast(0, 0, 0, 1, 0, 0, 50, nil, 2.5) == nil,\n"
    "           \"i\")\n"
    "    local wall = engine.find_entity_by_name(\"Wall\")\n"
    "    expect(engine.get_collision_layer(wall) == 16, \"j\")\n"
    "    expect(engine.set_collision_mask(wall, "
    "~engine.layer_mask(\"Player\"))\n"
    "           and engine.get_collision_mask(wall) == 0xFFFFFFF7,\n"
    "           \"k\")\n"
    "    expect(engine.set_collision_layer(wall, 1.5) == false and\n"
    "           engine.get_collision_layer(wall) == 16, \"l\")\n"
    "    engine.set_game_state(table.concat(passed))\n"
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

/// Adds a named static box at `x` on `layer` to the authoring world.
bool add_named_box(engine::runtime::World &world, const char *name, float x,
                   std::uint32_t layer) noexcept {
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(x, 0.0F, 0.0F);
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 2.0F, 2.0F);
  collider.collisionLayer = layer;
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_name_component(entity, nameComponent) &&
         world.add_collider(entity, collider);
}

/// Authors the scene: an Enemy-layer "Wall" at x = 5, a default-layer
/// "Ground" block at x = 10, and the scripted entity.
bool write_scene() noexcept {
  std::unique_ptr<engine::runtime::World> author(new (std::nothrow)
                                                     engine::runtime::World());
  if (author == nullptr) {
    return false;
  }
  const engine::runtime::Entity scripted = author->create_scene_object();
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s",
                kScriptPath);
  return add_named_box(*author, "Wall", 5.0F, 1U << 4U) &&
         add_named_box(*author, "Ground", 10.0F, 1U) &&
         (scripted != engine::runtime::kInvalidEntity) &&
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
  std::snprintf(config.collisionLayers.names[3],
                sizeof(config.collisionLayers.names[3]), "%s", "Player");
  std::snprintf(config.collisionLayers.names[4],
                sizeof(config.collisionLayers.names[4]), "%s", "Enemy");
  engine::content::set_collision_layer_pair(&config.collisionLayers, 3U, 4U,
                                            false);
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
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0),
          "the frame delta override is accepted");
    for (int frame = 0;
         (frame < 30) && (std::strcmp(game_state(), kExpected) != 0); ++frame) {
      CHECK(pipeline.execute_frame(), "frame runs");
    }
    const std::string finalState = game_state();
    CHECK(finalState == kExpected, "every layer check in the script passed");
    if (finalState != kExpected) {
      std::fprintf(stderr, "  passed: \"%s\"\n", finalState.c_str());
    }

    engine::runtime::World *world = pipeline.world();
    CHECK((world != nullptr) &&
              (engine::physics::get_collision_matrix(*world).rows[3] ==
               ~(1U << 4U)) &&
              (engine::physics::get_collision_matrix(*world).rows[4] ==
               ~(1U << 3U)),
          "the run's World carries the project's matrix after the scene "
          "load");
    pipeline.teardown();
  }
  engine::shutdown();
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_collision_layers_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("lua_collision_layers_test: all checks passed\n");
  return 0;
}
