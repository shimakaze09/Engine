// Verifies that the engine-installed collision dispatch survives every scene
// commit that replaces the live World's content (#410): the deferred
// engine.load_scene transition through process_pending_scene_op with a Lua
// on_collision handler observing the pairs, the direct load_scene path
// (editor File -> Load Scene), the buffer load_scene path (editor Stop
// restore), engine.new_scene's reset_world, and repeated transitions.
// Regression: the dispatch is run-tier state installed once per run, and a
// staged World never carries one, so a commit that copied it from the staged
// World silently detached every collision callback after the first load.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kHelperScript = "slcd_helper.lua";
constexpr const char *kContactSceneFile = "slcd_contact.scene.json";
constexpr float kFixedDt = 1.0F / 60.0F;

/// Pairs delivered to the C++ recording dispatch since the last reset.
std::size_t g_recordedPairs = 0U;

/// C++ dispatch standing in for the pipeline's scripting dispatch on the
/// paths that do not go through Lua.
void record_collision_pairs(const engine::core::Entity * /*pairData*/,
                            std::size_t pairCount) noexcept {
  g_recordedPairs += pairCount;
}

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
  static_cast<void>(std::remove(kHelperScript));
  static_cast<void>(std::remove(kContactSceneFile));
}

/// Authors the contact scene into `world`: a static unit block at the
/// origin and a resting dynamic sphere overlapping its top face by 0.1 m,
/// under zero gravity so the pair is reported on the very first step and
/// stays stationary otherwise.
bool author_contact_scene(engine::runtime::World &world) noexcept {
  engine::runtime::set_gravity(world, 0.0F, 0.0F, 0.0F);

  engine::runtime::Transform blockTransform{};
  const engine::runtime::Entity block =
      world.create_scene_object(blockTransform);
  engine::runtime::Transform sphereTransform{};
  sphereTransform.position = engine::math::Vec3(0.0F, 0.9F, 0.0F);
  const engine::runtime::Entity sphere =
      world.create_scene_object(sphereTransform);
  if ((block == engine::runtime::kInvalidEntity) ||
      (sphere == engine::runtime::kInvalidEntity)) {
    return false;
  }

  engine::runtime::Collider blockCollider{};
  blockCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::Collider sphereCollider{};
  sphereCollider.shape = engine::runtime::ColliderShape::Sphere;
  sphereCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  return world.add_collider(block, blockCollider) &&
         world.add_collider(sphere, sphereCollider) &&
         world.add_rigid_body(sphere, body);
}

