// Verifies the declared scene-transition reset protocol: transient
// script execution state (coroutines) from the outgoing scene must not
// keep acting after process_pending_scene_op commits the replacement
// world, for both load_scene and new_scene ops and across repeated
// transitions.

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

constexpr const char *kSceneAScript = "scene_reset_a.lua";
constexpr const char *kSceneBScript = "scene_reset_b.lua";
constexpr const char *kSceneBFile = "scene_reset_b.scene.json";

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

/// Ticks coroutines and applies their deferred world mutations like the
/// pipeline's scripting stage does.
void tick_frame() noexcept {
  engine::scripting::tick_coroutines();
  engine::scripting::flush_deferred_mutations();
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
  engine::scripting::set_frame_time(0.016F, 0.016F);

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

  const char *sceneAScript =
      "local M = {}\n"
      "function M.on_begin_play(self)\n"
      "    engine.start_coroutine(function()\n"
      "        while true do\n"
      "            leak = (leak or 0) + 1\n"
      "            engine.wait(0)\n"
      "        end\n"
      "    end)\n"
      "    engine.load_scene(\"scene_reset_b.scene.json\")\n"
      "end\n"
      "return M\n";
  const char *sceneBScript =
      "local M = {}\n"
      "function M.on_begin_play(self)\n"
      "    engine.set_name(self, \"leak_\" .. tostring(leak or 0))\n"
      "end\n"
      "return M\n";
  if (!write_text_file(kSceneAScript, sceneAScript) ||
      !write_text_file(kSceneBScript, sceneBScript)) {
    std::puts("fixture write failed");
    result = 1;
  }

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

  if (result == 0) {
    dispatch_begin_play(*world);
    tick_frame();
    if (!engine::scripting::has_pending_scene_op()) {
      std::puts("scene A did not request a transition");
      result = 1;
    }
  }

  if ((result == 0) &&
      !engine::runtime::process_pending_scene_op(*world)) {
    std::puts("pending scene op failed");
    result = 1;
  }
  if (result == 0) {
    tick_frame();
    dispatch_begin_play(*world);
    if (world->find_entity_by_name("leak_2") ==
        engine::runtime::kInvalidEntity) {
      std::puts("scene A coroutine survived the scene transition");
      result = 1;
    }
  }

  if (result == 0) {
    const char *helperScript =
        "function start_leaker_and_new_scene()\n"
        "    engine.start_coroutine(function()\n"
        "        while true do\n"
        "            leak = (leak or 0) + 100\n"
        "            engine.wait(0)\n"
        "        end\n"
        "    end)\n"
        "    engine.new_scene()\n"
        "end\n";
    if (!write_text_file(kSceneAScript, helperScript) ||
        !engine::scripting::load_script(kSceneAScript) ||
        !engine::scripting::call_script_function(
            "start_leaker_and_new_scene")) {
      std::puts("new-scene helper failed");
      result = 1;
    } else if (!engine::runtime::process_pending_scene_op(*world)) {
      std::puts("new-scene op failed");
      result = 1;
    } else {
      tick_frame();
      dispatch_begin_play(*world);
      if (world->alive_entity_count() != 0U) {
        std::puts("new scene left entities alive");
        result = 1;
      }
    }
  }

  if (result == 0) {
    if (!add_scripted_entity(*world, "Probe", kSceneBScript)) {
      std::puts("probe setup failed");
      result = 1;
    } else {
      dispatch_begin_play(*world);
      if (world->find_entity_by_name("leak_102") ==
          engine::runtime::kInvalidEntity) {
        std::puts("scene B coroutine survived the new-scene teardown");
        result = 1;
      }
    }
  }

  if (result == 0) {
    const char *failedLoadScript =
        "function start_leaker_and_bad_load()\n"
        "    engine.start_coroutine(function()\n"
        "        while true do\n"
        "            leak = (leak or 0) + 1\n"
        "            engine.wait(0)\n"
        "        end\n"
        "    end)\n"
        "    engine.load_scene(\"scene_reset_missing.scene.json\")\n"
        "end\n"
        "function assert_leak_survived_failed_load()\n"
        "    if (leak or 0) ~= 104 then\n"
        "        error('failed load did not preserve script state: ' ..\n"
        "              tostring(leak))\n"
        "    end\n"
        "end\n";
    if (!write_text_file(kSceneAScript, failedLoadScript) ||
        !engine::scripting::load_script(kSceneAScript) ||
        !engine::scripting::call_script_function("start_leaker_and_bad_load")) {
      std::puts("failed-load helper failed");
      result = 1;
    } else if (engine::runtime::process_pending_scene_op(*world)) {
      std::puts("missing scene load unexpectedly succeeded");
      result = 1;
    } else {
      tick_frame();
      if (!engine::scripting::call_script_function(
              "assert_leak_survived_failed_load")) {
        std::puts("failed scene load must leave coroutines running");
        result = 1;
      }
      engine::scripting::clear_pending_scene_op();
    }
  }

  engine::runtime::unbind_scripting_runtime(locator);
  engine::scripting::shutdown_scripting();
  cleanup_files();
  return result;
}
