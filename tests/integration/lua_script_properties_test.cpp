// Script properties end to end (#134): a real engine::bootstrap() and
// EnginePipeline in player mode load an authored scene where three
// entities share one script that declares five properties. "Tuned" carries
// overrides of four of them, "Plain" carries none, and "Stale" carries a
// value whose type the script no longer declares. Each entity's
// on_begin_play reads its properties with engine.get_property and the last
// to finish reports "ok" or the first check that failed: an override wins,
// a missing one takes the script's default, each keeps Lua's integer or
// float subtype, an integer default of a float property reads as a float,
// a stale value falls back to the default, and an undeclared name is nil.

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
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

constexpr const char *kScriptPath = "lua_script_properties.lua";
constexpr const char *kScenePath = "lua_script_properties.scene";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

constexpr const char *kScript =
    "local M = {}\n"
    "M.properties = {\n"
    "    speed = 0.25,\n"
    "    lives = { type = \"integer\", default = 3, min = 0 },\n"
    "    label = \"none\",\n"
    "    enabled = true,\n"
    "    amplitude = { type = \"float\", default = 2 },\n"
    "}\n"
    "local done = 0\n"
    "local function expect(ok, what)\n"
    "    if not ok then engine.set_game_state(what) error(what, 2) end\n"
    "end\n"
    "local function get(self, name) return engine.get_property(self, name) "
    "end\n"
    "function M.on_begin_play(self)\n"
    "    local tuned = engine.find_entity_by_name(\"Tuned\")\n"
    "    if self == tuned then\n"
    "        expect(tostring(get(self, \"speed\")) == \"3.0\", \"tuned "
    "speed\")\n"
    "        expect(tostring(get(self, \"lives\")) == \"5\", \"tuned lives\")\n"
    "        expect(get(self, \"label\") == \"Bob\", \"tuned label\")\n"
    "        expect(get(self, \"enabled\") == false, \"tuned enabled\")\n"
    "    else\n"
    "        expect(tostring(get(self, \"speed\")) == \"0.25\", \"default "
    "speed\")\n"
    "        expect(tostring(get(self, \"lives\")) == \"3\", \"default "
    "lives\")\n"
    "        expect(get(self, \"label\") == \"none\", \"default label\")\n"
    "        expect(get(self, \"enabled\") == true, \"default enabled\")\n"
    "    end\n"
    "    expect(tostring(get(self, \"amplitude\")) == \"2.0\", \"float "
    "default\")\n"
    "    expect(get(self, \"missing\") == nil, \"undeclared is nil\")\n"
    "    done = done + 1\n"
    "    if done == 3 then engine.set_game_state(\"ok\") end\n"
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

engine::runtime::ScriptPropertyValue value_of(float value) noexcept {
  engine::runtime::ScriptPropertyValue out{};
  out.type = engine::runtime::ScriptPropertyType::Float;
  out.floatValue = value;
  return out;
}

/// Adds a named scene object running the script, with `overrides`.
bool add_scripted(
    engine::runtime::World &world, const char *name,
    const engine::runtime::ScriptPropertiesComponent *overrides) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s",
                kScriptPath);
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_name_component(entity, nameComponent) &&
         world.add_script_component(entity, script) &&
         ((overrides == nullptr) ||
          world.add_script_properties(entity, *overrides));
}

bool write_scene() noexcept {
  using engine::runtime::ScriptPropertyType;
  using engine::runtime::ScriptPropertyValue;
  std::unique_ptr<engine::runtime::World> author(new (std::nothrow)
                                                     engine::runtime::World());
  if (author == nullptr) {
    return false;
  }
  engine::runtime::ScriptPropertiesComponent tuned{};
  ScriptPropertyValue lives{};
  lives.type = ScriptPropertyType::Integer;
  lives.integerValue = 5;
  ScriptPropertyValue label{};
  label.type = ScriptPropertyType::String;
  std::snprintf(label.text, sizeof(label.text), "%s", "Bob");
  ScriptPropertyValue enabled{};
  enabled.type = ScriptPropertyType::Bool;
  enabled.boolValue = false;
  engine::runtime::ScriptPropertiesComponent stale{};
  ScriptPropertyValue wrongType{};
  wrongType.type = ScriptPropertyType::String;
  std::snprintf(wrongType.text, sizeof(wrongType.text), "%s", "far");
  return engine::runtime::script_properties_set(&tuned, 0U, "speed",
                                                value_of(3.0F)) &&
         engine::runtime::script_properties_set(&tuned, 0U, "lives", lives) &&
         engine::runtime::script_properties_set(&tuned, 0U, "label", label) &&
         engine::runtime::script_properties_set(&tuned, 0U, "enabled", enabled) &&
         engine::runtime::script_properties_set(&stale, 0U, "amplitude",
                                                wrongType) &&
         add_scripted(*author, "Tuned", &tuned) &&
         add_scripted(*author, "Plain", nullptr) &&
         add_scripted(*author, "Stale", &stale) &&
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
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0),
          "the frame delta override is accepted");
    for (int frame = 0; (frame < 10) && (std::strcmp(game_state(), "ok") != 0);
         ++frame) {
      CHECK(pipeline.execute_frame(), "frame runs");
    }
    const std::string state = game_state();
    CHECK(state == "ok", "every property check in the script passed");
    if (state != "ok") {
      std::fprintf(stderr, "  game state: \"%s\"\n", state.c_str());
    }
    pipeline.teardown();
  }
  engine::shutdown();
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_script_properties_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("lua_script_properties_test: all checks passed\n");
  return 0;
}
