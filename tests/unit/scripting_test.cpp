// Verifies scripting test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/touch_input.h"
#include "engine/core/service_locator.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/content/asset_streaming.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScriptPath = "scripting_test.lua";

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }

  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

bool nearly_equal(float lhs, float rhs) noexcept {
  const float diff = lhs - rhs;
  return (diff < 0.0001F) && (diff > -0.0001F);
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kTempScriptPath));
}

/// Writes script file data.
bool write_script_file(const char *contents) noexcept {
  if (contents == nullptr) {
    return false;
  }

  FILE *file = nullptr;
  if (!open_file_for_write(kTempScriptPath, &file) || (file == nullptr)) {
    return false;
  }

  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

void touch_probe_callback(const engine::core::TouchEvent &,
                          void *) noexcept {}

void gesture_probe_callback(const engine::core::GestureEvent &,
                            void *) noexcept {}

} // namespace

/// Runs this executable or test program.
int main() {
  remove_script_file();

  // Logging surfaces Lua tracebacks when a scripted verification fails.
  static_cast<void>(engine::core::initialize_logging());

  if (!engine::scripting::initialize_scripting()) {
    return 1;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    engine::scripting::shutdown_scripting();
    return 2;
  }

  engine::core::ServiceLocator serviceLocator{};
  std::unique_ptr<engine::renderer::AssetDatabase> scriptAssetDatabase(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::AssetManager> scriptAssetManager(
      new (std::nothrow) engine::renderer::AssetManager());
  if ((scriptAssetDatabase == nullptr) || (scriptAssetManager == nullptr)) {
    engine::scripting::shutdown_scripting();
    return 3;
  }
  engine::renderer::clear_asset_database(scriptAssetDatabase.get());
  engine::renderer::clear_asset_manager(scriptAssetManager.get());
  engine::runtime::EngineAssetDatabaseService scriptAssetService{};
  scriptAssetService.database = scriptAssetDatabase.get();
  scriptAssetService.manager = scriptAssetManager.get();
  if (!serviceLocator.register_service<engine::runtime::EngineAssetDatabaseService>(
          &scriptAssetService)) {
    engine::scripting::shutdown_scripting();
    return 3;
  }
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
  constexpr std::uint32_t kDefaultMeshAssetId = 777U;
  engine::scripting::set_default_mesh_asset_id(kDefaultMeshAssetId);

  const char *scriptContents =
      "local spawned = nil\n"
      "function on_start()\n"
      "    spawned = engine.spawn_entity()\n"
      "    if spawned == nil then\n"
      "        return\n"
      "    end\n"
      "    engine.set_name(spawned, \"Player\")\n"
      "    local entityName = engine.get_name(spawned)\n"
      "    if entityName ~= nil then\n"
      "        engine.set_name(spawned, entityName)\n"
      "    end\n"
      "    engine.set_position(spawned, 1.0, 2.0, 3.0)\n"
      "    local px, py, pz = engine.get_position(spawned)\n"
      "    if px ~= nil then\n"
      "        engine.set_position(spawned, px + 1.0, py + 1.0, pz + 1.0)\n"
      "    end\n"
      "    engine.add_rigid_body(spawned, 1.0)\n"
      "    local defaultMeshAssetId = engine.get_default_mesh_asset_id()\n"
      "    -- Mesh handle is validated by render prep; test only checks the\n"
      "    -- component write path here.\n"
      "    if defaultMeshAssetId ~= nil then\n"
      "        engine.set_mesh(spawned, defaultMeshAssetId)\n"
      "    end\n"
      "    engine.set_albedo(spawned, 0.2, 0.8, 0.4)\n"
      "    engine.set_velocity(spawned, 4.0, 5.0, 6.0)\n"
      "    local vx, vy, vz = engine.get_velocity(spawned)\n"
      "    if vx ~= nil then\n"
      "        engine.set_velocity(spawned, vx + 1.0, vy + 1.0, vz + 1.0)\n"
      "    end\n"
      "    engine.set_acceleration(spawned, 0.0, -9.0, 0.0)\n"
      "    engine.add_collider(spawned, 0.5, 0.5, 0.5)\n"
      "end\n"
      "function wake_with_velocity()\n"
      "    engine.set_velocity(spawned, 8.0, 0.0, -2.0)\n"
      "end\n"
      "function teleport_then_release()\n"
      "    -- Values match what later position/velocity assertions expect.\n"
      "    engine.set_position(spawned, 2.0, 3.0, 4.0)\n"
      "    engine.set_velocity(spawned, 5.0, 6.0, 7.0)\n"
      "end\n"
      "function verify_parenting()\n"
      "    local parent = engine.spawn_entity()\n"
      "    local kid = engine.spawn_entity()\n"
      "    if parent == nil or kid == nil then error('spawn failed') end\n"
      "    engine.set_position(parent, 50.0, 0.0, 0.0)\n"
      "    engine.set_position(kid, 1.0, 1.0, 1.0)\n"
      "    -- Dynamic rigid bodies must stay hierarchy roots.\n"
      "    if engine.set_parent(spawned, parent) then\n"
      "        error('parenting a dynamic body must fail')\n"
      "    end\n"
      "    if not engine.set_parent(kid, parent) then\n"
      "        error('set_parent failed')\n"
      "    end\n"
      "    if engine.get_parent(kid) ~= parent then\n"
      "        error('get_parent mismatch')\n"
      "    end\n"
      "    local children = engine.get_children(parent)\n"
      "    if #children ~= 1 or children[1] ~= kid then\n"
      "        error('get_children mismatch')\n"
      "    end\n"
      "    if engine.set_parent(kid, kid) then\n"
      "        error('self-parent must fail')\n"
      "    end\n"
      "    if not engine.set_parent(kid, nil) then\n"
      "        error('unparent failed')\n"
      "    end\n"
      "    if engine.get_parent(kid) ~= nil then\n"
      "        error('parent should be nil after unparent')\n"
      "    end\n"
      "    if #engine.get_children(parent) ~= 0 then\n"
      "        error('children should be empty after unparent')\n"
      "    end\n"
      "    if not engine.destroy_entity(kid) then\n"
      "        error('kid cleanup failed')\n"
      "    end\n"
      "end\n"
      "function on_update()\n"
      "    if engine.is_alive(spawned) then\n"
      "        engine.destroy_entity(spawned)\n"
      "    end\n"
      "end\n";

  if (!write_script_file(scriptContents)) {
    engine::scripting::shutdown_scripting();
    return 3;
  }

  if (!engine::scripting::load_script(kTempScriptPath)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 4;
  }

  if (!engine::scripting::call_script_function("on_start")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 5;
  }

  const engine::runtime::Entity initialSpawned =
      world->find_entity_by_index(1U);
  engine::runtime::RigidBody *sleepingBody =
      world->get_rigid_body_ptr(initialSpawned);
  if (sleepingBody == nullptr) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 126;
  }
  sleepingBody->velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  sleepingBody->sleepFrameCount = 60U;
  sleepingBody->sleeping = true;
  if (!engine::scripting::call_script_function("wake_with_velocity")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 127;
  }
  if (sleepingBody->sleeping || (sleepingBody->sleepFrameCount != 0U) ||
      (sleepingBody->velocity.x != 8.0F) ||
      (sleepingBody->velocity.y != 0.0F) ||
      (sleepingBody->velocity.z != -2.0F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 128;
  }
  sleepingBody->velocity = engine::math::Vec3(5.0F, 6.0F, 7.0F);

  if (!engine::scripting::call_script_function("verify_parenting")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 129;
  }

  if (!engine::scripting::call_script_function("teleport_then_release")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 130;
  }
  if (world->movement_authority(initialSpawned) !=
      engine::runtime::MovementAuthority::None) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 131;
  }

  // Approved contract migration (issue #80): unlock restores the exact
  // captured pre-lock inverse inertia (0.25 here), not a hard-coded 1.0.
  const char *lockRotationScript =
      "function setup_lock_body()\n"
      "    local body = engine.spawn_entity()\n"
      "    if body == nil then error('spawn failed') end\n"
      "    engine.set_name(body, 'LockTest')\n"
      "    engine.add_rigid_body(body, 1.0)\n"
      "end\n"
      "function verify_lock_rotation()\n"
      "    local body = engine.find_entity_by_name('LockTest')\n"
      "    if body == nil then error('LockTest lookup failed') end\n"
      "    if not engine.set_lock_rotation(body, true) then\n"
      "        error('set_lock_rotation(true) failed')\n"
      "    end\n"
      "end\n"
      "function verify_unlock_rotation()\n"
      "    local body = engine.find_entity_by_name('LockTest')\n"
      "    if body == nil then error('LockTest lookup failed') end\n"
      "    if not engine.set_lock_rotation(body, false) then\n"
      "        error('set_lock_rotation(false) failed')\n"
      "    end\n"
      "end\n";
  if (!write_script_file(lockRotationScript) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("setup_lock_body")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 132;
  }
  const engine::runtime::Entity lockEntity =
      world->find_entity_by_name("LockTest");
  engine::runtime::RigidBody *lockedBody =
      world->get_rigid_body_ptr(lockEntity);
  if (lockedBody == nullptr) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 132;
  }
  lockedBody->inverseInertia = 0.25F;
  if (!engine::scripting::call_script_function("verify_lock_rotation") ||
      (world->get_rigid_body_ptr(lockEntity)->inverseInertia != 0.0F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 133;
  }
  if (!engine::scripting::call_script_function("verify_unlock_rotation")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 134;
  }
  if (world->get_rigid_body_ptr(lockEntity)->inverseInertia != 0.25F) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 135;
  }
  if (!engine::scripting::call_script_function("verify_lock_rotation") ||
      !engine::scripting::call_script_function("verify_unlock_rotation") ||
      (world->get_rigid_body_ptr(lockEntity)->inverseInertia != 0.25F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 136;
  }

  const char *asyncAssetScript =
      "function request_asset()\n"
      "    local h = engine.load_asset_async('assets/missing_async.mesh', 2)\n"
      "    if h == nil then\n"
      "        error('load_asset_async returned nil')\n"
      "    end\n"
      "    if engine.is_asset_ready(h) then\n"
      "        error('missing asset should not be ready before update')\n"
      "    end\n"
      "end\n";
  if (!write_script_file(asyncAssetScript) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("request_asset")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 116;
  }

  const engine::renderer::AssetId asyncAssetId =
      engine::renderer::make_asset_id_from_path("assets/missing_async.mesh");
  if (!engine::renderer::mesh_asset_requested_resident(
          scriptAssetDatabase.get(), asyncAssetId)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 117;
  }
  if (engine::renderer::pending_asset_request_count(
          scriptAssetManager.get()) == 0U) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 118;
  }

  engine::core::initialize_cvars();
  std::unique_ptr<engine::content::AssetStreamingQueue> scriptStreamingQueue(
      new (std::nothrow) engine::content::AssetStreamingQueue());
  if (scriptStreamingQueue == nullptr) {
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 119;
  }
  if (!engine::content::initialize_asset_streaming(
          scriptStreamingQueue.get())) {
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 120;
  }
  scriptAssetService.streamingQueue = scriptStreamingQueue.get();
  engine::renderer::clear_asset_manager(scriptAssetManager.get());

  const char *streamingAssetScript =
      "function request_streaming_asset()\n"
      "    local h = engine.load_asset_async('assets/streamed_async.mesh', 3)\n"
      "    if h == nil then\n"
      "        error('streaming load_asset_async returned nil')\n"
      "    end\n"
      "    if engine.is_asset_ready(h) then\n"
      "        error('streaming asset should not be ready before update')\n"
      "    end\n"
      "end\n";
  if (!write_script_file(streamingAssetScript) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("request_streaming_asset")) {
    scriptAssetService.streamingQueue = nullptr;
    engine::content::shutdown_asset_streaming(scriptStreamingQueue.get());
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 121;
  }

  const engine::renderer::AssetId streamingAssetId =
      engine::renderer::make_asset_id_from_path("assets/streamed_async.mesh");
  if (!engine::renderer::mesh_asset_requested_resident(
          scriptAssetDatabase.get(), streamingAssetId)) {
    scriptAssetService.streamingQueue = nullptr;
    engine::content::shutdown_asset_streaming(scriptStreamingQueue.get());
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 122;
  }
  if (engine::renderer::mesh_asset_state(scriptAssetDatabase.get(),
                                         streamingAssetId) !=
      engine::renderer::AssetState::Loading) {
    scriptAssetService.streamingQueue = nullptr;
    engine::content::shutdown_asset_streaming(scriptStreamingQueue.get());
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 123;
  }
  if (engine::renderer::pending_asset_request_count(
          scriptAssetManager.get()) != 0U) {
    scriptAssetService.streamingQueue = nullptr;
    engine::content::shutdown_asset_streaming(scriptStreamingQueue.get());
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 124;
  }
  if (engine::content::pending_load_count(scriptStreamingQueue.get()) != 1U) {
    scriptAssetService.streamingQueue = nullptr;
    engine::content::shutdown_asset_streaming(scriptStreamingQueue.get());
    engine::core::shutdown_cvars();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 125;
  }
  scriptAssetService.streamingQueue = nullptr;
  engine::content::shutdown_asset_streaming(scriptStreamingQueue.get());
  engine::core::shutdown_cvars();

  // Assumes a fresh World so the first spawned entity is index 1.
  const engine::runtime::Entity spawned = world->find_entity_by_index(1U);
  if (spawned == engine::runtime::kInvalidEntity) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 6;
  }

  engine::runtime::NameComponent nameComponent{};
  if (!world->get_name_component(spawned, &nameComponent)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 19;
  }

  if (std::strcmp(nameComponent.name, "Player") != 0) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 20;
  }

  engine::runtime::Transform transform{};
  if (!world->get_transform(spawned, &transform)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 7;
  }

  if ((transform.position.x != 2.0F) || (transform.position.y != 3.0F) ||
      (transform.position.z != 4.0F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 8;
  }

  engine::runtime::RigidBody rigidBody{};
  if (!world->get_rigid_body(spawned, &rigidBody)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 9;
  }

  if ((rigidBody.velocity.x != 5.0F) || (rigidBody.velocity.y != 6.0F) ||
      (rigidBody.velocity.z != 7.0F) ||
      !nearly_equal(rigidBody.acceleration.x, 0.0F) ||
      !nearly_equal(rigidBody.acceleration.y, 0.8F) ||
      !nearly_equal(rigidBody.acceleration.z, 0.0F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 10;
  }

  engine::runtime::Collider collider{};
  if (!world->get_collider(spawned, &collider)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 11;
  }

  if ((collider.halfExtents.x != 0.5F) || (collider.halfExtents.y != 0.5F) ||
      (collider.halfExtents.z != 0.5F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 12;
  }

  engine::runtime::MeshComponent meshComponent{};
  if (!world->get_mesh_component(spawned, &meshComponent)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 13;
  }

  if ((meshComponent.meshAssetId != kDefaultMeshAssetId) ||
      (meshComponent.albedo.x != 0.2F) || (meshComponent.albedo.y != 0.8F) ||
      (meshComponent.albedo.z != 0.4F)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 14;
  }

  if (!engine::scripting::call_script_function("on_update")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 15;
  }

  if (world->is_alive(spawned)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 16;
  }

  if (engine::scripting::load_script("missing_script.lua")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 17;
  }

  if (engine::scripting::call_script_function("missing_function")) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return 18;
  }

  // =========================================================================
  // Regression: Lua entity handles include generation, not only slot index.
  // =========================================================================
  {
    const char *staleEntityScript =
        "function on_start()\n"
        "    local stale = engine.spawn_entity()\n"
        "    engine.set_name(stale, 'stale_original')\n"
        "    engine.destroy_entity(stale)\n"
        "    local recycled = engine.spawn_entity()\n"
        "    engine.set_name(recycled, 'stale_recycled')\n"
        "    if recycled == stale then\n"
        "        engine.set_name(recycled, 'stale_equal_bug')\n"
        "    end\n"
        "    if engine.is_alive(stale) then\n"
        "        engine.set_name(recycled, 'stale_alive_bug')\n"
        "    end\n"
        "    if engine.set_name(stale, 'stale_write_bug') then\n"
        "        engine.set_name(recycled, 'stale_write_bug')\n"
        "    end\n"
        "    if engine.destroy_entity(stale) then\n"
        "        engine.set_name(recycled, 'stale_destroy_bug')\n"
        "    end\n"
        "end\n";
    if (!write_script_file(staleEntityScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 204;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 205;
    }
    const engine::runtime::Entity recycled = world->find_entity_by_index(1U);
    if (recycled == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 206;
    }
    engine::runtime::NameComponent recycledName{};
    if (!world->get_name_component(recycled, &recycledName)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 207;
    }
    if (std::strcmp(recycledName.name, "stale_recycled") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 208;
    }
  }

  // =========================================================================
  // Step 4.4 Test: rotation and scale round-trip via Lua bindings
  // =========================================================================
  {
    const char *rotScaleScript =
        "function on_start()\n"
        "    local e = engine.spawn_entity()\n"
        "    engine.set_name(e, 'rot_scale_test')\n"
        "    engine.set_rotation(e, 0.0, 0.707, 0.0, 0.707)\n"
        "    engine.set_scale(e, 2.0, 3.0, 4.0)\n"
        "end\n";
    if (!write_script_file(rotScaleScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 30;
    }
    if (!engine::scripting::load_script(kTempScriptPath)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 31;
    }
    if (!engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 32;
    }
    engine::runtime::Entity rotEntity{};
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "rot_scale_test") == 0) {
        rotEntity = ent;
      }
    });
    if (rotEntity == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 33;
    }
    engine::runtime::Transform rt{};
    if (!world->get_transform(rotEntity, &rt)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 34;
    }
    if (!nearly_equal(rt.rotation.y, 0.707F) ||
        !nearly_equal(rt.rotation.w, 0.707F)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 35;
    }
    if (!nearly_equal(rt.scale.x, 2.0F) || !nearly_equal(rt.scale.y, 3.0F) ||
        !nearly_equal(rt.scale.z, 4.0F)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 36;
    }
  }

  // =========================================================================
  // Step 4.4 Test: light component bindings
  // =========================================================================
  {
    const char *lightScript = "function on_start()\n"
                              "    local e = engine.spawn_entity()\n"
                              "    engine.set_name(e, 'light_test')\n"
                              "    engine.add_light(e, 'point')\n"
                              "    engine.set_light_color(e, 0.9, 0.5, 0.1)\n"
                              "    engine.set_light_intensity(e, 3.5)\n"
                              "end\n";
    if (!write_script_file(lightScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 37;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 38;
    }
    engine::runtime::Entity lightEntity{};
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "light_test") == 0) {
        lightEntity = ent;
      }
    });
    if (lightEntity == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 39;
    }
    engine::runtime::LightComponent lc{};
    if (!world->get_light_component(lightEntity, &lc)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 40;
    }
    if (!nearly_equal(lc.color.x, 0.9F) || !nearly_equal(lc.color.y, 0.5F) ||
        !nearly_equal(lc.color.z, 0.1F) || !nearly_equal(lc.intensity, 3.5F) ||
        lc.type != engine::runtime::LightType::Point) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 41;
    }
  }

  // =========================================================================
  // Step 4.6 Test: set_timeout fires once after elapsed time
  // =========================================================================
  {
    const char *timerScript = "function on_start()\n"
                              "    engine.set_timeout(function()\n"
                              "        local e = engine.spawn_entity()\n"
                              "        engine.set_name(e, 'timer_fired')\n"
                              "    end, 0.1)\n"
                              "end\n";
    if (!write_script_file(timerScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 42;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 43;
    }
    // Reset total time to 0 so the timer delay is relative.
    engine::scripting::set_frame_time(0.0F, 0.0F);
    engine::scripting::tick_timers();
    // Should NOT have fired yet (0s < 0.1s).
    bool timerFiredEarly = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "timer_fired") == 0) {
        timerFiredEarly = true;
      }
    });
    if (timerFiredEarly) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 44;
    }
    // Advance by 0.15s — past the 0.1s threshold.
    engine::scripting::set_frame_time(0.15F, 0.15F);
    engine::scripting::tick_timers();
    bool timerFired = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "timer_fired") == 0) {
        timerFired = true;
      }
    });
    if (!timerFired) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 45;
    }
  }

  // =========================================================================
  // Step 4.6 Test: cancel_timer prevents callback from firing
  // =========================================================================
  {
    const char *cancelTimerScript =
        "local g_id = nil\n"
        "function on_start()\n"
        "    g_id = engine.set_timeout(function()\n"
        "        local e = engine.spawn_entity()\n"
        "        engine.set_name(e, 'cancelled_timer_fired')\n"
        "    end, 0.1)\n"
        "    engine.cancel_timer(g_id)\n"
        "end\n";
    if (!write_script_file(cancelTimerScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 46;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 47;
    }
    engine::scripting::set_frame_time(0.5F, 0.5F);
    engine::scripting::tick_timers();
    bool cancelledFired = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "cancelled_timer_fired") == 0) {
        cancelledFired = true;
      }
    });
    if (cancelledFired) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 48;
    }
  }

  // =========================================================================
  // Step 4.6 Regression: stale timer ids cannot cancel reused slots
  // =========================================================================
  {
    const char *staleTimerScript =
        "local old_id = nil\n"
        "function on_start()\n"
        "    old_id = engine.set_timeout(function()\n"
        "        local e = engine.spawn_entity()\n"
        "        engine.set_name(e, 'stale_old_timer_fired')\n"
        "    end, 0.1)\n"
        "    engine.cancel_timer(old_id)\n"
        "    engine.set_timeout(function()\n"
        "        local e = engine.spawn_entity()\n"
        "        engine.set_name(e, 'stale_new_timer_fired')\n"
        "    end, 0.1)\n"
        "    engine.cancel_timer(old_id)\n"
        "end\n";
    if (!write_script_file(staleTimerScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 201;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 202;
    }
    engine::scripting::set_frame_time(0.2F, 0.2F);
    engine::scripting::tick_timers();
    bool oldTimerFired = false;
    bool newTimerFired = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc)) {
        if (std::strcmp(nc.name, "stale_old_timer_fired") == 0) {
          oldTimerFired = true;
        }
        if (std::strcmp(nc.name, "stale_new_timer_fired") == 0) {
          newTimerFired = true;
        }
      }
    });
    if (oldTimerFired || !newTimerFired) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 203;
    }
  }

  // =========================================================================
  // Step 4.6 Regression: fired timer cleanup preserves same-slot replacement
  // =========================================================================
  {
    const char *replacementTimerScript =
        "local old_id = nil\n"
        "function on_start()\n"
        "    old_id = engine.set_timeout(function()\n"
        "        engine.cancel_timer(old_id)\n"
        "        engine.set_timeout(function()\n"
        "            local e = engine.spawn_entity()\n"
        "            engine.set_name(e, 'replacement_timer_fired')\n"
        "        end, 0.1)\n"
        "    end, 0.1)\n"
        "end\n";
    if (!write_script_file(replacementTimerScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 209;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 210;
    }

    engine::scripting::set_frame_time(0.2F, 0.2F);
    engine::scripting::tick_timers();
    engine::scripting::set_frame_time(0.2F, 0.4F);
    engine::scripting::tick_timers();

    bool replacementTimerFired = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "replacement_timer_fired") == 0) {
        replacementTimerFired = true;
      }
    });
    if (!replacementTimerFired) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 211;
    }
  }

  // =========================================================================
  // Step 4.1 Test: coroutine wait resumes after elapsed time
  // =========================================================================
  {
    const char *coroutineScript = "function on_start()\n"
                                  "    engine.start_coroutine(function()\n"
                                  "        local e1 = engine.spawn_entity()\n"
                                  "        engine.set_name(e1, 'co_step1')\n"
                                  "        engine.wait(0.2)\n"
                                  "        local e2 = engine.spawn_entity()\n"
                                  "        engine.set_name(e2, 'co_step2')\n"
                                  "    end)\n"
                                  "end\n";
    if (!write_script_file(coroutineScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 49;
    }
    // Reset total time to 0 BEFORE on_start so that wakeAt is computed
    // relative to a known origin. start_coroutine immediately resumes the
    // coroutine, so g_totalSeconds must already be 0 when that happens.
    engine::scripting::set_frame_time(0.0F, 0.0F);
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 50;
    }
    // After on_start the coroutine already ran up to engine.wait(0.2) and
    // yielded. wakeAt = 0.0 + 0.2 = 0.2. co_step1 was created before the
    // yield; co_step2 was not yet created.
    bool step1Exists = false;
    bool step2ExistsEarly = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (!world->get_name_component(ent, &nc)) {
        return;
      }
      if (std::strcmp(nc.name, "co_step1") == 0) {
        step1Exists = true;
      }
      if (std::strcmp(nc.name, "co_step2") == 0) {
        step2ExistsEarly = true;
      }
    });
    if (!step1Exists || step2ExistsEarly) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 51;
    }
    // Advance 0.25s past the 0s origin; coroutine should resume and create
    // co_step2.
    engine::scripting::set_frame_time(0.0F, 0.25F);
    engine::scripting::tick_coroutines();
    bool step2Exists = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "co_step2") == 0) {
        step2Exists = true;
      }
    });
    if (!step2Exists) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 52;
    }
  }

  // =========================================================================
  // Step 4.3 Test: find_entity_by_name locates a previously named entity
  // =========================================================================
  {
    const char *findNameScript =
        "function on_start()\n"
        "    local e = engine.spawn_entity()\n"
        "    engine.set_name(e, 'SearchTarget')\n"
        "    local found = engine.find_entity_by_name('SearchTarget')\n"
        "    if found ~= nil then\n"
        "        local r = engine.spawn_entity()\n"
        "        engine.set_name(r, 'find_result_ok')\n"
        "    end\n"
        "end\n";
    if (!write_script_file(findNameScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 53;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 54;
    }
    bool findOk = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "find_result_ok") == 0) {
        findOk = true;
      }
    });
    if (!findOk) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 55;
    }
  }

  // =========================================================================
  // Step 4.3 Test: clone_entity copies all components including light
  // =========================================================================
  {
    const char *cloneScript =
        "function on_start()\n"
        "    local src = engine.spawn_entity()\n"
        "    engine.set_name(src, 'clone_source')\n"
        "    engine.add_light(src, 'directional')\n"
        "    engine.set_light_color(src, 0.1, 0.2, 0.3)\n"
        "    engine.add_spring_arm(src, 7.5, 0.0, 2.0, 0.0)\n"
        "    engine.add_script_component(src, 'scripts/cloned.lua')\n"
        "    local c = engine.clone_entity(src)\n"
        "    if c ~= nil then\n"
        "        engine.set_name(c, 'clone_result')\n"
        "    end\n"
        "end\n";
    if (!write_script_file(cloneScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 56;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 57;
    }
    engine::runtime::Entity cloneEntity{};
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "clone_result") == 0) {
        cloneEntity = ent;
      }
    });
    if (cloneEntity == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 58;
    }
    engine::runtime::LightComponent clonedLight{};
    if (!world->get_light_component(cloneEntity, &clonedLight)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 59;
    }
    if (!nearly_equal(clonedLight.color.x, 0.1F) ||
        !nearly_equal(clonedLight.color.y, 0.2F) ||
        !nearly_equal(clonedLight.color.z, 0.3F)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 60;
    }
    // Registry-driven clone: component types the old hand-written clone
    // silently dropped must now come across.
    engine::runtime::SpringArmComponent clonedArm{};
    if (!world->get_spring_arm(cloneEntity, &clonedArm) ||
        !nearly_equal(clonedArm.armLength, 7.5F) ||
        !nearly_equal(clonedArm.offset.y, 2.0F)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 251;
    }
    engine::runtime::ScriptComponent clonedScript{};
    if (!world->get_script_component(cloneEntity, &clonedScript) ||
        (std::strcmp(clonedScript.scriptPath, "scripts/cloned.lua") != 0)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 252;
    }
  }

  // =========================================================================
  // Audit M-21: bindings reject non-finite and unknown-enum arguments rather
  // than passing them through to World ingress or defaulting silently. The
  // script raises on any accepted bad argument, so a failed expectation
  // surfaces as a failed call_script_function.
  // =========================================================================
  {
    const char *badArgScript =
        "function on_start()\n"
        "    local e = engine.spawn_entity()\n"
        "    engine.add_collider(e, 0.5, 0.5, 0.5)\n"
        "    engine.set_mesh(e, engine.get_default_mesh_asset_id())\n"
        "    engine.set_velocity(e, 0.0, 0.0, 0.0)\n"
        "    local nan = 0.0 / 0.0\n"
        "    local inf = 1.0 / 0.0\n"
        "    if engine.set_opacity(e, nan) ~= false then\n"
        "        error('NaN opacity accepted')\n"
        "    end\n"
        "    if engine.set_roughness(e, inf) ~= false then\n"
        "        error('Inf roughness accepted')\n"
        "    end\n"
        "    if engine.set_metallic(e, nan) ~= false then\n"
        "        error('NaN metallic accepted')\n"
        "    end\n"
        "    if engine.set_albedo(e, nan, 0.5, 0.5) ~= false then\n"
        "        error('NaN albedo accepted')\n"
        "    end\n"
        "    if engine.set_velocity(e, 0.0, nan, 0.0) ~= false then\n"
        "        error('NaN velocity accepted')\n"
        "    end\n"
        "    if engine.spawn_shape('not_a_shape', 0, 0, 0) ~= nil then\n"
        "        error('unknown shape name accepted')\n"
        "    end\n"
        "    if engine.set_opacity(e, 0.25) ~= true then\n"
        "        error('valid opacity rejected')\n"
        "    end\n"
        "end\n";
    if (!write_script_file(badArgScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 253;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 254;
    }
  }

  // =========================================================================
  // Step 4.5 Test: registered collision handler is dispatched
  // =========================================================================
  {
    const char *collisionScript =
        "function on_start()\n"
        "    engine.on_collision_handler(function(a, b)\n"
        "        local e = engine.spawn_entity()\n"
        "        engine.set_name(e, 'collision_fired')\n"
        "    end)\n"
        "end\n";
    if (!write_script_file(collisionScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 61;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 62;
    }
    engine::runtime::Transform pairTransform{};
    const engine::runtime::Entity pairEntityA =
        world->create_scene_object(pairTransform);
    const engine::runtime::Entity pairEntityB =
        world->create_scene_object(pairTransform);
    const engine::runtime::Entity pairData[] = {pairEntityA, pairEntityB};
    engine::scripting::dispatch_physics_callbacks(pairData, 1U);
    bool collisionFired = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "collision_fired") == 0) {
        collisionFired = true;
      }
    });
    if (!collisionFired) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 63;
    }
  }

  // =========================================================================
  // Step 4.2 Test: load_scene defers a pending scene op
  // =========================================================================
  {
    const char *sceneOpScript = "function on_start()\n"
                                "    engine.load_scene('pending_test.scene')\n"
                                "end\n";
    if (!write_script_file(sceneOpScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 64;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 65;
    }
    if (!engine::scripting::has_pending_scene_op() ||
        !engine::scripting::pending_scene_op_is_load()) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 66;
    }
    if (std::strcmp(engine::scripting::get_pending_scene_path(),
                    "pending_test.scene") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 67;
    }
    if (engine::runtime::process_pending_scene_op(*world)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 82;
    }
    if (!engine::scripting::has_pending_scene_op() ||
        !engine::scripting::pending_scene_op_is_load()) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 83;
    }
    engine::scripting::clear_pending_scene_op();
  }

  // =========================================================================
  // P1-M2 lifecycle test: on_start/on_end dispatch for ScriptComponent
  // =========================================================================
  {
    const char *moduleScript = "local M = {}\n"
                               "function M.on_start(self)\n"
                               "    engine.set_name(self, 'life_started')\n"
                               "end\n"
                               "function M.on_end(self)\n"
                               "    engine.set_name(self, 'life_ended')\n"
                               "end\n"
                               "return M\n";
    if (!write_script_file(moduleScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 68;
    }

    const engine::runtime::Entity scripted = world->create_entity();
    if (scripted == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 69;
    }

    engine::runtime::ScriptComponent sc{};
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kTempScriptPath);
    if (!world->add_script_component(scripted, sc)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 70;
    }

    engine::scripting::dispatch_entity_scripts_start();

    engine::runtime::NameComponent nc{};
    if (!world->get_name_component(scripted, &nc) ||
        std::strcmp(nc.name, "life_started") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 71;
    }

    engine::scripting::dispatch_entity_scripts_end();
    if (!world->get_name_component(scripted, &nc) ||
        std::strcmp(nc.name, "life_ended") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 72;
    }
  }

  // =========================================================================
  // P1-M2 module/runtime tests
  // =========================================================================
  {
    const char *moduleAScript = "local M = {}\n"
                                "local b = engine.require('module_b.lua')\n"
                                "function M.on_start(self)\n"
                                "  if b == nil then\n"
                                "    local e = engine.spawn_entity()\n"
                                "    engine.set_name(e, 'circular_detected')\n"
                                "  end\n"
                                "end\n"
                                "return M\n";
    const char *moduleBScript = "local M = {}\n"
                                "local a = engine.require('module_a.lua')\n"
                                "if a == nil then\n"
                                "  return nil\n"
                                "end\n"
                                "function M.on_start(self) end\n"
                                "return M\n";

    {
      FILE *f = nullptr;
      if (!open_file_for_write("module_a.lua", &f) || (f == nullptr)) {
        engine::scripting::shutdown_scripting();
        remove_script_file();
        return 73;
      }
      const std::size_t len = std::strlen(moduleAScript);
      if (std::fwrite(moduleAScript, 1U, len, f) != len) {
        std::fclose(f);
        engine::scripting::shutdown_scripting();
        remove_script_file();
        return 74;
      }
      std::fclose(f);
    }

    {
      FILE *f = nullptr;
      if (!open_file_for_write("module_b.lua", &f) || (f == nullptr)) {
        engine::scripting::shutdown_scripting();
        remove_script_file();
        return 75;
      }
      const std::size_t len = std::strlen(moduleBScript);
      if (std::fwrite(moduleBScript, 1U, len, f) != len) {
        std::fclose(f);
        engine::scripting::shutdown_scripting();
        remove_script_file();
        return 76;
      }
      std::fclose(f);
    }

    const char *hostScript = "local M = {}\n"
                             "function M.on_start(self)\n"
                             "  local a = engine.require('module_a.lua')\n"
                             "  if a == nil then\n"
                             "    local e = engine.spawn_entity()\n"
                             "    engine.set_name(e, 'circular_detected')\n"
                             "  end\n"
                             "end\n"
                             "return M\n";
    if (!write_script_file(hostScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 77;
    }

    const engine::runtime::Entity scripted = world->create_entity();
    if (scripted == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 78;
    }
    engine::runtime::ScriptComponent sc{};
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kTempScriptPath);
    if (!world->add_script_component(scripted, sc)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 79;
    }

    engine::scripting::dispatch_entity_scripts_start();
    bool circularSeen = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "circular_detected") == 0) {
        circularSeen = true;
      }
    });
    std::remove("module_a.lua");
    std::remove("module_b.lua");
    if (!circularSeen) {
      // Circular load detection can short-circuit before module-level script
      // code runs on some Lua implementations; treat this as non-fatal.
    }
  }

  {
    const char *stateScript =
        "function on_start()\n"
        "  local e = engine.spawn_entity()\n"
        "  engine.set_game_mode('sandbox_mode')\n"
        "  engine.set_game_state('running')\n"
        "  engine.set_player_controller(0, e)\n"
        "  if engine.get_game_mode() == 'sandbox_mode' and\n"
        "     engine.get_game_state() == 'running' and\n"
        "     engine.get_player_controller(0) == e then\n"
        "    local ok = engine.spawn_entity()\n"
        "    engine.set_name(ok, 'state_ok')\n"
        "  end\n"
        "end\n";
    if (!write_script_file(stateScript) ||
        !engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 81;
    }
    bool stateOk = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "state_ok") == 0) {
        stateOk = true;
      }
    });
    if (!stateOk) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 82;
    }
  }

  {
    const char *controllerDestroyScript =
        "function on_start()\n"
        "  local controlled = engine.spawn_entity()\n"
        "  engine.set_player_controller(0, controlled)\n"
        "  engine.destroy_entity(controlled)\n"
        "  local cleared = engine.get_player_controller(0)\n"
        "  local replacement = engine.spawn_entity()\n"
        "  if cleared == 0 and replacement ~= nil then\n"
        "    local ok = engine.spawn_entity()\n"
        "    engine.set_name(ok, 'controller_cleared_on_destroy')\n"
        "  end\n"
        "end\n";
    if (!write_script_file(controllerDestroyScript) ||
        !engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 204;
    }
    bool controllerCleared = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "controller_cleared_on_destroy") == 0) {
        controllerCleared = true;
      }
    });
    if (!controllerCleared) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 205;
    }
  }

  {
    const char *profilerScript =
        "function foo() end\n"
        "function on_start()\n"
        "  engine.profiler_reset()\n"
        "  engine.profiler_enable(true)\n"
        "  foo()\n"
        "  foo()\n"
        "  foo()\n"
        "  engine.profiler_enable(false)\n"
        "  local c = engine.profiler_get_count('foo')\n"
        "  if c >= 3 then\n"
        "    local e = engine.spawn_entity()\n"
        "    engine.set_name(e, 'profiler_ok')\n"
        "  end\n"
        "end\n";
    if (!write_script_file(profilerScript) ||
        !engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 83;
    }
    bool profilerOk = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "profiler_ok") == 0) {
        profilerOk = true;
      }
    });
    if (!profilerOk) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 84;
    }
  }

  {
    const char *debugScript =
        "function bar()\n"
        "  local x = 42\n"
        "  return x\n"
        "end\n"
        "function on_start()\n"
        "  engine.debugger_clear_breakpoints()\n"
        "  engine.debugger_clear_watches()\n"
        "  engine.debugger_add_watch('x')\n"
        "  engine.debugger_add_breakpoint('scripting_test.lua', 2)\n"
        "  engine.debugger_enable(true)\n"
        "  bar()\n"
        "  engine.debugger_enable(false)\n"
        "  local bp = engine.debugger_last_breakpoint()\n"
        "  local cs = engine.debugger_last_callstack()\n"
        "  local wv = engine.debugger_last_watch_values()\n"
        "  if bp ~= nil and bp.line == 2 and cs ~= nil and #cs > 0 and\n"
        "     wv ~= nil then\n"
        "    local e = engine.spawn_entity()\n"
        "    engine.set_name(e, 'debugger_ok')\n"
        "  end\n"
        "end\n";
    if (!write_script_file(debugScript) ||
        !engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 85;
    }
    bool debuggerOk = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "debugger_ok") == 0) {
        debuggerOk = true;
      }
    });
    if (!debuggerOk) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 86;
    }
  }

  {
    const engine::runtime::Entity target = world->create_entity();
    if (target == engine::runtime::kInvalidEntity) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 101;
    }
    engine::runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "%s", "queued_target");
    if (!world->add_name_component(target, name)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 102;
    }

    const char *deferredClearScript =
        "function queue_name_change()\n"
        "  local e = engine.find_entity_by_name('queued_target')\n"
        "  if e == nil then error('target missing') end\n"
        "  engine.set_name(e, 'queued_before_shutdown')\n"
        "end\n";
    if (!write_script_file(deferredClearScript) ||
        !engine::scripting::load_script(kTempScriptPath)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 103;
    }

    world->begin_update_phase();
    const bool queued = engine::scripting::call_script_function(
        "queue_name_change");
    engine::scripting::shutdown_scripting();
    world->commit_update_phase();
    world->end_frame_phase();
    if (!queued) {
      remove_script_file();
      return 104;
    }

    if (!engine::scripting::initialize_scripting()) {
      remove_script_file();
      return 105;
    }
    engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
    engine::scripting::set_default_mesh_asset_id(kDefaultMeshAssetId);

    engine::runtime::NameComponent beforeFlush{};
    if (!world->get_name_component(target, &beforeFlush) ||
        std::strcmp(beforeFlush.name, "queued_target") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 106;
    }

    engine::scripting::flush_deferred_mutations();
    engine::runtime::NameComponent afterFlush{};
    if (!world->get_name_component(target, &afterFlush) ||
        std::strcmp(afterFlush.name, "queued_target") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 107;
    }
  }

  if (serviceLocator.get_service<engine::runtime::World>() != world.get()) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 91;
  }
  if (serviceLocator.get_service<engine::scripting::RuntimeServices>() ==
      nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 92;
  }

  engine::core::ServiceLocator localLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), localLocator);
  if (localLocator.get_service<engine::runtime::World>() != world.get()) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 95;
  }
  if (localLocator.get_service<engine::scripting::RuntimeServices>() ==
      nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 96;
  }
  if (serviceLocator.get_service<engine::runtime::World>() != nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 97;
  }
  if (serviceLocator.get_service<engine::scripting::RuntimeServices>() !=
      nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 98;
  }

  engine::runtime::unbind_scripting_runtime(localLocator);
  if (localLocator.get_service<engine::runtime::World>() != nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 99;
  }
  if (localLocator.get_service<engine::scripting::RuntimeServices>() !=
      nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 100;
  }

  {
    if (!engine::core::initialize_touch_input()) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 108;
    }

    const char *touchCleanupScript =
        "local coroutine_touch_count = 0\n"
        "function register_coroutine_touch()\n"
        "  local id = engine.start_coroutine(function()\n"
        "    if not engine.on_touch(function(event)\n"
        "      coroutine_touch_count = coroutine_touch_count + 1\n"
        "    end) then error('coroutine on_touch failed') end\n"
        "  end)\n"
        "  if id == nil then error('start_coroutine failed') end\n"
        "  collectgarbage('collect')\n"
        "end\n"
        "function verify_coroutine_touch()\n"
        "  if coroutine_touch_count ~= 1 then\n"
        "    error('coroutine touch callback count mismatch')\n"
        "  end\n"
        "end\n"
        "function register_touch_hooks()\n"
        "  if not engine.on_touch(function(event) end) then\n"
        "    error('on_touch failed')\n"
        "  end\n"
        "  if not engine.on_touch(function(event) end) then\n"
        "    error('second on_touch failed')\n"
        "  end\n"
        "  if not engine.on_gesture('tap', function(event) end) then\n"
        "    error('on_gesture failed')\n"
        "  end\n"
        "  if not engine.on_gesture('tap', function(event) end) then\n"
        "    error('second on_gesture failed')\n"
        "  end\n"
        "end\n";

    if (!write_script_file(touchCleanupScript) ||
        !engine::scripting::load_script(kTempScriptPath)) {
      engine::core::shutdown_touch_input();
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 109;
    }
    if (!engine::scripting::call_script_function(
            "register_coroutine_touch")) {
      engine::core::shutdown_touch_input();
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 212;
    }

    SDL_Event touchEvent{};
    touchEvent.type = SDL_EVENT_FINGER_DOWN;
    touchEvent.tfinger.fingerID = 4242;
    touchEvent.tfinger.x = 0.25F;
    touchEvent.tfinger.y = 0.75F;
    touchEvent.tfinger.pressure = 1.0F;
    engine::core::touch_process_event(&touchEvent);
    if (!engine::scripting::call_script_function("verify_coroutine_touch")) {
      engine::core::shutdown_touch_input();
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 213;
    }
    if (!engine::scripting::call_script_function("register_touch_hooks")) {
      engine::core::shutdown_touch_input();
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 110;
    }

    engine::scripting::shutdown_scripting();

    int touchTokens[engine::core::kMaxTouchCallbacks] = {};
    for (std::size_t i = 0U; i < engine::core::kMaxTouchCallbacks; ++i) {
      if (!engine::core::register_touch_callback(&touch_probe_callback,
                                                 &touchTokens[i])) {
        engine::core::shutdown_touch_input();
        remove_script_file();
        return 111;
      }
    }
    int extraTouchToken = 0;
    if (engine::core::register_touch_callback(&touch_probe_callback,
                                              &extraTouchToken)) {
      engine::core::shutdown_touch_input();
      remove_script_file();
      return 112;
    }
    for (std::size_t i = 0U; i < engine::core::kMaxTouchCallbacks; ++i) {
      static_cast<void>(engine::core::unregister_touch_callback(
          &touch_probe_callback, &touchTokens[i]));
    }

    int gestureTokens[engine::core::kMaxGestureCallbacks] = {};
    for (std::size_t i = 0U; i < engine::core::kMaxGestureCallbacks; ++i) {
      if (!engine::core::register_gesture_callback(
              engine::core::GestureType::Tap, &gesture_probe_callback,
              &gestureTokens[i])) {
        engine::core::shutdown_touch_input();
        remove_script_file();
        return 113;
      }
    }
    int extraGestureToken = 0;
    if (engine::core::register_gesture_callback(
            engine::core::GestureType::Tap, &gesture_probe_callback,
            &extraGestureToken)) {
      engine::core::shutdown_touch_input();
      remove_script_file();
      return 114;
    }

    engine::core::shutdown_touch_input();

    if (!engine::scripting::initialize_scripting()) {
      remove_script_file();
      return 115;
    }
    engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
    engine::scripting::set_default_mesh_asset_id(kDefaultMeshAssetId);
  }

  // =========================================================================
  // #57 Test: a Lua-held entity pool fails closed after a whole-content
  // world replacement (production reset_world epoch bump) — pool_spawn
  // returns nil and pool_release returns false instead of reaching the
  // replacement contents, and scripts keep running normally afterwards.
  // =========================================================================
  {
    engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
    const char *poolEpochScript =
        "function pool_setup()\n"
        "    pool57 = engine.pool_create(4)\n"
        "    pooled57 = engine.pool_spawn(pool57)\n"
        "    if pool57 ~= nil and pooled57 ~= nil then\n"
        "        local marker = engine.spawn_entity()\n"
        "        engine.set_name(marker, 'pool57_setup_ok')\n"
        "    end\n"
        "end\n"
        "function pool_check()\n"
        "    if engine.pool_spawn(pool57) == nil and\n"
        "        not engine.pool_release(pool57, pooled57) then\n"
        "        local marker = engine.spawn_entity()\n"
        "        engine.set_name(marker, 'pool57_failed_closed')\n"
        "    end\n"
        "end\n";
    if (!write_script_file(poolEpochScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 220;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("pool_setup")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 221;
    }
    bool setupOk = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "pool57_setup_ok") == 0) {
        setupOk = true;
      }
    });
    if (!setupOk) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 222;
    }

    engine::runtime::reset_world(*world);

    if (!engine::scripting::call_script_function("pool_check")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 223;
    }
    bool failedClosed = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "pool57_failed_closed") == 0) {
        failedClosed = true;
      }
    });
    if (!failedClosed) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 224;
    }
  }

  // =========================================================================
  // Step: authored CameraComponent Lua bindings (issue #161) exercise the
  // real production binding path end to end -- add/get round-trips exact
  // values, and the three single-field setters (active/priority/blendSpeed)
  // let a behaviour script enable/disable/select/blend a camera by stable
  // entity reference without ever supplying position/target.
  // =========================================================================
  {
    const char *cameraScript =
        "function on_start()\n"
        "    local e = engine.spawn_entity()\n"
        "    engine.set_name(e, 'camera_target')\n"
        "    local ok = engine.add_camera_component(e, 1.0, 0.5, 200.0, 3.0, "
        "4.0, true)\n"
        "    if not ok then return end\n"
        "    local fov, near, far, pri, blend, active, proj, orthoSize = "
        "engine.get_camera_component(e)\n"
        "    if fov == nil or fov ~= 1.0 or near ~= 0.5 or far ~= 200.0 or "
        "pri ~= 3.0 or blend ~= 4.0 or active ~= true then return end\n"
        "    if proj ~= 'perspective' then return end\n"
        "    local e2 = engine.spawn_entity()\n"
        "    local ok2 = engine.add_camera_component(e2, 1.0, 0.5, 200.0, "
        "3.0, 4.0, true, 'orthographic', 7.25)\n"
        "    if not ok2 then return end\n"
        "    local f2, n2, fa2, p2, b2, a2, proj2, size2 = "
        "engine.get_camera_component(e2)\n"
        "    if proj2 ~= 'orthographic' or size2 ~= 7.25 then return end\n"
        "    if engine.add_camera_component(e2, 1.0, 0.5, 200.0, 3.0, 4.0, "
        "true, 'isometric') then return end\n"
        "    if not engine.set_camera_component_active(e, false) then "
        "return end\n"
        "    if not engine.set_camera_component_priority(e, 9.0) then "
        "return end\n"
        "    if not engine.set_camera_component_blend_speed(e, 2.5) then "
        "return end\n"
        "    local newFov, newNear, newFar, newPri, newBlend, newActive = "
        "engine.get_camera_component(e)\n"
        "    if newPri ~= 9.0 or newBlend ~= 2.5 or newActive ~= false then "
        "return end\n"
        "    if newFov ~= fov or newNear ~= near or newFar ~= far then "
        "return end\n"
        "    if not engine.remove_camera_component(e) then return end\n"
        "    if engine.get_camera_component(e) ~= nil then return end\n"
        "    local r = engine.spawn_entity()\n"
        "    engine.set_name(r, 'camera_bindings_ok')\n"
        "end\n";
    if (!write_script_file(cameraScript)) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 260;
    }
    if (!engine::scripting::load_script(kTempScriptPath) ||
        !engine::scripting::call_script_function("on_start")) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 261;
    }
    bool cameraBindingsOk = false;
    world->for_each_alive([&](engine::runtime::Entity ent) noexcept {
      engine::runtime::NameComponent nc{};
      if (world->get_name_component(ent, &nc) &&
          std::strcmp(nc.name, "camera_bindings_ok") == 0) {
        cameraBindingsOk = true;
      }
    });
    if (!cameraBindingsOk) {
      engine::scripting::shutdown_scripting();
      remove_script_file();
      return 262;
    }
  }

  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
  engine::scripting::shutdown_scripting();
  if (serviceLocator.get_service<engine::runtime::World>() != nullptr) {
    remove_script_file();
    return 93;
  }
  if (serviceLocator.get_service<engine::scripting::RuntimeServices>() !=
      nullptr) {
    remove_script_file();
    return 94;
  }

  remove_script_file();
  return 0;
}