/// Runs one rendered frame of one fixed step through the production bridge
/// sequence and drains the frame's pairs to the installed dispatch.
bool run_contact_frame(engine::runtime::World &world) noexcept {
  world.begin_update_phase();
  const bool stepped =
      world.update_transforms_range(0U, world.transform_count(), kFixedDt) &&
      engine::runtime::step_physics(world, kFixedDt) &&
      engine::runtime::resolve_collisions(world, kFixedDt);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  if (!stepped) {
    return false;
  }
  engine::runtime::dispatch_collision_callbacks(world);
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

/// Test 1 (red on base): the production player/script path. The pipeline
/// installs the scripting dispatch once at initialize; a script requests
/// engine.load_scene; process_pending_scene_op commits the contact scene;
/// the next physics frame must still reach the Lua on_collision handler.
bool test_lua_handler_after_pending_scene_load() noexcept {
  Fixture fx{};
  if (!fx.init()) {
    std::puts("test 1: fixture init failed");
    return false;
  }
  engine::runtime::set_collision_dispatch(
      *fx.world, &engine::scripting::dispatch_physics_callbacks);

  const char *helper =
      "collision_hits = 0\n"
      "engine.on_collision_handler(function(a, b)\n"
      "    collision_hits = collision_hits + 1\n"
      "end)\n"
      "function slcd_request_load()\n"
      "    engine.load_scene(\"slcd_contact.scene.json\")\n"
      "end\n"
      "function slcd_assert_hit()\n"
      "    if collision_hits < 1 then\n"
      "        error('on_collision never fired after load_scene')\n"
      "    end\n"
      "end\n";
  bool ok = write_text_file(kHelperScript, helper) &&
            engine::scripting::load_script(kHelperScript) &&
            engine::scripting::call_script_function("slcd_request_load");
  if (!ok) {
    std::puts("test 1: helper script setup failed");
  } else if (!engine::scripting::has_pending_scene_op()) {
    std::puts("test 1: load_scene did not queue a pending op");
    ok = false;
  } else if (!engine::runtime::process_pending_scene_op(*fx.world)) {
    std::puts("test 1: pending scene op failed");
    ok = false;
  } else if (fx.world->alive_entity_count() != 2U) {
    std::puts("test 1: contact scene did not commit");
    ok = false;
  } else if (!run_contact_frame(*fx.world)) {
    std::puts("test 1: physics frame failed");
    ok = false;
  } else if (!engine::scripting::call_script_function("slcd_assert_hit")) {
    std::puts("test 1: Lua on_collision was not reached after load_scene");
    ok = false;
  }
  fx.shutdown();
  return ok;
}

/// Test 2: the direct path (editor File -> Load Scene) and the buffer path
/// (editor Stop restore) keep a C++ dispatch across the commit; the
/// same-world reset_world path (engine.new_scene) is pinned alongside as
/// the boundary that never regressed.
bool test_direct_and_buffer_loads_keep_dispatch() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    std::puts("test 2: world allocation failed");
    return false;
  }
  world->end_frame_phase();
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);

  // File path.
  g_recordedPairs = 0U;
  if (!engine::runtime::load_scene(*world, kContactSceneFile)) {
    std::puts("test 2: file load_scene failed");
    return false;
  }
  if (!run_contact_frame(*world) || (g_recordedPairs == 0U)) {
    std::puts("test 2: dispatch lost across file load_scene");
    return false;
  }

  // Buffer path: reload the authored contact (the step above already
  // separated the sphere), snapshot it exactly like the editor's pre-play
  // snapshot, then restore it over itself.
  static char snapshot[64U * 1024U] = {};
  std::size_t snapshotSize = 0U;
  if (!engine::runtime::load_scene(*world, kContactSceneFile) ||
      !engine::runtime::save_scene(*world, snapshot, sizeof(snapshot),
                                   &snapshotSize)) {
    std::puts("test 2: snapshot save failed");
    return false;
  }
  g_recordedPairs = 0U;
  if (!engine::runtime::load_scene(*world, snapshot, snapshotSize)) {
    std::puts("test 2: buffer load_scene failed");
    return false;
  }
  if (!run_contact_frame(*world) || (g_recordedPairs == 0U)) {
    std::puts("test 2: dispatch lost across buffer load_scene");
    return false;
  }

  // reset_world keeps the dispatch on the same World object; re-authoring
  // the contact afterwards still reports through it.
  engine::runtime::reset_world(*world);
  g_recordedPairs = 0U;
  if (!author_contact_scene(*world) || !run_contact_frame(*world) ||
      (g_recordedPairs == 0U)) {
    std::puts("test 2: dispatch lost across reset_world");
    return false;
  }
  return true;
}

/// Test 3: repeated transitions (many loads in one run) keep the dispatch
/// every time, and a World that never had a dispatch stays silent after a
/// load rather than acquiring one from anywhere.
bool test_repeated_loads_and_no_dispatch_world() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  std::unique_ptr<engine::runtime::World> silent(new (std::nothrow)
                                                     engine::runtime::World());
  if ((world == nullptr) || (silent == nullptr)) {
    std::puts("test 3: world allocation failed");
    return false;
  }
  world->end_frame_phase();
  silent->end_frame_phase();
  engine::runtime::set_collision_dispatch(*world, &record_collision_pairs);

  for (int i = 0; i < 4; ++i) {
    g_recordedPairs = 0U;
    if (!engine::runtime::load_scene(*world, kContactSceneFile) ||
        !run_contact_frame(*world) || (g_recordedPairs == 0U)) {
      std::printf("test 3: dispatch lost on load %d\n", i);
      return false;
    }
  }

  g_recordedPairs = 0U;
  if (!engine::runtime::load_scene(*silent, kContactSceneFile) ||
      !run_contact_frame(*silent)) {
    std::puts("test 3: silent world load failed");
    return false;
  }
  if (g_recordedPairs != 0U) {
    std::puts("test 3: a world without a dispatch reported pairs");
    return false;
  }
  return true;
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

  int result = 0;

  // The contact scene is authored once through the production serializer
  // and reused by every load below.
  {
    std::unique_ptr<engine::runtime::World> author(
        new (std::nothrow) engine::runtime::World());
    if ((author == nullptr) || !author_contact_scene(*author) ||
        !engine::runtime::save_scene(*author, kContactSceneFile)) {
      std::puts("contact scene fixture failed");
      result = 1;
    }
  }

  if ((result == 0) && !test_lua_handler_after_pending_scene_load()) {
    result = 1;
  }
  if ((result == 0) && !test_direct_and_buffer_loads_keep_dispatch()) {
    result = 1;
  }
  if ((result == 0) && !test_repeated_loads_and_no_dispatch_world()) {
    result = 1;
  }

  engine::scripting::shutdown_scripting();
  cleanup_files();
  return result;
}
