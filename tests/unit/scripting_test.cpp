#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScriptPath = "scripting_test.lua";

bool nearly_equal(float lhs, float rhs) noexcept {
  const float diff = lhs - rhs;
  return (diff < 0.0001F) && (diff > -0.0001F);
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kTempScriptPath));
}

bool write_script_file(const char *contents) noexcept {
  if (contents == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kTempScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kTempScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

} // namespace

int main() {
  remove_script_file();

  if (!engine::scripting::initialize_scripting()) {
    return 1;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    engine::scripting::shutdown_scripting();
    return 2;
  }

  engine::scripting::set_scripting_world(world.get());
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
    // Advance to 0.15s — past the 0.1s threshold.
    engine::scripting::set_frame_time(0.0F, 0.15F);
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
    // Advance past the timer threshold; it should NOT fire.
    engine::scripting::set_frame_time(0.0F, 0.5F);
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
    const char *cloneScript = "function on_start()\n"
                              "    local src = engine.spawn_entity()\n"
                              "    engine.set_name(src, 'clone_source')\n"
                              "    engine.add_light(src, 'directional')\n"
                              "    engine.set_light_color(src, 0.1, 0.2, 0.3)\n"
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
    constexpr std::uint32_t kPairData[] = {1U, 2U};
    engine::scripting::dispatch_physics_callbacks(kPairData, 1U);
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
    engine::scripting::clear_pending_scene_op();
  }

  engine::scripting::shutdown_scripting();
  remove_script_file();
  return 0;
}
