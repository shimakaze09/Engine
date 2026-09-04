// Verifies on_end_play dispatch on script-driven scene transitions (#198):
// engine.load_scene and engine.new_scene now fire on_end_play for every
// outgoing scripted entity before teardown, matching editor Stop. Covers
// the red-on-base regression (state a handler schedules in on_end_play
// must survive the transition), zero/many-entity dispatch, no double
// dispatch for an entity destroyed earlier the same frame, repeated
// transitions, failed-load no-dispatch, and the reentrancy policy (a
// handler's own load_scene/new_scene/spawn/destroy request must not
// corrupt the transition already in flight).

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

constexpr const char *kSceneAScript = "ep_scene_a.lua";
constexpr const char *kSceneBScript = "ep_scene_b.lua";
constexpr const char *kSceneCScript = "ep_scene_c.lua";
constexpr const char *kCounterScript = "ep_counter.lua";
constexpr const char *kUtilScript = "ep_util.lua";
// Test 7's own path: never reused as an entity script module elsewhere, so
// its mtime-gated module cache entry can't collide with (or go stale
// against) kSceneAScript's reuse as a plain top-level helper in tests
// 2/3/4/6.
constexpr const char *kReentrancyScript = "ep_reentrancy.lua";
constexpr const char *kSceneBFile = "ep_scene_b.scene.json";
constexpr const char *kSceneCFile = "ep_scene_c.scene.json";

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
  static_cast<void>(std::remove(kSceneCScript));
  static_cast<void>(std::remove(kCounterScript));
  static_cast<void>(std::remove(kUtilScript));
  static_cast<void>(std::remove(kReentrancyScript));
  static_cast<void>(std::remove(kSceneBFile));
  static_cast<void>(std::remove(kSceneCFile));
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
                         const char *scriptPath,
                         engine::runtime::Entity *outEntity = nullptr) noexcept {
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
  if (!world.add_script_component(entity, script)) {
    return false;
  }
  if (outEntity != nullptr) {
    *outEntity = entity;
  }
  return true;
}

/// Destroys an entity and dispatches deferred EndPlay for it exactly like
/// EnginePipeline::Impl::stage_post_frame does for a same-frame destroy
/// (Simulation-phase destroy_entity defers, then begin/end_end_play_phase
/// dispatches and flushes it), so process_pending_scene_op sees it already
/// gone.
bool destroy_with_end_play(engine::runtime::World &world,
                           engine::runtime::Entity entity) noexcept {
  world.begin_update_phase();
  if (!world.destroy_entity(entity)) {
    world.commit_update_phase();
    return false;
  }
  world.commit_update_phase();
  world.begin_end_play_phase();
  engine::scripting::dispatch_entity_scripts_end_play(&world);
  world.end_end_play_phase();
  engine::scripting::flush_deferred_mutations();
  return true;
}

/// Fixture bundle: a bound world plus the asset services scripting needs.
struct Fixture final {
  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase;
  std::unique_ptr<engine::renderer::AssetManager> assetManager;
  std::unique_ptr<engine::runtime::World> world;
  engine::core::ServiceLocator locator{};
  engine::runtime::EngineAssetDatabaseService assetService{};

  bool init() noexcept {
    world.reset(new (std::nothrow) engine::runtime::World());
    assetDatabase.reset(new (std::nothrow) engine::renderer::AssetDatabase());
    assetManager.reset(new (std::nothrow) engine::renderer::AssetManager());
    if ((world == nullptr) || (assetDatabase == nullptr) ||
        (assetManager == nullptr)) {
      return false;
    }
    engine::renderer::clear_asset_database(assetDatabase.get());
    engine::renderer::clear_asset_manager(assetManager.get());
    assetService.database = assetDatabase.get();
    assetService.manager = assetManager.get();
    if (!locator.register_service<engine::runtime::EngineAssetDatabaseService>(
            &assetService)) {
      return false;
    }
    engine::runtime::bind_scripting_runtime(world.get(), locator);
    world->end_frame_phase();
    return true;
  }

  void shutdown() noexcept {
    engine::runtime::unbind_scripting_runtime(locator);
  }
};

} // namespace

