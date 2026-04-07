#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kPrefabPath = "prefab_test_temp.json";

void remove_prefab_file() noexcept {
  static_cast<void>(std::remove(kPrefabPath));
}

bool nearly_equal(float lhs, float rhs) noexcept {
  const float diff = lhs - rhs;
  return (diff < 0.0001F) && (diff > -0.0001F);
}

} // namespace

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
  strncpy_s(nameComp.name, sizeof(nameComp.name), "PrefabSource", sizeof(nameComp.name) - 1U);
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

  remove_prefab_file();
  return 0;
}
