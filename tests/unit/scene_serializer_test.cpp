#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/json.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

int build_source_scene(const char *path) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  const engine::runtime::Entity third = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity)
      || (second == engine::runtime::kInvalidEntity)
      || (third == engine::runtime::kInvalidEntity)) {
    return 2;
  }

  engine::runtime::Transform firstTransform{};
  firstTransform.position = engine::math::Vec3(1.25F, 2.5F, 3.75F);
  if (!world->add_transform(first, firstTransform)) {
    return 3;
  }

  engine::runtime::RigidBody firstBody{};
  firstBody.velocity = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  firstBody.acceleration = engine::math::Vec3(0.0F, -9.8F, 0.0F);
  firstBody.inverseMass = 0.5F;
  if (!world->add_rigid_body(first, firstBody)) {
    return 4;
  }

  engine::runtime::Transform secondTransform{};
  secondTransform.position = engine::math::Vec3(-4.0F, 0.25F, 0.5F);
  if (!world->add_transform(second, secondTransform)) {
    return 5;
  }

  engine::runtime::Transform thirdTransform{};
  thirdTransform.position = engine::math::Vec3(5.0F, 6.0F, 7.0F);
  if (!world->add_transform(third, thirdTransform)) {
    return 6;
  }

  engine::runtime::NameComponent secondName{};
  std::snprintf(secondName.name, sizeof(secondName.name), "%s", "Player");
  if (!world->add_name_component(second, secondName)) {
    return 10;
  }

  engine::runtime::Collider thirdCollider{};
  thirdCollider.halfExtents = engine::math::Vec3(1.0F, 1.5F, 2.5F);
  if (!world->add_collider(third, thirdCollider)) {
    return 7;
  }

  engine::runtime::MeshComponent thirdMesh{};
  thirdMesh.meshAssetId = 7U;
  thirdMesh.albedo = engine::math::Vec3(0.3F, 0.7F, 0.1F);
  if (!world->add_mesh_component(third, thirdMesh)) {
    return 8;
  }

  if (!engine::runtime::save_scene(*world, path)) {
    return 9;
  }

  return 0;
}

int build_source_buffer(
    std::array<char, engine::core::JsonWriter::kBufferBytes> *outBuffer,
    std::size_t *outSize) {
  if ((outBuffer == nullptr) || (outSize == nullptr)) {
    return 30;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 31;
  }

  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  const engine::runtime::Entity third = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity)
      || (second == engine::runtime::kInvalidEntity)
      || (third == engine::runtime::kInvalidEntity)) {
    return 32;
  }

  engine::runtime::Transform firstTransform{};
  firstTransform.position = engine::math::Vec3(1.25F, 2.5F, 3.75F);
  if (!world->add_transform(first, firstTransform)) {
    return 33;
  }

  engine::runtime::RigidBody firstBody{};
  firstBody.velocity = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  firstBody.acceleration = engine::math::Vec3(0.0F, -9.8F, 0.0F);
  firstBody.inverseMass = 0.5F;
  if (!world->add_rigid_body(first, firstBody)) {
    return 34;
  }

  engine::runtime::Transform secondTransform{};
  secondTransform.position = engine::math::Vec3(-4.0F, 0.25F, 0.5F);
  if (!world->add_transform(second, secondTransform)) {
    return 35;
  }

  engine::runtime::Transform thirdTransform{};
  thirdTransform.position = engine::math::Vec3(5.0F, 6.0F, 7.0F);
  if (!world->add_transform(third, thirdTransform)) {
    return 36;
  }

  engine::runtime::NameComponent secondName{};
  std::snprintf(secondName.name, sizeof(secondName.name), "%s", "Player");
  if (!world->add_name_component(second, secondName)) {
    return 51;
  }

  engine::runtime::Collider thirdCollider{};
  thirdCollider.halfExtents = engine::math::Vec3(1.0F, 1.5F, 2.5F);
  if (!world->add_collider(third, thirdCollider)) {
    return 37;
  }

  engine::runtime::MeshComponent thirdMesh{};
  thirdMesh.meshAssetId = 7U;
  thirdMesh.albedo = engine::math::Vec3(0.3F, 0.7F, 0.1F);
  if (!world->add_mesh_component(third, thirdMesh)) {
    return 38;
  }

  if (!engine::runtime::save_scene(
          *world, outBuffer->data(), outBuffer->size(), outSize)) {
    return 39;
  }

  if (*outSize == 0U) {
    return 40;
  }

  return 0;
}

