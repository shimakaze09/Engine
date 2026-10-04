// Entity tags end to end: a real engine::bootstrap() and EnginePipeline in
// player mode load an authored scene whose coins carry the tag "coin" and
// whose enemy carries "enemy". A script that begins play queries, adds,
// rejects, removes and lists tags through the engine.* functions and
// reports "ok" or the first check that failed. The World then shows the
// script's edits: the enemy, stripped of its last tag, no longer carries a
// Tags component, and the coins' authored tags are untouched.

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

constexpr const char *kScriptPath = "lua_entity_tags.lua";
constexpr const char *kScenePath = "lua_entity_tags.scene";

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
    "local function expect(ok, what)\n"
    "    if not ok then error(what, 2) end\n"
    "end\n"
    "function M.on_begin_play(self)\n"
    "    local coinA = engine.find_entity_by_name(\"CoinA\")\n"
    "    local coinB = engine.find_entity_by_name(\"CoinB\")\n"
    "    local enemy = engine.find_entity_by_name(\"Enemy\")\n"
    "    local list, total = engine.find_entities_by_tag(\"COIN\")\n"
    "    expect(total == 2 and #list == 2, \"two coins\")\n"
    "    expect(list[1] == coinA and list[2] == coinB, \"index order\")\n"
    "    expect(engine.add_tag(enemy, \"coin\") == true, \"add\")\n"
    "    expect(engine.add_tag(enemy, \"Coin\") == true, \"repeat is ok\")\n"
    "    local ok, why = engine.add_tag(enemy, \"not a tag\")\n"
    "    expect(ok == false and type(why) == \"string\", \"invalid refused\")\n"
    "    list, total = engine.find_entities_by_tag(\"coin\")\n"
    "    expect(total == 3 and list[3] == enemy, \"enemy found\")\n"
    "    expect(engine.has_tag(enemy, \"COIN\"), \"has ignoring case\")\n"
    "    expect(engine.remove_tag(enemy, \"coin\"), \"remove\")\n"
    "    expect(not engine.has_tag(enemy, \"coin\"), \"removed\")\n"
    "    expect(not engine.remove_tag(enemy, \"coin\"), \"remove absent\")\n"
    "    local tags = engine.get_tags(enemy)\n"
    "    expect(#tags == 1 and tags[1] == \"enemy\", \"get_tags\")\n"
    "    expect(engine.remove_tag(enemy, \"enemy\"), \"remove last\")\n"
    "    expect(#engine.get_tags(enemy) == 0, \"no tags left\")\n"
    "    expect(engine.get_tags(coinA)[1] == \"coin\", \"coin untouched\")\n"
    "    engine.set_game_state(\"ok\")\n"
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

/// Adds a named scene object carrying one tag.
bool add_tagged(engine::runtime::World &world, const char *name,
                const char *tag) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  engine::runtime::TagSetComponent tags{};
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_name_component(entity, nameComponent) &&
         (engine::runtime::tag_set_add(&tags, tag) ==
          engine::runtime::TagSetAdd::Added) &&
         world.add_tag_set_component(entity, tags);
}

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
  return add_tagged(*author, "CoinA", "coin") &&
         add_tagged(*author, "CoinB", "Coin") &&
         add_tagged(*author, "Enemy", "enemy") &&
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
    CHECK(state == "ok", "every tag check in the script passed");
    if (state != "ok") {
      std::fprintf(stderr, "  game state: \"%s\"\n", state.c_str());
    }

    engine::runtime::World *world = pipeline.world();
    CHECK(world != nullptr, "the pipeline has a world");
    if (world != nullptr) {
      const engine::runtime::Entity enemy = world->find_entity_by_name("Enemy");
      CHECK(world->get_tag_set_component_ptr(enemy) == nullptr,
            "removing the last tag removed the Tags component");
      CHECK(world->find_entities_by_tag("coin", nullptr, 0U) == 2U,
            "the coins keep their authored tags");
    }
    pipeline.teardown();
  }
  engine::shutdown();
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_entity_tags_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("lua_entity_tags_test: all checks passed\n");
  return 0;
}
