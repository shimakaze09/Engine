// Verifies scene serializer test behavior for the Engine test suite.

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

constexpr const char *kRoundTripName = "Player \"One\" \\ Path";
constexpr const char *kRoundTripScriptPath =
    "assets\\scripts\\hero \"one\".lua";

/// Handles nearly equal.
bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

/// No-op timer callback used to create active timer state for reset tests.
void noop_timer(engine::runtime::TimerId, void *) noexcept {}

/// Seeds scene-owned state that must not survive reset or scene replacement.
int seed_non_entity_scene_state(engine::runtime::World &world) {
  if (world.timer_manager().set_timeout(1.0F, noop_timer, nullptr)
      == engine::runtime::kInvalidTimerId) {
    return 80;
  }

  engine::runtime::CameraEntry camera{};
  camera.position = engine::math::Vec3(2.0F, 3.0F, 4.0F);
  if (!world.camera_manager().push_camera(engine::runtime::Entity{1U, 1U},
                                          camera, 10.0F)) {
    return 81;
  }

  if (!world.game_mode().set_rule("round", "warmup")) {
    return 82;
  }
  if (!world.game_mode().start()) {
    return 83;
  }

  return 0;
}

/// Verifies that scene state has been restored to the default empty state.
int verify_non_entity_scene_state_cleared(
    const engine::runtime::World &world) {
  if (world.timer_manager().active_count() != 0U) {
    return 84;
  }
  if (world.camera_manager().camera_count() != 0U) {
    return 85;
  }
  if (world.game_mode().state
      != engine::runtime::GameMode::State::WaitingToStart) {
    return 86;
  }
  if (std::strcmp(world.game_mode().name, "default") != 0) {
    return 89;
  }
  if (world.game_mode().ruleCount != 0U) {
    return 90;
  }

  return 0;
}

/// Builds the requested runtime data for source scene.
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
  std::snprintf(secondName.name, sizeof(secondName.name), "%s",
                kRoundTripName);
  if (!world->add_name_component(second, secondName)) {
    return 10;
  }
  engine::runtime::ScriptComponent secondScript{};
  std::snprintf(secondScript.scriptPath, sizeof(secondScript.scriptPath), "%s",
                kRoundTripScriptPath);
  if (!world->add_script_component(second, secondScript)) {
    return 12;
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

  engine::runtime::ReflectionProbeComponent thirdProbe{};
  thirdProbe.boxExtents = engine::math::Vec3(3.0F, 4.0F, 5.0F);
  thirdProbe.radius = 18.0F;
  thirdProbe.intensity = 1.25F;
  thirdProbe.prefilteredResolution = 256U;
  thirdProbe.irradianceResolution = 64U;
  thirdProbe.mipLevels = 6U;
  thirdProbe.boxProjection = true;
  thirdProbe.needsBake = false;
  if (!world->add_reflection_probe_component(third, thirdProbe)) {
    return 11;
  }

  if (!engine::runtime::save_scene(*world, path)) {
    return 9;
  }

  return 0;
}

/// Builds the requested runtime data for source buffer.
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
  std::snprintf(secondName.name, sizeof(secondName.name), "%s",
                kRoundTripName);
  if (!world->add_name_component(second, secondName)) {
    return 51;
  }
  engine::runtime::ScriptComponent secondScript{};
  std::snprintf(secondScript.scriptPath, sizeof(secondScript.scriptPath), "%s",
                kRoundTripScriptPath);
  if (!world->add_script_component(second, secondScript)) {
    return 53;
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

  engine::runtime::ReflectionProbeComponent thirdProbe{};
  thirdProbe.boxExtents = engine::math::Vec3(3.0F, 4.0F, 5.0F);
  thirdProbe.radius = 18.0F;
  thirdProbe.intensity = 1.25F;
  thirdProbe.prefilteredResolution = 256U;
  thirdProbe.irradianceResolution = 64U;
  thirdProbe.mipLevels = 6U;
  thirdProbe.boxProjection = true;
  thirdProbe.needsBake = false;
  if (!world->add_reflection_probe_component(third, thirdProbe)) {
    return 52;
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

/// Handles verify loaded scene.
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
  bool foundScriptValue = false;
  bool foundReflectionProbeValue = false;

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
        && (std::strcmp(name.name, kRoundTripName) == 0)) {
      foundNameValue = true;
    }

    engine::runtime::ScriptComponent script{};
    if (world->get_script_component(entity, &script) &&
        (std::strcmp(script.scriptPath, kRoundTripScriptPath) == 0)) {
      foundScriptValue = true;
    }

    engine::runtime::ReflectionProbeComponent probe{};
    if (world->get_reflection_probe_component(entity, &probe)) {
      foundReflectionProbeValue =
          nearly_equal(probe.boxExtents.z, 5.0F) &&
          nearly_equal(probe.radius, 18.0F) &&
          nearly_equal(probe.intensity, 1.25F) &&
          (probe.prefilteredResolution == 256U) &&
          (probe.irradianceResolution == 64U) &&
          (probe.mipLevels == 6U) && probe.boxProjection &&
          !probe.needsBake;
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

  if (!foundScriptValue) {
    return 67;
  }

  if (world->reflection_probe_count() != 1U) {
    return 78;
  }

  if (!foundReflectionProbeValue) {
    return 79;
  }

  return 0;
}

/// Handles verify loaded scene from buffer.
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
  bool foundScriptValue = false;
  bool foundReflectionProbeValue = false;

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
        && (std::strcmp(name.name, kRoundTripName) == 0)) {
      foundNameValue = true;
    }

    engine::runtime::ScriptComponent script{};
    if (world->get_script_component(entity, &script) &&
        (std::strcmp(script.scriptPath, kRoundTripScriptPath) == 0)) {
      foundScriptValue = true;
    }

    engine::runtime::ReflectionProbeComponent probe{};
    if (world->get_reflection_probe_component(entity, &probe)) {
      foundReflectionProbeValue =
          nearly_equal(probe.boxExtents.z, 5.0F) &&
          nearly_equal(probe.radius, 18.0F) &&
          nearly_equal(probe.intensity, 1.25F) &&
          (probe.prefilteredResolution == 256U) &&
          (probe.irradianceResolution == 64U) &&
          (probe.mipLevels == 6U) && probe.boxProjection &&
          !probe.needsBake;
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

  if (!foundScriptValue) {
    return 68;
  }

  if (world->reflection_probe_count() != 1U) {
    return 87;
  }

  if (!foundReflectionProbeValue) {
    return 88;
  }

  return 0;
}

