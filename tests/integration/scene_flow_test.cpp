// Verifies the scene transition flow end to end: a scripted entity in
// scene A requests engine.load_scene, the pipeline-side pending-op
// processor dispatches on_end_play to scene A's outgoing entities, commits
// scene B into the live world, scene B's scripts fire on_begin_play, and
// Lua globals survive the transition (the cross-scene state channel the
// templates use) while the entity handle does not. Also pins
// engine.new_scene teardown, including its own on_end_play dispatch.
//
// Contract migration (#198, 2026-08-16): before this change, neither
// load_scene nor new_scene dispatched on_end_play to the outgoing scene's
// scripted entities (only editor Stop did), so a script that released
// resources or saved state in on_end_play silently skipped that path on
// every script-driven transition. This file did not previously assert
// on_end_play behavior either way; it now pins the corrected contract —
// on_end_play fires for both ops, before the outgoing entities are torn
// down. See tests/integration/scene_transition_end_play_test.cpp for the
// full boundary matrix (many entities, same-frame destroy, repeated
// transitions, failed load, reentrancy).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kSceneAScript = "scene_flow_a.lua";
constexpr const char *kSceneBScript = "scene_flow_b.lua";
constexpr const char *kHelperScript = "scene_flow_helper.lua";
constexpr const char *kSceneBFile = "scene_flow_b.scene.json";

/// Writes one text file for a script or scene fixture.
bool write_text_file(const char *path, const char *contents) noexcept {
  FILE *file = nullptr;
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
  const std::size_t length = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

/// Deletes every fixture this test writes.
void cleanup_files() noexcept {
  static_cast<void>(std::remove(kSceneAScript));
  static_cast<void>(std::remove(kSceneBScript));
  static_cast<void>(std::remove(kHelperScript));
  static_cast<void>(std::remove(kSceneBFile));
}

/// Fires begin-play dispatch and applies the deferred mutations exactly
/// like the pipeline's play stages do.
void dispatch_begin_play(engine::runtime::World &world) noexcept {
  world.begin_begin_play_phase();
  engine::scripting::dispatch_entity_scripts_begin_play(&world);
  world.end_begin_play_phase();
  engine::scripting::flush_deferred_mutations();
}

/// Adds a named, scripted scene object to the world.
bool add_scripted_entity(engine::runtime::World &world, const char *name,
                         const char *scriptPath) noexcept {
  const auto entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if (!world.add_name_component(entity, nameComponent)) {
    return false;
  }
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                scriptPath);
  return world.add_script_component(entity, script);
}

} // namespace