/// Runs this executable or test program.
int main() {
  cleanup_files();
  static_cast<void>(engine::core::initialize_logging());

  if (!engine::scripting::initialize_scripting()) {
    std::puts("scripting init failed");
    return 1;
  }

  int result = 0;

  // Scene B: a plain landing scene with no scripts of its own beyond a
  // marker entity, saved once and reused as the load_scene target.
  const char *sceneBScript =
      "local M = {}\n"
      "function M.on_begin_play(self)\n"
      "    engine.set_name(self, \"GoalB\")\n"
      "end\n"
      "return M\n";
  // Scene C: the reentrancy test's forbidden target — its presence after a
  // transition proves a reentrant load_scene call was honored (must never
  // happen).
  const char *sceneCScript =
      "local M = {}\n"
      "function M.on_begin_play(self)\n"
      "    engine.set_name(self, \"GoalC\")\n"
      "end\n"
      "return M\n";
  const char *counterScript =
      "local M = {}\n"
      "function M.on_end_play(self)\n"
      "    end_play_count = (end_play_count or 0) + 1\n"
      "    end_play_names = (end_play_names or \"\") ..\n"
      "        engine.get_name(self) .. \";\"\n"
      "end\n"
      "return M\n";
  // A single Lua VM is shared across every subtest below (like every other
  // scene-transition test in this suite), so any global two subtests both
  // touch needs an explicit reset — ep_reset_counters() is that reset for
  // end_play_count/end_play_names, shared by tests 3/4/6.
  const char *utilScript =
      "function ep_reset_counters()\n"
      "    end_play_count = 0\n"
      "    end_play_names = \"\"\n"
      "end\n";
  if (!write_text_file(kSceneBScript, sceneBScript) ||
      !write_text_file(kSceneCScript, sceneCScript) ||
      !write_text_file(kCounterScript, counterScript) ||
      !write_text_file(kUtilScript, utilScript) ||
      !engine::scripting::load_script(kUtilScript)) {
    std::puts("fixture script write failed");
    result = 1;
  }

  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (scene B save)");
      result = 1;
    } else {
      if (!add_scripted_entity(*fx.world, "GoalB", kSceneBScript) ||
          !engine::runtime::save_scene(*fx.world, kSceneBFile)) {
        std::puts("scene B setup failed");
        result = 1;
      }
      fx.shutdown();
    }
  }
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (scene C save)");
      result = 1;
    } else {
      if (!add_scripted_entity(*fx.world, "GoalC", kSceneCScript) ||
          !engine::runtime::save_scene(*fx.world, kSceneCFile)) {
        std::puts("scene C setup failed");
        result = 1;
      }
      fx.shutdown();
    }
  }

  // --- Test 1: red-on-base regression — state a handler schedules in
  // on_end_play must survive engine.load_scene through the production
  // process_pending_scene_op path. Base: on_end_play never fires, so
  // saved_progress stays nil. Fixed: it fires before scene B commits, so
  // saved_progress == 42 survives as a Lua global (the cross-scene handoff
  // channel; the entity handle itself does not survive).
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 1)");
      result = 1;
    } else {
      const char *sceneAScript =
          "local M = {}\n"
          "function M.on_begin_play(self)\n"
          "    engine.load_scene(\"ep_scene_b.scene.json\")\n"
          "end\n"
          "function M.on_end_play(self)\n"
          "    saved_progress = 42\n"
          "end\n"
          "return M\n";
      if (!write_text_file(kSceneAScript, sceneAScript) ||
          !add_scripted_entity(*fx.world, "StarterA", kSceneAScript)) {
        std::puts("test 1 setup failed");
        result = 1;
      } else {
        dispatch_begin_play(*fx.world);
        if (!engine::scripting::has_pending_scene_op()) {
          std::puts("test 1: scene A did not request a transition");
          result = 1;
        } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
          std::puts("test 1: pending scene op failed");
          result = 1;
        } else if (fx.world->find_entity_by_name("GoalB") ==
                  engine::runtime::kInvalidEntity) {
          std::puts("test 1: scene B did not commit");
          result = 1;
        }
        // Read the on_end_play-set global back through a tiny probe script.
        if (result == 0) {
          const char *probe =
              "function ep_read_saved_progress()\n"
              "    if saved_progress ~= 42 then\n"
              "        error('on_end_play state lost across load_scene: ' ..\n"
              "              tostring(saved_progress))\n"
              "    end\n"
              "end\n";
          if (!write_text_file("ep_probe.lua", probe) ||
              !engine::scripting::load_script("ep_probe.lua") ||
              !engine::scripting::call_script_function(
                  "ep_read_saved_progress")) {
            std::puts("test 1: on_end_play state did not survive load_scene");
            result = 1;
          }
          static_cast<void>(std::remove("ep_probe.lua"));
        }
      }
      fx.shutdown();
    }
  }

  // --- Test 2: zero scripted entities — an empty outgoing world must
  // transition cleanly with nothing to dispatch.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 2)");
      result = 1;
    } else {
      const char *helper =
          "function ep_request_load()\n"
          "    engine.load_scene(\"ep_scene_b.scene.json\")\n"
          "end\n";
      if (!write_text_file(kSceneAScript, helper) ||
          !engine::scripting::load_script(kSceneAScript) ||
          !engine::scripting::call_script_function("ep_request_load")) {
        std::puts("test 2 setup failed");
        result = 1;
      } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
        std::puts("test 2: pending scene op failed on empty world");
        result = 1;
      } else if (fx.world->find_entity_by_name("GoalB") ==
                engine::runtime::kInvalidEntity) {
        std::puts("test 2: scene B did not commit over an empty world");
        result = 1;
      }
      fx.shutdown();
    }
  }

  // --- Test 3: many scripted entities — every one must receive
  // on_end_play exactly once, dispatched while still alive.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 3)");
      result = 1;
    } else {
      constexpr int kEntityCount = 5;
      bool setupOk = engine::scripting::call_script_function(
          "ep_reset_counters");
      for (int i = 0; setupOk && (i < kEntityCount); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Many%d", i);
        if (!add_scripted_entity(*fx.world, name, kCounterScript)) {
          setupOk = false;
          break;
        }
      }
      const char *helper =
          "function ep_request_load()\n"
          "    engine.load_scene(\"ep_scene_b.scene.json\")\n"
          "end\n"
          "function ep_assert_many_dispatched()\n"
          "    if (end_play_count or 0) ~= 5 then\n"
          "        error('expected 5 on_end_play dispatches, got ' ..\n"
          "              tostring(end_play_count))\n"
          "    end\n"
          "    for i = 0, 4 do\n"
          "        local marker = \"Many\" .. tostring(i) .. \";\"\n"
          "        if not string.find(end_play_names, marker, 1, true) then\n"
          "            error('entity ' .. marker .. ' missing from dispatch order log')\n"
          "        end\n"
          "    end\n"
          "end\n";
      if (!setupOk || !write_text_file(kSceneAScript, helper) ||
          !engine::scripting::load_script(kSceneAScript) ||
          !engine::scripting::call_script_function("ep_request_load")) {
        std::puts("test 3 setup failed");
        result = 1;
      } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
        std::puts("test 3: pending scene op failed");
        result = 1;
      } else if (!engine::scripting::call_script_function(
                     "ep_assert_many_dispatched")) {
        std::puts("test 3: not every scripted entity received on_end_play");
        result = 1;
      }
      fx.shutdown();
    }
  }

  // --- Test 4: an entity destroyed earlier the same frame must not
  // receive on_end_play a second time from the transition dispatch.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 4)");
      result = 1;
    } else {
      engine::runtime::Entity doomed{};
      if (!engine::scripting::call_script_function("ep_reset_counters") ||
          !add_scripted_entity(*fx.world, "Doomed", kCounterScript, &doomed)) {
        std::puts("test 4 setup failed");
        result = 1;
      } else {
        dispatch_begin_play(*fx.world);
        if (!destroy_with_end_play(*fx.world, doomed)) {
          std::puts("test 4: same-frame destroy failed");
          result = 1;
        } else {
          const char *helper =
              "function ep_request_load()\n"
              "    engine.load_scene(\"ep_scene_b.scene.json\")\n"
              "end\n"
              "function ep_assert_single_dispatch()\n"
              "    if (end_play_count or 0) ~= 1 then\n"
              "        error('expected exactly one on_end_play, got ' ..\n"
              "              tostring(end_play_count))\n"
              "    end\n"
              "end\n";
          if (!write_text_file(kSceneAScript, helper) ||
              !engine::scripting::load_script(kSceneAScript) ||
              !engine::scripting::call_script_function("ep_request_load")) {
            std::puts("test 4: transition trigger failed");
            result = 1;
          } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
            std::puts("test 4: pending scene op failed");
            result = 1;
          } else if (!engine::scripting::call_script_function(
                         "ep_assert_single_dispatch")) {
            std::puts("test 4: destroyed entity was dispatched twice");
            result = 1;
          }
        }
      }
      fx.shutdown();
    }
  }

  // --- Test 5: repeated transitions — every load_scene/new_scene commit
  // must dispatch exactly the entities present in that outgoing scene, not
  // an accumulation from earlier transitions.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 5)");
      result = 1;
    } else {
      const char *iterScript =
          "function ep_iter_setup()\n"
          "    end_play_count = 0\n"
          "end\n"
          "function ep_iter_new_scene()\n"
          "    engine.new_scene()\n"
          "end\n"
          "function ep_iter_assert(expected)\n"
          "    if end_play_count ~= expected then\n"
          "        error('iteration mismatch: expected ' ..\n"
          "              tostring(expected) .. ' got ' ..\n"
          "              tostring(end_play_count))\n"
          "    end\n"
          "end\n"
          "function ep_iter_assert_float(expected)\n"
          "    ep_iter_assert(math.floor(expected + 0.5))\n"
          "end\n";
      if (!write_text_file(kSceneAScript, iterScript) ||
          !engine::scripting::load_script(kSceneAScript) ||
          !engine::scripting::call_script_function("ep_iter_setup")) {
        std::puts("test 5 setup failed");
        result = 1;
      }
      for (int i = 0; (result == 0) && (i < 4); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Rep%d", i);
        if (!add_scripted_entity(*fx.world, name, kCounterScript)) {
          std::printf("test 5: entity setup failed at iter %d\n", i);
          result = 1;
        } else if (!engine::scripting::call_script_function(
                       "ep_iter_new_scene")) {
          std::printf("test 5: transition trigger failed at iter %d\n", i);
          result = 1;
        } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
          std::printf("test 5: pending scene op failed at iter %d\n", i);
          result = 1;
        } else if (!engine::scripting::call_script_function_float(
                       "ep_iter_assert_float", static_cast<float>(i + 1))) {
          std::printf("test 5: dispatch count mismatch at iter %d\n", i);
          result = 1;
        }
      }
      fx.shutdown();
    }
  }

  // --- Test 6: failed load must skip on_end_play entirely — the outgoing
  // scene keeps playing untouched.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 6)");
      result = 1;
    } else {
      if (!engine::scripting::call_script_function("ep_reset_counters") ||
          !add_scripted_entity(*fx.world, "Survivor", kCounterScript)) {
        std::puts("test 6 setup failed");
        result = 1;
      } else {
        dispatch_begin_play(*fx.world);
        const char *helper =
            "function ep_request_bad_load()\n"
            "    engine.load_scene(\"ep_scene_missing.scene.json\")\n"
            "end\n"
            "function ep_assert_no_dispatch()\n"
            "    if (end_play_count or 0) ~= 0 then\n"
            "        error('on_end_play fired for a failed load: ' ..\n"
            "              tostring(end_play_count))\n"
            "    end\n"
            "end\n";
        if (!write_text_file(kSceneAScript, helper) ||
            !engine::scripting::load_script(kSceneAScript) ||
            !engine::scripting::call_script_function("ep_request_bad_load")) {
          std::puts("test 6: bad-load trigger failed");
          result = 1;
        } else if (engine::runtime::process_pending_scene_op(*fx.world)) {
          std::puts("test 6: missing scene load unexpectedly succeeded");
          result = 1;
        } else if (fx.world->find_entity_by_name("Survivor") ==
                  engine::runtime::kInvalidEntity) {
          std::puts("test 6: outgoing entity did not survive a failed load");
          result = 1;
        } else if (!engine::scripting::call_script_function(
                       "ep_assert_no_dispatch")) {
          std::puts("test 6: on_end_play fired despite the failed load");
          result = 1;
        }
        engine::scripting::clear_pending_scene_op();
      }
      fx.shutdown();
    }
  }

  // --- Test 7: reentrancy — an on_end_play handler that itself calls
  // engine.load_scene, engine.new_scene, and engine.spawn_entity must not
  // corrupt the transition already committing. Policy: reject with a
  // logged warning and continue the original transition unchanged.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 7)");
      result = 1;
    } else {
      const char *reentrancyScript =
          "local M = {}\n"
          "function M.on_begin_play(self)\n"
          "    engine.load_scene(\"ep_scene_b.scene.json\")\n"
          "end\n"
          "function M.on_end_play(self)\n"
          "    reentrant_count = (reentrant_count or 0) + 1\n"
          "    engine.load_scene(\"ep_scene_c.scene.json\")\n"
          "    engine.new_scene()\n"
          "    reentrant_spawn = engine.spawn_entity()\n"
          "    reentrant_destroy_ok = engine.destroy_entity(self)\n"
          "end\n"
          "return M\n";
      if (!write_text_file(kReentrancyScript, reentrancyScript) ||
          !add_scripted_entity(*fx.world, "StarterA", kReentrancyScript)) {
        std::puts("test 7 setup failed");
        result = 1;
      } else {
        dispatch_begin_play(*fx.world);
        if (!engine::scripting::has_pending_scene_op()) {
          std::puts("test 7: scene A did not request the original transition");
          result = 1;
        } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
          std::puts("test 7: pending scene op failed");
          result = 1;
        } else if (fx.world->find_entity_by_name("GoalB") ==
                  engine::runtime::kInvalidEntity) {
          std::puts("test 7: original transition (scene B) did not commit");
          result = 1;
        } else if (fx.world->find_entity_by_name("GoalC") !=
                  engine::runtime::kInvalidEntity) {
          std::puts("test 7: reentrant load_scene(C) was honored");
          result = 1;
        } else if (engine::scripting::has_pending_scene_op()) {
          std::puts("test 7: a reentrant scene op leaked past the transition");
          result = 1;
        } else {
          const char *probe =
              "function ep_assert_reentrancy_contained()\n"
              "    if reentrant_count ~= 1 then\n"
              "        error('on_end_play fired other than once: ' ..\n"
              "              tostring(reentrant_count))\n"
              "    end\n"
              "    if reentrant_spawn ~= nil then\n"
              "        error('reentrant spawn_entity was not rejected')\n"
              "    end\n"
              "end\n";
          if (!write_text_file("ep_probe7.lua", probe) ||
              !engine::scripting::load_script("ep_probe7.lua") ||
              !engine::scripting::call_script_function(
                  "ep_assert_reentrancy_contained")) {
            std::puts("test 7: reentrant mutation was not safely contained");
            result = 1;
          }
          static_cast<void>(std::remove("ep_probe7.lua"));
        }
      }
      fx.shutdown();
    }
  }

  // --- Test 8: a mutation an on_end_play handler defers during the
  // transition must not reach the replacement scene (#411). Scene B's
  // GoalB is allocated first in a fresh World, so it holds the same
  // {index, generation} as scene A's StarterA; without the content-epoch
  // stamp the next flush would move GoalB to the outgoing handler's
  // target position. Also pins that the queue still serves the new scene
  // afterwards: a mutation deferred against scene B's own epoch applies.
  if (result == 0) {
    Fixture fx{};
    if (!fx.init()) {
      std::puts("fixture init failed (test 8)");
      result = 1;
    } else {
      const char *retargetScript =
          "local M = {}\n"
          "function M.on_begin_play(self)\n"
          "    engine.load_scene(\"ep_scene_b.scene.json\")\n"
          "end\n"
          "function M.on_end_play(self)\n"
          "    stale_move_queued = engine.set_position(self, 100, 200, 300)\n"
          "end\n"
          "return M\n";
      const char *probe =
          "function ep_assert_stale_move_queued()\n"
          "    if stale_move_queued ~= true then\n"
          "        error('on_end_play set_position was not accepted: ' ..\n"
          "              tostring(stale_move_queued))\n"
          "    end\n"
          "end\n";
      engine::runtime::Entity starter{};
      if (!write_text_file(kReentrancyScript, retargetScript) ||
          !add_scripted_entity(*fx.world, "StarterA", kReentrancyScript,
                               &starter) ||
          !write_text_file("ep_probe8.lua", probe) ||
          !engine::scripting::load_script("ep_probe8.lua")) {
        std::puts("test 8 setup failed");
        result = 1;
      } else {
        dispatch_begin_play(*fx.world);
        if (!engine::scripting::has_pending_scene_op()) {
          std::puts("test 8: scene A did not request a transition");
          result = 1;
        } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
          std::puts("test 8: pending scene op failed");
          result = 1;
        } else if (!engine::scripting::call_script_function(
                       "ep_assert_stale_move_queued")) {
          std::puts("test 8: the outgoing handler's mutation was refused");
          result = 1;
        }
        const engine::runtime::Entity goal =
            fx.world->find_entity_by_name("GoalB");
        if ((result == 0) && (goal == engine::runtime::kInvalidEntity)) {
          std::puts("test 8: scene B did not commit");
          result = 1;
        }
        if ((result == 0) && (goal.index != starter.index)) {
          std::puts("test 8: fixture assumption broken: GoalB does not "
                    "reuse StarterA's entity index");
          result = 1;
        }
        // The pipeline's next post-frame flush: the retained entry must be
        // dropped, not applied to the recycled index.
        if (result == 0) {
          engine::scripting::flush_deferred_mutations();
          engine::runtime::Transform goalTransform{};
          if (!fx.world->get_transform(goal, &goalTransform)) {
            std::puts("test 8: GoalB lost its transform");
            result = 1;
          } else if ((goalTransform.position.x != 0.0F) ||
                     (goalTransform.position.y != 0.0F) ||
                     (goalTransform.position.z != 0.0F)) {
            std::puts("test 8: a stale deferred mutation retargeted the "
                      "replacement scene's entity");
            result = 1;
          }
        }
        // Same-epoch boundary: a mutation deferred inside scene B's own
        // end-play dispatch (the destroy of a sibling entity, whose handler
        // moves GoalB) is still applied by the following flush.
        if (result == 0) {
          const char *deferInNewScene =
              "local M = {}\n"
              "function M.on_end_play(self)\n"
              "    local goal = engine.find_entity_by_name(\"GoalB\")\n"
              "    same_epoch_move = engine.set_position(goal, 7, 8, 9)\n"
              "end\n"
              "return M\n";
          engine::runtime::Entity mover{};
          if (!write_text_file(kSceneCScript, deferInNewScene) ||
              !add_scripted_entity(*fx.world, "Mover", kSceneCScript,
                                   &mover)) {
            std::puts("test 8: mover setup failed");
            result = 1;
          } else {
            dispatch_begin_play(*fx.world);
            if (!destroy_with_end_play(*fx.world, mover)) {
              std::puts("test 8: mover destroy failed");
              result = 1;
            } else {
              engine::runtime::Transform goalTransform{};
              if (!fx.world->get_transform(goal, &goalTransform) ||
                  (goalTransform.position.x != 7.0F) ||
                  (goalTransform.position.y != 8.0F) ||
                  (goalTransform.position.z != 9.0F)) {
                std::puts("test 8: a same-epoch deferred mutation was "
                          "not applied");
                result = 1;
              }
            }
          }
        }
      }
      static_cast<void>(std::remove("ep_probe8.lua"));
      fx.shutdown();
    }
  }

  engine::scripting::shutdown_scripting();
  cleanup_files();
  return result;
}