int verify_loaded_scene(const char *path) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }

  if (!engine::runtime::load_scene(*world, path)) {
    return 21;
  }

  std::size_t aliveCount = 0U;
  std::size_t meshCount = 0U;
  bool foundRigidBodyValue = false;
  bool foundColliderValue = false;
  bool foundMeshValue = false;
  bool foundNameValue = false;

  for (std::uint32_t index = 1U; index <= static_cast<std::uint32_t>(
                                     engine::runtime::World::kMaxEntities);
       ++index) {
    const engine::runtime::Entity entity = world->find_entity_by_index(index);
    if (entity == engine::runtime::kInvalidEntity) {
      continue;
    }

    ++aliveCount;

    engine::runtime::RigidBody rigidBody{};
    if (world->get_rigid_body(entity, &rigidBody)
        && nearly_equal(rigidBody.inverseMass, 0.5F)) {
      foundRigidBodyValue = nearly_equal(rigidBody.velocity.y, 2.0F);
    }

    engine::runtime::Collider collider{};
    if (world->get_collider(entity, &collider)
        && nearly_equal(collider.halfExtents.z, 2.5F)) {
      foundColliderValue = true;
    }

    engine::runtime::MeshComponent mesh{};
    if (world->get_mesh_component(entity, &mesh)) {
      ++meshCount;
      if ((mesh.meshAssetId == 7U) && nearly_equal(mesh.albedo.y, 0.7F)) {
        foundMeshValue = true;
      }
    }

    engine::runtime::NameComponent name{};
    if (world->get_name_component(entity, &name)
        && (std::strcmp(name.name, "Player") == 0)) {
      foundNameValue = true;
    }
  }

  if (aliveCount != 3U) {
    return 22;
  }

  if (world->transform_count() != 3U) {
    return 23;
  }

  if (world->rigid_body_count() != 1U) {
    return 24;
  }

  if (world->collider_count() != 1U) {
    return 25;
  }

  if (meshCount != 1U) {
    return 26;
  }

  if (!foundRigidBodyValue) {
    return 27;
  }

  if (!foundColliderValue) {
    return 28;
  }

  if (!foundMeshValue) {
    return 29;
  }

  if (!foundNameValue) {
    return 60;
  }

  return 0;
}

int verify_loaded_scene_from_buffer(
    const std::array<char, engine::core::JsonWriter::kBufferBytes> &buffer,
    std::size_t size) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 41;
  }

  if (!engine::runtime::load_scene(*world, buffer.data(), size)) {
    return 42;
  }

  std::size_t aliveCount = 0U;
  std::size_t meshCount = 0U;
  bool foundRigidBodyValue = false;
  bool foundColliderValue = false;
  bool foundMeshValue = false;
  bool foundNameValue = false;

  for (std::uint32_t index = 1U; index <= static_cast<std::uint32_t>(
                                     engine::runtime::World::kMaxEntities);
       ++index) {
    const engine::runtime::Entity entity = world->find_entity_by_index(index);
    if (entity == engine::runtime::kInvalidEntity) {
      continue;
    }

    ++aliveCount;

    engine::runtime::RigidBody rigidBody{};
    if (world->get_rigid_body(entity, &rigidBody)
        && nearly_equal(rigidBody.inverseMass, 0.5F)) {
      foundRigidBodyValue = nearly_equal(rigidBody.velocity.y, 2.0F);
    }

    engine::runtime::Collider collider{};
    if (world->get_collider(entity, &collider)
        && nearly_equal(collider.halfExtents.z, 2.5F)) {
      foundColliderValue = true;
    }

    engine::runtime::MeshComponent mesh{};
    if (world->get_mesh_component(entity, &mesh)) {
      ++meshCount;
      if ((mesh.meshAssetId == 7U) && nearly_equal(mesh.albedo.y, 0.7F)) {
        foundMeshValue = true;
      }
    }

    engine::runtime::NameComponent name{};
    if (world->get_name_component(entity, &name)
        && (std::strcmp(name.name, "Player") == 0)) {
      foundNameValue = true;
    }
  }

  if (aliveCount != 3U) {
    return 43;
  }

  if (world->transform_count() != 3U) {
    return 44;
  }

  if (world->rigid_body_count() != 1U) {
    return 45;
  }

  if (world->collider_count() != 1U) {
    return 46;
  }

  if (meshCount != 1U) {
    return 47;
  }

  if (!foundRigidBodyValue) {
    return 48;
  }

  if (!foundColliderValue) {
    return 49;
  }

  if (!foundMeshValue) {
    return 50;
  }

  if (!foundNameValue) {
    return 61;
  }

  return 0;
}

