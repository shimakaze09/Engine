// Verifies prefab test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kPrefabPath = "prefab_test_temp.json";

/// Removes a value or component from the target system for prefab file.
void remove_prefab_file() noexcept {
  static_cast<void>(std::remove(kPrefabPath));
}

/// Handles nearly equal.
bool nearly_equal(float lhs, float rhs) noexcept {
  const float diff = lhs - rhs;
  return (diff < 0.0001F) && (diff > -0.0001F);
}

bool write_prefab_text(const char *text) noexcept {
  if (text == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kPrefabPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kPrefabPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

int verify_instantiate_rejects_malformed_component() {
  remove_prefab_file();
  constexpr const char *kMalformedPrefab =
      "{\"version\":1,\"components\":{\"Transform\":{\"position\":\"bad\"}}}";
  if (!write_prefab_text(kMalformedPrefab)) {
    remove_prefab_file();
    return 32;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    remove_prefab_file();
    return 40;
  }
  const engine::runtime::Entity entity =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  remove_prefab_file();
  if (entity != engine::runtime::kInvalidEntity) {
    return 33;
  }
  if (world->alive_entity_count() != 0U) {
    return 34;
  }
  return 0;
}

int verify_instantiate_rolls_back_on_component_add_failure() {
  remove_prefab_file();
  constexpr const char *kPointLightPrefab =
      "{\"version\":1,\"components\":{\"PointLightComponent\":{\"radius\":3}}}";
  if (!write_prefab_text(kPointLightPrefab)) {
    remove_prefab_file();
    return 35;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    remove_prefab_file();
    return 41;
  }
  for (std::size_t i = 0U;
       i < engine::runtime::World::kMaxPointLightComponents; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      remove_prefab_file();
      return 36;
    }
    engine::runtime::PointLightComponent pointLight{};
    if (!world->add_point_light_component(entity, pointLight)) {
      remove_prefab_file();
      return 37;
    }
  }

  const std::size_t aliveBefore = world->alive_entity_count();
  const engine::runtime::Entity entity =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  remove_prefab_file();
  if (entity != engine::runtime::kInvalidEntity) {
    return 38;
  }
  if (world->alive_entity_count() != aliveBefore) {
    return 39;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  remove_prefab_file();

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  // Create a source entity with every supported component type.
  const engine::runtime::Entity src = world->create_entity();
  if (src == engine::runtime::kInvalidEntity) {
    return 2;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  transform.scale = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  if (!world->add_transform(src, transform)) {
    return 3;
  }

  engine::runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 0.5F;
  if (!world->add_rigid_body(src, rigidBody)) {
    return 4;
  }

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 1.0F, 0.5F);
  collider.restitution = 0.7F;
  if (!world->add_collider(src, collider)) {
    return 5;
  }

  engine::runtime::NameComponent nameComp{};
  constexpr const char *kPrefabSourceName = "PrefabSource";
  const std::size_t nameLength = std::strlen(kPrefabSourceName);
  const std::size_t copyLength = (nameLength < (sizeof(nameComp.name) - 1U))
                                     ? nameLength
                                     : (sizeof(nameComp.name) - 1U);
  if (copyLength > 0U) {
    std::memcpy(nameComp.name, kPrefabSourceName, copyLength);
  }
  nameComp.name[copyLength] = '\0';
  if (!world->add_name_component(src, nameComp)) {
    return 6;
  }

  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = 42U;
  mesh.albedo = engine::math::Vec3(0.8F, 0.2F, 0.4F);
  mesh.roughness = 0.6F;
  mesh.metallic = 0.1F;
  mesh.opacity = 0.9F;
  if (!world->add_mesh_component(src, mesh)) {
    return 7;
  }

  engine::runtime::LightComponent light{};
  light.color = engine::math::Vec3(1.0F, 0.8F, 0.6F);
  light.intensity = 2.5F;
  light.type = engine::runtime::LightType::Point;
  if (!world->add_light_component(src, light)) {
    return 8;
  }

  engine::runtime::ReflectionProbeComponent reflectionProbe{};
  reflectionProbe.boxExtents = engine::math::Vec3(4.0F, 5.0F, 6.0F);
  reflectionProbe.radius = 14.0F;
  reflectionProbe.intensity = 0.75F;
  reflectionProbe.prefilteredResolution = 256U;
  reflectionProbe.irradianceResolution = 64U;
  reflectionProbe.mipLevels = 6U;
  reflectionProbe.boxProjection = true;
  reflectionProbe.needsBake = false;
  if (!world->add_reflection_probe_component(src, reflectionProbe)) {
    return 26;
  }

  engine::runtime::FoliagePatchComponent foliage{};
  foliage.meshAssetIds[0] = 77U;
  foliage.meshAssetIds[1] = 88U;
  foliage.instanceCount = 2U;
  foliage.density = 1.75F;
  foliage.albedo = engine::math::Vec3(0.2F, 0.8F, 0.25F);
  foliage.windStrength = 0.3F;
  foliage.windFrequency = 1.4F;
  foliage.instances[0].offset = engine::math::Vec3(-0.5F, 0.0F, 0.25F);
  foliage.instances[0].scale = 0.6F;
  foliage.instances[0].phase = 0.2F;
  foliage.instances[0].lodIndex = 0U;
  foliage.instances[1].offset = engine::math::Vec3(0.75F, 0.0F, -0.5F);
  foliage.instances[1].scale = 0.9F;
  foliage.instances[1].phase = 1.2F;
  foliage.instances[1].lodIndex = 1U;
  if (!world->add_foliage_patch_component(src, foliage)) {
    return 29;
  }

  // Save the prefab.
  if (!engine::runtime::save_prefab(*world, src, kPrefabPath)) {
    remove_prefab_file();
    return 9;
  }

  // Instantiate into the same world (creates a new entity).
  const engine::runtime::Entity inst =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  if (inst == engine::runtime::kInvalidEntity) {
    remove_prefab_file();
    return 10;
  }

  // Verify Transform.
  engine::runtime::Transform instTransform{};
  if (!world->get_transform(inst, &instTransform)) {
    remove_prefab_file();
    return 11;
  }
  if (!nearly_equal(instTransform.position.x, 1.0F) ||
      !nearly_equal(instTransform.position.y, 2.0F) ||
      !nearly_equal(instTransform.position.z, 3.0F)) {
    remove_prefab_file();
    return 12;
  }
  if (!nearly_equal(instTransform.scale.x, 2.0F) ||
      !nearly_equal(instTransform.scale.y, 2.0F) ||
      !nearly_equal(instTransform.scale.z, 2.0F)) {
    remove_prefab_file();
    return 13;
  }

  // Verify RigidBody.
  engine::runtime::RigidBody instRb{};
  if (!world->get_rigid_body(inst, &instRb)) {
    remove_prefab_file();
    return 14;
  }
  if (!nearly_equal(instRb.inverseMass, 0.5F)) {
    remove_prefab_file();
    return 15;
  }

  // Verify Collider.
  engine::runtime::Collider instCol{};
  if (!world->get_collider(inst, &instCol)) {
    remove_prefab_file();
    return 16;
  }
  if (!nearly_equal(instCol.halfExtents.y, 1.0F) ||
      !nearly_equal(instCol.restitution, 0.7F)) {
    remove_prefab_file();
    return 17;
  }

  // Verify NameComponent.
  engine::runtime::NameComponent instName{};
  if (!world->get_name_component(inst, &instName)) {
    remove_prefab_file();
    return 18;
  }
  if (std::strcmp(instName.name, "PrefabSource") != 0) {
    remove_prefab_file();
    return 19;
  }

  // Verify MeshComponent.
  engine::runtime::MeshComponent instMesh{};
  if (!world->get_mesh_component(inst, &instMesh)) {
    remove_prefab_file();
    return 20;
  }
  if (instMesh.meshAssetId != 42U || !nearly_equal(instMesh.albedo.x, 0.8F) ||
      !nearly_equal(instMesh.roughness, 0.6F) ||
      !nearly_equal(instMesh.metallic, 0.1F) ||
      !nearly_equal(instMesh.opacity, 0.9F)) {
    remove_prefab_file();
    return 21;
  }

  // Verify LightComponent.
  engine::runtime::LightComponent instLight{};
  if (!world->get_light_component(inst, &instLight)) {
    remove_prefab_file();
    return 22;
  }
  if (!nearly_equal(instLight.color.x, 1.0F) ||
      !nearly_equal(instLight.color.y, 0.8F) ||
      !nearly_equal(instLight.color.z, 0.6F) ||
      !nearly_equal(instLight.intensity, 2.5F) ||
      instLight.type != engine::runtime::LightType::Point) {
    remove_prefab_file();
    return 23;
  }

  // Verify ReflectionProbeComponent.
  engine::runtime::ReflectionProbeComponent instProbe{};
  if (!world->get_reflection_probe_component(inst, &instProbe)) {
    remove_prefab_file();
    return 27;
  }
  if (!nearly_equal(instProbe.boxExtents.y, 5.0F) ||
      !nearly_equal(instProbe.radius, 14.0F) ||
      !nearly_equal(instProbe.intensity, 0.75F) ||
      (instProbe.prefilteredResolution != 256U) ||
      (instProbe.irradianceResolution != 64U) ||
      (instProbe.mipLevels != 6U) || !instProbe.boxProjection ||
      instProbe.needsBake) {
    remove_prefab_file();
    return 28;
  }

  // Verify FoliagePatchComponent.
  engine::runtime::FoliagePatchComponent instFoliage{};
  if (!world->get_foliage_patch_component(inst, &instFoliage)) {
    remove_prefab_file();
    return 30;
  }
  if ((instFoliage.meshAssetIds[0] != 77U) ||
      (instFoliage.meshAssetIds[1] != 88U) ||
      (instFoliage.instanceCount != 2U) ||
      !nearly_equal(instFoliage.density, 1.75F) ||
      !nearly_equal(instFoliage.albedo.y, 0.8F) ||
      !nearly_equal(instFoliage.windStrength, 0.3F) ||
      !nearly_equal(instFoliage.windFrequency, 1.4F) ||
      !nearly_equal(instFoliage.instances[1].offset.x, 0.75F) ||
      !nearly_equal(instFoliage.instances[1].scale, 0.9F) ||
      (instFoliage.instances[1].lodIndex != 1U)) {
    remove_prefab_file();
    return 31;
  }

  // Verify invalid-input guards.
  if (engine::runtime::save_prefab(*world, engine::runtime::kInvalidEntity,
                                   kPrefabPath)) {
    remove_prefab_file();
    return 24;
  }
  if (engine::runtime::instantiate_prefab(*world, nullptr) !=
      engine::runtime::kInvalidEntity) {
    remove_prefab_file();
    return 25;
  }

  int result = verify_instantiate_rejects_malformed_component();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_instantiate_rolls_back_on_component_add_failure();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  remove_prefab_file();
  return 0;
}