/// Handles verify scene version in buffer.
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

/// Handles verify duplicate persistent id fails.
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

/// Handles verify large scene round trip.
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

/// Verifies that reset_world clears all scene-owned state, not just entities.
int verify_reset_world_clears_scene_state() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 91;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 92;
  }

  engine::runtime::Transform transform{};
  if (!world->add_transform(entity, transform)) {
    return 93;
  }

  const int seedResult = seed_non_entity_scene_state(*world);
  if (seedResult != 0) {
    return seedResult;
  }

  engine::runtime::reset_world(*world);

  if (world->alive_entity_count() != 0U) {
    return 94;
  }

  return verify_non_entity_scene_state_cleared(*world);
}

/// Verifies that loading a scene replaces stale non-entity world state.
int verify_load_scene_replaces_existing_scene_state(
    const std::array<char, engine::core::JsonWriter::kBufferBytes> &buffer,
    std::size_t size) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 95;
  }

  const int seedResult = seed_non_entity_scene_state(*world);
  if (seedResult != 0) {
    return seedResult;
  }

  if (!engine::runtime::load_scene(*world, buffer.data(), size)) {
    return 96;
  }

  if (world->alive_entity_count() != 3U) {
    return 97;
  }

  return verify_non_entity_scene_state_cleared(*world);
}

int verify_point_spot_light_parse_failures_reject_scene() {
  constexpr const char *kBadPointLightScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"PointLightComponent\":{\"color\":\"bad\"}}}]}";
  std::unique_ptr<engine::runtime::World> pointWorld(
      new (std::nothrow) engine::runtime::World());
  if (pointWorld == nullptr) {
    return 114;
  }
  if (engine::runtime::load_scene(*pointWorld, kBadPointLightScene,
                                  std::strlen(kBadPointLightScene))) {
    return 110;
  }
  if (pointWorld->alive_entity_count() != 0U) {
    return 111;
  }

  constexpr const char *kBadSpotLightScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"SpotLightComponent\":{\"outerConeAngle\":\"wide\"}}}]}";
  std::unique_ptr<engine::runtime::World> spotWorld(
      new (std::nothrow) engine::runtime::World());
  if (spotWorld == nullptr) {
    return 115;
  }
  if (engine::runtime::load_scene(*spotWorld, kBadSpotLightScene,
                                  std::strlen(kBadSpotLightScene))) {
    return 112;
  }
  if (spotWorld->alive_entity_count() != 0U) {
    return 113;
  }

  return 0;
}

/// Malformed foliage fields must reject the scene (shared strict reader —
/// scene and prefab serializers now use one implementation, REVIEW_FINDINGS S5).
int verify_foliage_parse_failures_reject_scene() {
  constexpr const char *kBadFoliageDensityScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"density\":\"thick\"}}}]}";
  std::unique_ptr<engine::runtime::World> densityWorld(
      new (std::nothrow) engine::runtime::World());
  if (densityWorld == nullptr) {
    return 116;
  }
  if (engine::runtime::load_scene(*densityWorld, kBadFoliageDensityScene,
                                  std::strlen(kBadFoliageDensityScene))) {
    return 117;
  }
  if (densityWorld->alive_entity_count() != 0U) {
    return 118;
  }

  constexpr const char *kBadFoliageInstanceScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"instanceCount\":1,"
      "\"instances\":[{\"scale\":\"big\"}]}}}]}";
  std::unique_ptr<engine::runtime::World> instanceWorld(
      new (std::nothrow) engine::runtime::World());
  if (instanceWorld == nullptr) {
    return 119;
  }
  if (engine::runtime::load_scene(*instanceWorld, kBadFoliageInstanceScene,
                                  std::strlen(kBadFoliageInstanceScene))) {
    return 120;
  }
  if (instanceWorld->alive_entity_count() != 0U) {
    return 121;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
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

  result = verify_reset_world_clears_scene_state();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_load_scene_replaces_existing_scene_state(sceneBuffer,
                                                           sceneSize);
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_point_spot_light_parse_failures_reject_scene();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_foliage_parse_failures_reject_scene();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  static_cast<void>(std::remove(kScenePath));
  static_cast<void>(std::remove(kLargeScenePath));
  return result;
}