/// Runs this executable or test program.
int main() {
  cleanup_files();
  static_cast<void>(engine::core::initialize_logging());

  if (!engine::scripting::initialize_scripting()) {
    std::puts("scripting init failed");
    return 1;
  }

  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::AssetManager> assetManager(
      new (std::nothrow) engine::renderer::AssetManager());
  if ((world == nullptr) || (assetDatabase == nullptr) ||
      (assetManager == nullptr)) {
    engine::scripting::shutdown_scripting();
    return 1;
  }
  engine::renderer::clear_asset_database(assetDatabase.get());
  engine::renderer::clear_asset_manager(assetManager.get());
  engine::core::ServiceLocator locator{};
  engine::runtime::EngineAssetDatabaseService assetService{};
  assetService.database = assetDatabase.get();
  assetService.manager = assetManager.get();
  if (!locator.register_service<engine::runtime::EngineAssetDatabaseService>(
          &assetService)) {
    engine::scripting::shutdown_scripting();
    return 1;
  }
  engine::runtime::bind_scripting_runtime(world.get(), locator);
  world->end_frame_phase();

  int result = 0;

  // Scene A's script hands a value to scene B through a Lua global and
  // requests the transition; scene B's script consumes the global on
  // begin play and surfaces it as its entity name so the test can read
  // it back through the world. Each script also marks a Lua global from
  // on_end_play (#198): the VM persists across the transition (globals are
  // the cross-scene handoff channel) but the entity handle `self` does
  // not, so the marker itself must be a global, set before this world's
  // content is replaced.
  const char *sceneAScript =
      "local M = {}\n"
      "function M.on_begin_play(self)\n"
      "    handoff = 7\n"
      "    engine.load_scene(\"scene_flow_b.scene.json\")\n"
      "end\n"
      "function M.on_end_play(self)\n"
      "    scene_a_end_play = true\n"
      "end\n"
      "return M\n";
  const char *sceneBScript =
      "local M = {}\n"
      "function M.on_begin_play(self)\n"
      "    local started = (handoff or 0) + 1\n"
      "    engine.set_name(self, \"goal_\" .. tostring(started))\n"
      "end\n"
      "function M.on_end_play(self)\n"
      "    scene_b_end_play = true\n"
      "end\n"
      "return M\n";
  const char *helperScript =
      "function request_new_scene()\n"
      "    engine.new_scene()\n"
      "end\n"
      "function assert_scene_a_end_play()\n"
      "    if scene_a_end_play ~= true then\n"
      "        error('scene A on_end_play did not fire before load_scene ' ..\n"
      "              'committed (#198)')\n"
      "    end\n"
      "end\n"
      "function assert_scene_b_end_play()\n"
      "    if scene_b_end_play ~= true then\n"
      "        error('scene B on_end_play did not fire before new_scene ' ..\n"
      "              'committed (#198)')\n"
      "    end\n"
      "end\n";
  if (!write_text_file(kSceneAScript, sceneAScript) ||
      !write_text_file(kSceneBScript, sceneBScript) ||
      !write_text_file(kHelperScript, helperScript) ||
      !engine::scripting::load_script(kHelperScript)) {
    std::puts("fixture write failed");
    result = 1;
  }

  // Build scene B first and save it, then reset into scene A.
  if ((result == 0) &&
      (!add_scripted_entity(*world, "GoalB", kSceneBScript) ||
       !engine::runtime::save_scene(*world, kSceneBFile))) {
    std::puts("scene B setup failed");
    result = 1;
  }
  if (result == 0) {
    engine::runtime::reset_world(*world);
    if (!add_scripted_entity(*world, "StarterA", kSceneAScript)) {
      std::puts("scene A setup failed");
      result = 1;
    }
  }

  // Scene A begins play: its script requests the transition.
  if (result == 0) {
    dispatch_begin_play(*world);
    if (!engine::scripting::has_pending_scene_op()) {
      std::puts("scene A did not request a transition");
      result = 1;
    }
  }

  // The pipeline-side processor commits scene B into the live world.
  if ((result == 0) &&
      !engine::runtime::process_pending_scene_op(*world)) {
    std::puts("pending scene op failed");
    result = 1;
  }
  if (result == 0) {
    if (world->find_entity_by_name("GoalB") ==
        engine::runtime::kInvalidEntity) {
      std::puts("scene B entity missing after transition");
      result = 1;
    }
    if (world->find_entity_by_name("StarterA") !=
        engine::runtime::kInvalidEntity) {
      std::puts("scene A entity survived the transition");
      result = 1;
    }
    // #198: scene A's on_end_play must have fired before scene B committed.
    if (!engine::scripting::call_script_function("assert_scene_a_end_play")) {
      std::puts("scene A did not receive on_end_play before load_scene");
      result = 1;
    }
  }

  // Scene B's scripts fire begin play and read the cross-scene global:
  // handoff (7) + 1 must surface as the entity name "goal_8".
  if (result == 0) {
    dispatch_begin_play(*world);
    if (world->find_entity_by_name("goal_8") ==
        engine::runtime::kInvalidEntity) {
      std::puts("cross-scene handoff mismatch (goal_8 missing)");
      result = 1;
    }
  }

  // engine.new_scene tears the world down to empty via the same path.
  if (result == 0) {
    if (!engine::scripting::call_script_function("request_new_scene")) {
      std::puts("new-scene helper failed");
      result = 1;
    } else if (!engine::runtime::process_pending_scene_op(*world)) {
      std::puts("new-scene op failed");
      result = 1;
    } else if (world->alive_entity_count() != 0U) {
      std::puts("new scene left entities alive");
      result = 1;
    } else if (!engine::scripting::call_script_function(
                   "assert_scene_b_end_play")) {
      // #198: scene B's on_end_play must have fired before new_scene
      // cleared the world.
      std::puts("scene B did not receive on_end_play before new_scene");
      result = 1;
    }
  }

  engine::runtime::unbind_scripting_runtime(locator);
  engine::scripting::shutdown_scripting();
  cleanup_files();
  return result;
}