int verify_scene_version_in_buffer(
    const std::array<char, engine::core::JsonWriter::kBufferBytes> &buffer,
    std::size_t size) {
  engine::core::JsonParser parser{};
  if (!parser.parse(buffer.data(), size)) {
    return 62;
  }

  const engine::core::JsonValue *root = parser.root();
  if ((root == nullptr)
      || (root->type != engine::core::JsonValue::Type::Object)) {
    return 63;
  }

  engine::core::JsonValue versionValue{};
  if (!parser.get_object_field(*root, "version", &versionValue)) {
    return 64;
  }

  std::uint32_t version = 0U;
  if (!parser.as_uint(versionValue, &version)) {
    return 65;
  }

  if (version != 2U) {
    return 66;
  }

  return 0;
}

int verify_duplicate_persistent_id_fails() {
  constexpr const char *kDuplicateScene =
      "{\"version\":2,\"entities\":["
      "{\"persistentId\":11,\"components\":{}},"
      "{\"persistentId\":11,\"components\":{}}]}";

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 67;
  }

  if (engine::runtime::load_scene(
          *world, kDuplicateScene, std::strlen(kDuplicateScene))) {
    return 68;
  }

  return 0;
}

int verify_large_scene_round_trip(const char *path) {
  constexpr std::uint32_t kLargeEntityCount = 5000U;

  std::unique_ptr<engine::runtime::World> sourceWorld(
      new (std::nothrow) engine::runtime::World());
  if (sourceWorld == nullptr) {
    return 69;
  }

  for (std::uint32_t i = 0U; i < kLargeEntityCount; ++i) {
    const engine::runtime::Entity entity = sourceWorld->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return 70;
    }

    engine::runtime::Transform transform{};
    transform.position =
        engine::math::Vec3(static_cast<float>(i), 1.0F, static_cast<float>(i));
    if (!sourceWorld->add_transform(entity, transform)) {
      return 71;
    }
  }

  if (!engine::runtime::save_scene(*sourceWorld, path)) {
    return 72;
  }

  std::unique_ptr<engine::runtime::World> loadedWorld(
      new (std::nothrow) engine::runtime::World());
  if (loadedWorld == nullptr) {
    return 73;
  }

  if (!engine::runtime::load_scene(*loadedWorld, path)) {
    return 74;
  }

  if (loadedWorld->alive_entity_count() != kLargeEntityCount) {
    return 75;
  }

  if (loadedWorld->transform_count() != kLargeEntityCount) {
    return 76;
  }

  return 0;
}

} // namespace

int main() {
  constexpr const char *kScenePath = "scene_serializer_test_tmp.json";
  constexpr const char *kLargeScenePath = "scene_serializer_large_tmp.json";

  int result = build_source_scene(kScenePath);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    return result;
  }

  result = verify_loaded_scene(kScenePath);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    return result;
  }

  std::array<char, engine::core::JsonWriter::kBufferBytes> sceneBuffer{};
  std::size_t sceneSize = 0U;
  result = build_source_buffer(&sceneBuffer, &sceneSize);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    return result;
  }

  result = verify_loaded_scene_from_buffer(sceneBuffer, sceneSize);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_scene_version_in_buffer(sceneBuffer, sceneSize);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_duplicate_persistent_id_fails();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_large_scene_round_trip(kLargeScenePath);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  static_cast<void>(std::remove(kScenePath));
  static_cast<void>(std::remove(kLargeScenePath));
  return result;
}
