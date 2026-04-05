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

  if ((transform.position.x != 2.0F) || (transform.position.y != 3.0F)
      || (transform.position.z != 4.0F)) {
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

  if ((rigidBody.velocity.x != 5.0F) || (rigidBody.velocity.y != 6.0F)
      || (rigidBody.velocity.z != 7.0F)
      || !nearly_equal(rigidBody.acceleration.x, 0.0F)
      || !nearly_equal(rigidBody.acceleration.y, 0.8F)
      || !nearly_equal(rigidBody.acceleration.z, 0.0F)) {
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

  if ((collider.halfExtents.x != 0.5F) || (collider.halfExtents.y != 0.5F)
      || (collider.halfExtents.z != 0.5F)) {
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

  if ((meshComponent.meshAssetId != kDefaultMeshAssetId)
      || (meshComponent.material.albedo.x != 0.2F)
      || (meshComponent.material.albedo.y != 0.8F)
      || (meshComponent.material.albedo.z != 0.4F)) {
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

  engine::scripting::shutdown_scripting();
  remove_script_file();
  return 0;
}
