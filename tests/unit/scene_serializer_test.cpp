// Verifies scene serializer test behavior for the Engine test suite.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>

#include "engine/core/json.h"
#include "engine/physics/physics.h"
#include "engine/physics/primitive_hulls.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kRoundTripName = "Player \"One\" \\ Path";
constexpr const char *kRoundTripScriptPath =
    "assets\\scripts\\hero \"one\".lua";

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

engine::math::Quat collider_test_rotation(std::size_t index) noexcept {
  switch (index) {
  case 1U:
    return engine::math::Quat(1.0F, 0.0F, 0.0F, 0.0F);
  case 2U:
    return engine::math::Quat(0.0F, 1.0F, 0.0F, 0.0F);
  case 3U:
    return engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  case 4U:
    return engine::math::Quat(0.0F, 0.0F, 0.0F, -1.0F);
  default:
    return engine::math::Quat();
  }
}

bool collider_pose_equals(const engine::runtime::Collider &collider,
                          const engine::math::Vec3 &position,
                          const engine::math::Quat &rotation) noexcept {
  return (collider.localPosition.x == position.x) &&
         (collider.localPosition.y == position.y) &&
         (collider.localPosition.z == position.z) &&
         (collider.localRotation.x == rotation.x) &&
         (collider.localRotation.y == rotation.y) &&
         (collider.localRotation.z == rotation.z) &&
         (collider.localRotation.w == rotation.w);
}

/// No-op timer callback used to create active timer state for reset tests.
void noop_timer(engine::runtime::TimerId, void *) noexcept {}

/// Seeds scene-owned state that must not survive reset or scene replacement.
int seed_non_entity_scene_state(engine::runtime::World &world) {
  if (world.timer_manager().set_timeout(1.0F, noop_timer, nullptr) ==
      engine::runtime::kInvalidTimerId) {
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
int verify_non_entity_scene_state_cleared(const engine::runtime::World &world) {
  if (world.timer_manager().active_count() != 0U) {
    return 84;
  }
  if (world.camera_manager().camera_count() != 0U) {
    return 85;
  }
  if (world.game_mode().state !=
      engine::runtime::GameMode::State::WaitingToStart) {
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
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
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
  std::snprintf(secondName.name, sizeof(secondName.name), "%s", kRoundTripName);
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
  thirdMesh.sceneCaptureSourceId = 9U;
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

  engine::runtime::SceneCaptureComponent thirdCapture{};
  thirdCapture.width = 320U;
  thirdCapture.height = 180U;
  thirdCapture.fovRadians = 0.9F;
  thirdCapture.nearPlane = 0.25F;
  thirdCapture.farPlane = 60.0F;
  thirdCapture.enabled = false;
  if (!world->add_scene_capture_component(third, thirdCapture)) {
    return 13;
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
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity) ||
      (third == engine::runtime::kInvalidEntity)) {
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
  std::snprintf(secondName.name, sizeof(secondName.name), "%s", kRoundTripName);
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
  thirdMesh.sceneCaptureSourceId = 9U;
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

  engine::runtime::SceneCaptureComponent thirdCapture{};
  thirdCapture.width = 320U;
  thirdCapture.height = 180U;
  thirdCapture.fovRadians = 0.9F;
  thirdCapture.nearPlane = 0.25F;
  thirdCapture.farPlane = 60.0F;
  thirdCapture.enabled = false;
  if (!world->add_scene_capture_component(third, thirdCapture)) {
    return 54;
  }

  if (!engine::runtime::save_scene(*world, outBuffer->data(), outBuffer->size(),
                                   outSize)) {
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
  bool foundScriptValue = false;
  bool foundReflectionProbeValue = false;
  bool foundSceneCaptureValue = false;

  for (std::uint32_t index = 1U;
       index <=
       static_cast<std::uint32_t>(engine::runtime::World::kMaxEntities);
       ++index) {
    const engine::runtime::Entity entity = world->find_entity_by_index(index);
    if (entity == engine::runtime::kInvalidEntity) {
      continue;
    }

    ++aliveCount;

    engine::runtime::RigidBody rigidBody{};
    if (world->get_rigid_body(entity, &rigidBody) &&
        nearly_equal(rigidBody.inverseMass, 0.5F)) {
      foundRigidBodyValue = nearly_equal(rigidBody.velocity.y, 2.0F);
    }

    engine::runtime::Collider collider{};
    if (world->get_collider(entity, &collider) &&
        nearly_equal(collider.halfExtents.z, 2.5F)) {
      foundColliderValue = true;
    }

    engine::runtime::MeshComponent mesh{};
    if (world->get_mesh_component(entity, &mesh)) {
      ++meshCount;
      if ((mesh.meshAssetId == 7U) && nearly_equal(mesh.albedo.y, 0.7F) &&
          (mesh.sceneCaptureSourceId == 9U)) {
        foundMeshValue = true;
      }
    }

    engine::runtime::NameComponent name{};
    if (world->get_name_component(entity, &name) &&
        (std::strcmp(name.name, kRoundTripName) == 0)) {
      foundNameValue = true;
    }

    engine::runtime::ScriptComponent script{};
    if (world->get_script_component(entity, &script) &&
        (std::strcmp(script.scriptPath, kRoundTripScriptPath) == 0)) {
      foundScriptValue = true;
    }

    engine::runtime::ReflectionProbeComponent probe{};
    if (world->get_reflection_probe_component(entity, &probe)) {
      foundReflectionProbeValue = nearly_equal(probe.boxExtents.z, 5.0F) &&
                                  nearly_equal(probe.radius, 18.0F) &&
                                  nearly_equal(probe.intensity, 1.25F) &&
                                  (probe.prefilteredResolution == 256U) &&
                                  (probe.irradianceResolution == 64U) &&
                                  (probe.mipLevels == 6U) &&
                                  probe.boxProjection && !probe.needsBake;
    }

    engine::runtime::SceneCaptureComponent capture{};
    if (world->get_scene_capture_component(entity, &capture)) {
      foundSceneCaptureValue =
          (capture.width == 320U) && (capture.height == 180U) &&
          nearly_equal(capture.fovRadians, 0.9F) &&
          nearly_equal(capture.nearPlane, 0.25F) &&
          nearly_equal(capture.farPlane, 60.0F) && !capture.enabled;
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

  if (world->scene_capture_count() != 1U) {
    return 90;
  }

  if (!foundSceneCaptureValue) {
    return 91;
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
  bool foundScriptValue = false;
  bool foundReflectionProbeValue = false;
  bool foundSceneCaptureValue = false;

  for (std::uint32_t index = 1U;
       index <=
       static_cast<std::uint32_t>(engine::runtime::World::kMaxEntities);
       ++index) {
    const engine::runtime::Entity entity = world->find_entity_by_index(index);
    if (entity == engine::runtime::kInvalidEntity) {
      continue;
    }

    ++aliveCount;

    engine::runtime::RigidBody rigidBody{};
    if (world->get_rigid_body(entity, &rigidBody) &&
        nearly_equal(rigidBody.inverseMass, 0.5F)) {
      foundRigidBodyValue = nearly_equal(rigidBody.velocity.y, 2.0F);
    }

    engine::runtime::Collider collider{};
    if (world->get_collider(entity, &collider) &&
        nearly_equal(collider.halfExtents.z, 2.5F)) {
      foundColliderValue = true;
    }

    engine::runtime::MeshComponent mesh{};
    if (world->get_mesh_component(entity, &mesh)) {
      ++meshCount;
      if ((mesh.meshAssetId == 7U) && nearly_equal(mesh.albedo.y, 0.7F) &&
          (mesh.sceneCaptureSourceId == 9U)) {
        foundMeshValue = true;
      }
    }

    engine::runtime::NameComponent name{};
    if (world->get_name_component(entity, &name) &&
        (std::strcmp(name.name, kRoundTripName) == 0)) {
      foundNameValue = true;
    }

    engine::runtime::ScriptComponent script{};
    if (world->get_script_component(entity, &script) &&
        (std::strcmp(script.scriptPath, kRoundTripScriptPath) == 0)) {
      foundScriptValue = true;
    }

    engine::runtime::ReflectionProbeComponent probe{};
    if (world->get_reflection_probe_component(entity, &probe)) {
      foundReflectionProbeValue = nearly_equal(probe.boxExtents.z, 5.0F) &&
                                  nearly_equal(probe.radius, 18.0F) &&
                                  nearly_equal(probe.intensity, 1.25F) &&
                                  (probe.prefilteredResolution == 256U) &&
                                  (probe.irradianceResolution == 64U) &&
                                  (probe.mipLevels == 6U) &&
                                  probe.boxProjection && !probe.needsBake;
    }

    engine::runtime::SceneCaptureComponent capture{};
    if (world->get_scene_capture_component(entity, &capture)) {
      foundSceneCaptureValue =
          (capture.width == 320U) && (capture.height == 180U) &&
          nearly_equal(capture.fovRadians, 0.9F) &&
          nearly_equal(capture.nearPlane, 0.25F) &&
          nearly_equal(capture.farPlane, 60.0F) && !capture.enabled;
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

  if (world->scene_capture_count() != 1U) {
    return 92;
  }

  if (!foundSceneCaptureValue) {
    return 93;
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
  if ((root == nullptr) ||
      (root->type != engine::core::JsonValue::Type::Object)) {
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

  if (engine::runtime::load_scene(*world, kDuplicateScene,
                                  std::strlen(kDuplicateScene))) {
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

/// Point and spot lights round-trip exactly through the reflection path
/// (REVIEW_FINDINGS S7 moved them off the hand-written JSON blocks).
int verify_point_spot_light_scene_round_trip() {
  std::unique_ptr<engine::runtime::World> source(new (std::nothrow)
                                                     engine::runtime::World());
  if (source == nullptr) {
    return 122;
  }

  const engine::runtime::Entity entity = source->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 123;
  }

  engine::runtime::PointLightComponent point{};
  point.color = engine::math::Vec3(0.25F, 0.5F, 0.75F);
  point.intensity = 2.5F;
  point.radius = 12.0F;
  if (!source->add_point_light_component(entity, point)) {
    return 124;
  }

  engine::runtime::SpotLightComponent spot{};
  spot.color = engine::math::Vec3(1.0F, 0.5F, 0.25F);
  spot.direction = engine::math::Vec3(0.0F, -1.0F, 0.0F);
  spot.intensity = 3.5F;
  spot.radius = 20.0F;
  spot.innerConeAngle = 0.25F;
  spot.outerConeAngle = 0.5F;
  if (!source->add_spot_light_component(entity, spot)) {
    return 125;
  }

  std::array<char, engine::core::JsonWriter::kBufferBytes> buffer{};
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(*source, buffer.data(), buffer.size(),
                                   &size)) {
    return 126;
  }

  std::unique_ptr<engine::runtime::World> loaded(new (std::nothrow)
                                                     engine::runtime::World());
  if (loaded == nullptr) {
    return 127;
  }
  if (!engine::runtime::load_scene(*loaded, buffer.data(), size)) {
    return 128;
  }
  if (loaded->alive_entity_count() != 1U) {
    return 129;
  }

  engine::runtime::Entity loadedEntity = engine::runtime::kInvalidEntity;
  loaded->for_each_alive(
      [&](engine::runtime::Entity alive) noexcept { loadedEntity = alive; });

  engine::runtime::PointLightComponent loadedPoint{};
  if (!loaded->get_point_light_component(loadedEntity, &loadedPoint)) {
    return 130;
  }
  if (!nearly_equal(loadedPoint.color.x, 0.25F) ||
      !nearly_equal(loadedPoint.color.y, 0.5F) ||
      !nearly_equal(loadedPoint.color.z, 0.75F) ||
      !nearly_equal(loadedPoint.intensity, 2.5F) ||
      !nearly_equal(loadedPoint.radius, 12.0F)) {
    return 131;
  }

  engine::runtime::SpotLightComponent loadedSpot{};
  if (!loaded->get_spot_light_component(loadedEntity, &loadedSpot)) {
    return 132;
  }
  if (!nearly_equal(loadedSpot.color.x, 1.0F) ||
      !nearly_equal(loadedSpot.color.y, 0.5F) ||
      !nearly_equal(loadedSpot.color.z, 0.25F) ||
      !nearly_equal(loadedSpot.direction.x, 0.0F) ||
      !nearly_equal(loadedSpot.direction.y, -1.0F) ||
      !nearly_equal(loadedSpot.direction.z, 0.0F) ||
      !nearly_equal(loadedSpot.intensity, 3.5F) ||
      !nearly_equal(loadedSpot.radius, 20.0F) ||
      !nearly_equal(loadedSpot.innerConeAngle, 0.25F) ||
      !nearly_equal(loadedSpot.outerConeAngle, 0.5F)) {
    return 133;
  }

  return 0;
}

/// Malformed foliage fields must reject the scene (shared strict reader —
/// scene and prefab serializers now use one implementation, REVIEW_FINDINGS
/// S5).
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


/// Over-capacity authored data must reject the load whole (issue #387): an
/// overlong entity name, a fourth foliage LOD id, more instances than the
/// fixed capacity, an instanceCount disagreeing with the instances array,
/// or an over-capacity bare instanceCount all fail the load and leave the
/// destination World untouched; capacity-sized data still round-trips
/// byte-identically.
int verify_over_capacity_authored_data_rejected() {
  // 32 'n's: one byte past NameComponent's 31-char capacity.
  constexpr const char *kOverlongNameScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"name\":\"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\"}}]}";
  constexpr const char *kFourLodScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"meshAssetIds\":[1,2,3,4]}}}]}";
  constexpr const char *kCountAboveArrayScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"instanceCount\":2,"
      "\"instances\":[{\"scale\":1.0}]}}}]}";
  constexpr const char *kCountBelowArrayScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"instanceCount\":1,"
      "\"instances\":[{\"scale\":1.0},{\"scale\":2.0}]}}}]}";
  constexpr const char *kBareCountOverCapacityScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"instanceCount\":65}}}]}";

  // One instance entry past the fixed capacity.
  std::string overCapacityInstances =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"FoliagePatchComponent\":{\"instances\":[";
  for (std::size_t i = 0U;
       i <= engine::runtime::FoliagePatchComponent::kMaxInstances; ++i) {
    if (i > 0U) {
      overCapacityInstances += ",";
    }
    overCapacityInstances += "{\"scale\":1.0}";
  }
  overCapacityInstances += "]}}}]}";

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 200;
  }
  world->end_frame_phase();

  // Pre-existing content proves refusal preserves the destination.
  const engine::runtime::Entity keeper = world->create_scene_object();
  engine::runtime::NameComponent keeperName{};
  std::snprintf(keeperName.name, sizeof(keeperName.name), "%s", "keeper");
  if ((keeper == engine::runtime::kInvalidEntity) ||
      !world->add_name_component(keeper, keeperName)) {
    return 201;
  }

  const char *rejected[] = {kOverlongNameScene,
                            kFourLodScene,
                            kCountAboveArrayScene,
                            kCountBelowArrayScene,
                            kBareCountOverCapacityScene,
                            overCapacityInstances.c_str()};
  for (const char *scene : rejected) {
    if (engine::runtime::load_scene(*world, scene, std::strlen(scene))) {
      return 202;
    }
    engine::runtime::NameComponent survivor{};
    if ((world->alive_entity_count() != 1U) ||
        !world->get_name_component(keeper, &survivor) ||
        (std::strcmp(survivor.name, "keeper") != 0)) {
      return 203;
    }
  }

  // Capacity boundary: a 31-char name, all three LOD ids, and exactly
  // kMaxInstances instances load whole, and a save of the loaded World is
  // byte-identical to a save of the authored one — nothing was dropped or
  // normalized on the way through.
  std::unique_ptr<engine::runtime::World> authored(
      new (std::nothrow) engine::runtime::World());
  if (authored == nullptr) {
    return 204;
  }
  authored->end_frame_phase();
  const engine::runtime::Entity entity = authored->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return 205;
  }
  engine::runtime::NameComponent boundaryName{};
  for (std::size_t i = 0U;
       i < engine::runtime::NameComponent::kMaxNameLength; ++i) {
    boundaryName.name[i] = 'n';
  }
  if (!authored->add_name_component(entity, boundaryName)) {
    return 206;
  }
  engine::runtime::FoliagePatchComponent foliage{};
  for (std::size_t i = 0U;
       i < engine::runtime::FoliagePatchComponent::kMaxLods; ++i) {
    foliage.meshAssetIds[i] = 100ULL + i;
  }
  foliage.instanceCount = static_cast<std::uint32_t>(
      engine::runtime::FoliagePatchComponent::kMaxInstances);
  for (std::uint32_t i = 0U; i < foliage.instanceCount; ++i) {
    foliage.instances[i].offset =
        engine::math::Vec3(static_cast<float>(i), 0.0F, 0.0F);
    foliage.instances[i].scale = 1.0F;
  }
  if (!authored->add_foliage_patch_component(entity, foliage)) {
    return 207;
  }

  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      firstSave(new (std::nothrow)
                    std::array<char, engine::core::JsonWriter::kBufferBytes>());
  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      secondSave(
          new (std::nothrow)
              std::array<char, engine::core::JsonWriter::kBufferBytes>());
  if ((firstSave == nullptr) || (secondSave == nullptr)) {
    return 208;
  }
  std::size_t firstSize = 0U;
  std::size_t secondSize = 0U;
  if (!engine::runtime::save_scene(*authored, firstSave->data(),
                                   firstSave->size(), &firstSize)) {
    return 209;
  }

  std::unique_ptr<engine::runtime::World> loaded(new (std::nothrow)
                                                     engine::runtime::World());
  if (loaded == nullptr) {
    return 210;
  }
  loaded->end_frame_phase();
  if (!engine::runtime::load_scene(*loaded, firstSave->data(), firstSize)) {
    return 211;
  }
  if (!engine::runtime::save_scene(*loaded, secondSave->data(),
                                   secondSave->size(), &secondSize)) {
    return 212;
  }
  if ((firstSize != secondSize) ||
      (std::memcmp(firstSave->data(), secondSave->data(), firstSize) != 0)) {
    return 213;
  }

  return 0;
}

/// A malformed CameraComponent field fails the load and leaves the
/// destination World completely unchanged (staged World + commit-on-success
/// only, per the serialization contract), matching the FoliagePatch/Light
/// precedents above.
int verify_camera_parse_failure_rejects_scene() {
  constexpr const char *kBadCameraFovScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"CameraComponent\":{\"fovRadians\":\"wide\"}}}]}";
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 122;
  }
  if (engine::runtime::load_scene(*world, kBadCameraFovScene,
                                  std::strlen(kBadCameraFovScene))) {
    return 123;
  }
  if (world->alive_entity_count() != 0U) {
    return 124;
  }

  // A failed load must not disturb a World that already had content.
  const engine::runtime::Entity survivor = world->create_scene_object();
  if (survivor == engine::runtime::kInvalidEntity) {
    return 125;
  }
  if (engine::runtime::load_scene(*world, kBadCameraFovScene,
                                  std::strlen(kBadCameraFovScene))) {
    return 126;
  }
  if (!world->is_alive(survivor) || (world->alive_entity_count() != 1U)) {
    return 127;
  }

  return 0;
}

/// MeshComponent.materialAssetId round-trips exactly and stays zero for
/// scenes authored before material assets existed (key absent).
int verify_mesh_material_reference_round_trip() {
  std::unique_ptr<engine::runtime::World> source(new (std::nothrow)
                                                     engine::runtime::World());
  if (source == nullptr) {
    return 134;
  }

  const engine::runtime::Entity entity = source->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 135;
  }

  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = 42ULL;
  mesh.materialAssetId = 0xABCDEF0123456789ULL;
  if (!source->add_mesh_component(entity, mesh)) {
    return 136;
  }

  std::array<char, engine::core::JsonWriter::kBufferBytes> buffer{};
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(*source, buffer.data(), buffer.size(),
                                   &size)) {
    return 137;
  }

  std::unique_ptr<engine::runtime::World> loaded(new (std::nothrow)
                                                     engine::runtime::World());
  if (loaded == nullptr) {
    return 138;
  }
  if (!engine::runtime::load_scene(*loaded, buffer.data(), size)) {
    return 139;
  }

  engine::runtime::Entity loadedEntity = engine::runtime::kInvalidEntity;
  loaded->for_each_alive(
      [&](engine::runtime::Entity alive) noexcept { loadedEntity = alive; });
  engine::runtime::MeshComponent loadedMesh{};
  if (!loaded->get_mesh_component(loadedEntity, &loadedMesh)) {
    return 140;
  }
  if ((loadedMesh.meshAssetId != 42ULL) ||
      (loadedMesh.materialAssetId != 0xABCDEF0123456789ULL)) {
    return 141;
  }

  // Pre-material scene JSON (no materialAssetId key) must load as zero.
  constexpr const char *kLegacyScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"MeshComponent\":{\"meshAssetId\":7}}}]}";
  std::unique_ptr<engine::runtime::World> legacy(new (std::nothrow)
                                                     engine::runtime::World());
  if (legacy == nullptr) {
    return 142;
  }
  if (!engine::runtime::load_scene(*legacy, kLegacyScene,
                                   std::strlen(kLegacyScene))) {
    return 143;
  }
  engine::runtime::Entity legacyEntity = engine::runtime::kInvalidEntity;
  legacy->for_each_alive(
      [&](engine::runtime::Entity alive) noexcept { legacyEntity = alive; });
  engine::runtime::MeshComponent legacyMesh{};
  if (!legacy->get_mesh_component(legacyEntity, &legacyMesh)) {
    return 144;
  }
  if ((legacyMesh.meshAssetId != 7ULL) ||
      (legacyMesh.materialAssetId != 0ULL)) {
    return 145;
  }

  return 0;
}

/// Verifies every collider shape/local pose plus legacy and invalid JSON.
int verify_collider_scene_round_trip() {
  constexpr std::array<engine::runtime::ColliderShape, 5U> kShapes = {
      engine::runtime::ColliderShape::AABB,
      engine::runtime::ColliderShape::Sphere,
      engine::runtime::ColliderShape::Capsule,
      engine::runtime::ColliderShape::ConvexHull,
      engine::runtime::ColliderShape::Heightfield};
  std::unique_ptr<engine::runtime::World> source(new (std::nothrow)
                                                     engine::runtime::World());
  if (source == nullptr) {
    return 200;
  }

  std::array<engine::runtime::PersistentId, kShapes.size()> ids{};
  for (std::size_t i = 0U; i < kShapes.size(); ++i) {
    const engine::runtime::Entity entity = source->create_scene_object();
    if (entity == engine::runtime::kInvalidEntity) {
      return 201;
    }
    ids[i] = source->persistent_id(entity);
    engine::runtime::Collider collider{};
    collider.shape = kShapes[i];
    collider.localPosition = engine::math::Vec3(static_cast<float>(i + 1U),
                                                -static_cast<float>(i + 2U),
                                                static_cast<float>(i + 3U));
    collider.localRotation = collider_test_rotation(i);
    if (!source->add_collider(entity, collider)) {
      return 202;
    }
  }

  std::array<char, engine::core::JsonWriter::kBufferBytes> buffer{};
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(*source, buffer.data(), buffer.size(),
                                   &size)) {
    return 203;
  }
  std::unique_ptr<engine::runtime::World> loaded(new (std::nothrow)
                                                     engine::runtime::World());
  if ((loaded == nullptr) ||
      !engine::runtime::load_scene(*loaded, buffer.data(), size)) {
    return 204;
  }
  for (std::size_t i = 0U; i < kShapes.size(); ++i) {
    const engine::runtime::Entity entity =
        loaded->find_entity_by_persistent_id(ids[i]);
    engine::runtime::Collider collider{};
    const engine::math::Vec3 expectedPosition(static_cast<float>(i + 1U),
                                              -static_cast<float>(i + 2U),
                                              static_cast<float>(i + 3U));
    if ((entity == engine::runtime::kInvalidEntity) ||
        !loaded->get_collider(entity, &collider) ||
        (collider.shape != kShapes[i]) ||
        !collider_pose_equals(collider, expectedPosition,
                              collider_test_rotation(i))) {
      return 205;
    }
  }

  constexpr const char *kLegacyScene =
      "{\"version\":2,\"entities\":[{\"components\":{\"Collider\":{"
      "\"halfExtents\":[1,2,3]}}}]}";
  std::unique_ptr<engine::runtime::World> legacy(new (std::nothrow)
                                                     engine::runtime::World());
  if ((legacy == nullptr) ||
      !engine::runtime::load_scene(*legacy, kLegacyScene,
                                   std::strlen(kLegacyScene))) {
    return 206;
  }
  engine::runtime::Entity legacyEntity = engine::runtime::kInvalidEntity;
  legacy->for_each_alive(
      [&](engine::runtime::Entity entity) noexcept { legacyEntity = entity; });
  engine::runtime::Collider legacyCollider{};
  if (!legacy->get_collider(legacyEntity, &legacyCollider) ||
      (legacyCollider.shape != engine::runtime::ColliderShape::AABB) ||
      !collider_pose_equals(legacyCollider, engine::math::Vec3(),
                            engine::math::Quat())) {
    return 207;
  }

  constexpr const char *kInvalidScene =
      "{\"version\":2,\"entities\":[{\"components\":{\"Collider\":{\"shape\":5}"
      "}}]}";
  std::unique_ptr<engine::runtime::World> invalid(new (std::nothrow)
                                                      engine::runtime::World());
  if ((invalid == nullptr) ||
      engine::runtime::load_scene(*invalid, kInvalidScene,
                                  std::strlen(kInvalidScene)) ||
      (invalid->alive_entity_count() != 0U)) {
    return 208;
  }
  return 0;
}

/// EXPECTATION: a world carrying every persistent component type survives
/// a save/load round trip with every component present and its marker
/// values intact — the commit copy must never silently drop a type
/// (regression: AnimationComponent was omitted from copy_world_contents).
int check_every_component_type_survives_load() {
  using namespace engine::runtime;

  std::unique_ptr<World> source(new (std::nothrow) World());
  if (source == nullptr) {
    return 300;
  }

  Transform transform{};
  transform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  const Entity entity = source->create_scene_object(transform);
  if (entity == kInvalidEntity) {
    return 301;
  }

  NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "Everything");
  RigidBody body{};
  body.inverseMass = 0.5F;
  Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.25F, 0.5F, 0.75F);
  MeshComponent mesh{};
  mesh.meshAssetId = 4242ULL;
  FoliagePatchComponent foliage{};
  foliage.instanceCount = 1U;
  foliage.instances[0].scale = 0.625F;
  LightComponent light{};
  light.intensity = 2.5F;
  PointLightComponent pointLight{};
  pointLight.radius = 7.0F;
  SpotLightComponent spotLight{};
  spotLight.outerConeAngle = 0.75F;
  ReflectionProbeComponent probe{};
  probe.intensity = 1.25F;
  SceneCaptureComponent capture{};
  capture.width = 128U;
  capture.height = 64U;
  ScriptComponent script{};
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                "assets/scripts/marker.lua");
  SpringArmComponent springArm{};
  springArm.armLength = 4.5F;
  AnimationComponent animation{};
  std::snprintf(animation.controllerPath, sizeof(animation.controllerPath),
                "%s", "assets/character.animctrl.json");
  CameraComponent camera{};
  camera.priority = 6.5F;
  camera.fovRadians = 0.8F;

  if (!source->add_name_component(entity, name) ||
      !source->add_rigid_body(entity, body) ||
      !source->add_collider(entity, collider) ||
      !source->add_mesh_component(entity, mesh) ||
      !source->add_foliage_patch_component(entity, foliage) ||
      !source->add_light_component(entity, light) ||
      !source->add_point_light_component(entity, pointLight) ||
      !source->add_spot_light_component(entity, spotLight) ||
      !source->add_reflection_probe_component(entity, probe) ||
      !source->add_scene_capture_component(entity, capture) ||
      !source->add_script_component(entity, script) ||
      !source->add_spring_arm(entity, springArm) ||
      !source->add_animation_component(entity, animation) ||
      !source->add_camera_component(entity, camera)) {
    return 302;
  }

  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      buffer(new (std::nothrow)
                 std::array<char, engine::core::JsonWriter::kBufferBytes>());
  if (buffer == nullptr) {
    return 303;
  }
  std::size_t size = 0U;
  if (!save_scene(*source, buffer->data(), buffer->size(), &size) ||
      (size == 0U)) {
    return 304;
  }

  std::unique_ptr<World> loaded(new (std::nothrow) World());
  if ((loaded == nullptr) ||
      !load_scene(*loaded, buffer->data(), size)) {
    return 305;
  }
  const Entity found = loaded->find_entity_by_name("Everything");
  if (found == kInvalidEntity) {
    return 306;
  }

  Transform loadedTransform{};
  RigidBody loadedBody{};
  Collider loadedCollider{};
  MeshComponent loadedMesh{};
  FoliagePatchComponent loadedFoliage{};
  LightComponent loadedLight{};
  PointLightComponent loadedPointLight{};
  SpotLightComponent loadedSpotLight{};
  ReflectionProbeComponent loadedProbe{};
  SceneCaptureComponent loadedCapture{};
  ScriptComponent loadedScript{};
  SpringArmComponent loadedSpringArm{};
  AnimationComponent loadedAnimation{};
  CameraComponent loadedCamera{};

  if (!loaded->get_transform(found, &loadedTransform) ||
      (loadedTransform.position.x != 1.0F) ||
      !loaded->get_rigid_body(found, &loadedBody) ||
      (loadedBody.inverseMass != 0.5F) ||
      !loaded->get_collider(found, &loadedCollider) ||
      (loadedCollider.halfExtents.z != 0.75F) ||
      !loaded->get_mesh_component(found, &loadedMesh) ||
      (loadedMesh.meshAssetId != 4242ULL) ||
      !loaded->get_foliage_patch_component(found, &loadedFoliage) ||
      (loadedFoliage.instanceCount != 1U) ||
      (loadedFoliage.instances[0].scale != 0.625F) ||
      !loaded->get_light_component(found, &loadedLight) ||
      (loadedLight.intensity != 2.5F) ||
      !loaded->get_point_light_component(found, &loadedPointLight) ||
      (loadedPointLight.radius != 7.0F) ||
      !loaded->get_spot_light_component(found, &loadedSpotLight) ||
      (loadedSpotLight.outerConeAngle != 0.75F) ||
      !loaded->get_reflection_probe_component(found, &loadedProbe) ||
      (loadedProbe.intensity != 1.25F) ||
      !loaded->get_scene_capture_component(found, &loadedCapture) ||
      (loadedCapture.width != 128U) || (loadedCapture.height != 64U) ||
      !loaded->get_script_component(found, &loadedScript) ||
      (std::strcmp(loadedScript.scriptPath, "assets/scripts/marker.lua") !=
       0) ||
      !loaded->get_spring_arm(found, &loadedSpringArm) ||
      (loadedSpringArm.armLength != 4.5F)) {
    return 307;
  }

  if (!loaded->get_animation_component(found, &loadedAnimation) ||
      (std::strcmp(loadedAnimation.controllerPath,
                   "assets/character.animctrl.json") != 0)) {
    return 308;
  }

  if (!loaded->get_camera_component(found, &loadedCamera) ||
      (loadedCamera.priority != 6.5F) ||
      (loadedCamera.fovRadians != 0.8F)) {
    return 309;
  }

  return 0;
}

/// EXPECTATION: every authored AnimationComponent field survives the scene
/// round trip (issue #253 — `playing` and `playbackSpeed` are Inspector-
/// edited authored state that the bare-string wire shape could not carry,
/// so they silently reverted to defaults on load). Also pins the three
/// properties the shape split rests on: a component still holding the
/// default playing/speed keeps writing the bare string, the bare string is
/// still read as the legacy form, and runtime state is not persisted.
int check_animation_authored_fields_round_trip() {
  using namespace engine::runtime;

  constexpr const char *kControllerPath = "assets/character.animctrl.json";

  std::unique_ptr<World> source(new (std::nothrow) World());
  if (source == nullptr) {
    return 320;
  }

  const Entity entity = source->create_scene_object(Transform{});
  if (entity == kInvalidEntity) {
    return 321;
  }

  NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "Animated");
  AnimationComponent animation{};
  std::snprintf(animation.controllerPath, sizeof(animation.controllerPath),
                "%s", kControllerPath);
  animation.playing = false;
  animation.playbackSpeed = 2.25F;
  // Runtime state: set to non-defaults so the load side can prove the format
  // does not carry it back.
  animation.currentState = 3U;
  animation.stateTime = 9.5F;
  if (!source->add_name_component(entity, name) ||
      !source->add_animation_component(entity, animation)) {
    return 322;
  }

  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      buffer(new (std::nothrow)
                 std::array<char, engine::core::JsonWriter::kBufferBytes>());
  if (buffer == nullptr) {
    return 323;
  }
  std::size_t size = 0U;
  if (!save_scene(*source, buffer->data(), buffer->size(), &size) ||
      (size == 0U)) {
    return 324;
  }

  std::unique_ptr<World> loaded(new (std::nothrow) World());
  if ((loaded == nullptr) || !load_scene(*loaded, buffer->data(), size)) {
    return 325;
  }
  const Entity found = loaded->find_entity_by_name("Animated");
  if (found == kInvalidEntity) {
    return 326;
  }

  AnimationComponent loadedAnimation{};
  if (!loaded->get_animation_component(found, &loadedAnimation)) {
    return 327;
  }
  if (std::strcmp(loadedAnimation.controllerPath, kControllerPath) != 0) {
    return 328;
  }
  // Exact: the authored values are serialized data, not computed floats.
  if (loadedAnimation.playing || (loadedAnimation.playbackSpeed != 2.25F)) {
    return 329;
  }
  const AnimationComponent defaults{};
  if ((loadedAnimation.currentState != defaults.currentState) ||
      (loadedAnimation.stateTime != defaults.stateTime) ||
      (loadedAnimation.controllerSlot != defaults.controllerSlot)) {
    return 330;
  }

  // A component still holding the default playing/speed writes the bare
  // string, so scenes that author neither stay byte-identical to the files
  // this build's predecessors wrote.
  std::unique_ptr<World> defaultSource(new (std::nothrow) World());
  if (defaultSource == nullptr) {
    return 331;
  }
  const Entity defaultEntity = defaultSource->create_scene_object(Transform{});
  if (defaultEntity == kInvalidEntity) {
    return 332;
  }
  AnimationComponent defaultAnimation{};
  std::snprintf(defaultAnimation.controllerPath,
                sizeof(defaultAnimation.controllerPath), "%s", kControllerPath);
  if (!defaultSource->add_animation_component(defaultEntity,
                                              defaultAnimation)) {
    return 333;
  }
  std::size_t defaultSize = 0U;
  if (!save_scene(*defaultSource, buffer->data(), buffer->size(),
                  &defaultSize) ||
      (defaultSize == 0U)) {
    return 334;
  }
  const std::string defaultText(buffer->data(), defaultSize);
  const std::string expectedBareString =
      std::string("\"AnimationComponent\":\"") + kControllerPath + "\"";
  if (defaultText.find(expectedBareString) == std::string::npos) {
    return 335;
  }

  // The bare string remains a legal read, supplying defaults for the fields
  // it cannot express -- this is the migration path for every scene authored
  // before the object shape existed.
  constexpr const char *kLegacyScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"name\":\"Legacy\","
      "\"AnimationComponent\":\"assets/character.animctrl.json\"}}]}";
  std::unique_ptr<World> legacy(new (std::nothrow) World());
  if ((legacy == nullptr) ||
      !load_scene(*legacy, kLegacyScene, std::strlen(kLegacyScene))) {
    return 336;
  }
  const Entity legacyEntity = legacy->find_entity_by_name("Legacy");
  AnimationComponent legacyAnimation{};
  if ((legacyEntity == kInvalidEntity) ||
      !legacy->get_animation_component(legacyEntity, &legacyAnimation)) {
    return 337;
  }
  if ((std::strcmp(legacyAnimation.controllerPath, kControllerPath) != 0) ||
      (legacyAnimation.playing != defaults.playing) ||
      (legacyAnimation.playbackSpeed != defaults.playbackSpeed)) {
    return 338;
  }

  // Strict on the new fields, matching the other object-shaped codecs: a
  // present-but-malformed field fails the load rather than silently keeping
  // the default.
  constexpr const char *kMalformedScene =
      "{\"version\":2,\"entities\":[{\"components\":{"
      "\"AnimationComponent\":{"
      "\"controllerPath\":\"assets/character.animctrl.json\","
      "\"playing\":\"yes\"}}}]}";
  std::unique_ptr<World> malformed(new (std::nothrow) World());
  if (malformed == nullptr) {
    return 339;
  }
  if (load_scene(*malformed, kMalformedScene, std::strlen(kMalformedScene))) {
    return 340;
  }

  // Boundary: playbackSpeed is now written, so a non-finite value reaches the
  // JSON writer, which refuses it. The save fails rather than emitting a
  // token no reader accepts -- the same treatment every other serialized
  // float already gets. (Before the object shape the field was simply
  // dropped, so this path could not arise.)
  std::unique_ptr<World> nonFinite(new (std::nothrow) World());
  if (nonFinite == nullptr) {
    return 341;
  }
  const Entity nonFiniteEntity = nonFinite->create_scene_object(Transform{});
  if (nonFiniteEntity == kInvalidEntity) {
    return 342;
  }
  AnimationComponent nonFiniteAnimation{};
  std::snprintf(nonFiniteAnimation.controllerPath,
                sizeof(nonFiniteAnimation.controllerPath), "%s",
                kControllerPath);
  nonFiniteAnimation.playbackSpeed =
      std::numeric_limits<float>::infinity();
  if (!nonFinite->add_animation_component(nonFiniteEntity,
                                          nonFiniteAnimation)) {
    return 343;
  }
  std::size_t nonFiniteSize = 0U;
  if (save_scene(*nonFinite, buffer->data(), buffer->size(), &nonFiniteSize)) {
    return 344;
  }

  return 0;
}

/// EXPECTATION: non-default world gravity survives a save/load round trip
/// exactly, and a default-gravity scene loads back to the default.
int check_gravity_round_trip() {
  using namespace engine::runtime;

  std::unique_ptr<World> source(new (std::nothrow) World());
  if (source == nullptr) {
    return 320;
  }
  set_gravity(*source, 1.5F, -3.0F, 0.25F);

  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      buffer(new (std::nothrow)
                 std::array<char, engine::core::JsonWriter::kBufferBytes>());
  if (buffer == nullptr) {
    return 321;
  }
  std::size_t size = 0U;
  if (!save_scene(*source, buffer->data(), buffer->size(), &size)) {
    return 322;
  }

  std::unique_ptr<World> loaded(new (std::nothrow) World());
  if ((loaded == nullptr) || !load_scene(*loaded, buffer->data(), size)) {
    return 323;
  }
  float gx = 0.0F;
  float gy = 0.0F;
  float gz = 0.0F;
  if (!get_gravity(*loaded, &gx, &gy, &gz) || (gx != 1.5F) ||
      (gy != -3.0F) || (gz != 0.25F)) {
    return 324;
  }

  std::unique_ptr<World> defaultWorld(new (std::nothrow) World());
  if (defaultWorld == nullptr) {
    return 325;
  }
  size = 0U;
  if (!save_scene(*defaultWorld, buffer->data(), buffer->size(), &size)) {
    return 326;
  }
  std::unique_ptr<World> defaultLoaded(new (std::nothrow) World());
  if ((defaultLoaded == nullptr) ||
      !load_scene(*defaultLoaded, buffer->data(), size)) {
    return 327;
  }
  if (!get_gravity(*defaultLoaded, &gx, &gy, &gz) || (gx != 0.0F) ||
      (gy != -9.8F) || (gz != 0.0F)) {
    return 328;
  }

  return 0;
}

/// No-op callback for arming a timer ahead of a save.
void transient_timer_noop(engine::runtime::TimerId, void *) noexcept {}

/// EXPECTATION (issue #209): timers are runtime-only state. A save of a
/// world with an armed timer emits no "timers" block, and loading a legacy
/// scene that carries one leaves the loaded world's timer manager empty
/// instead of restoring inert timers the canonical load path could never
/// fire (they carried no callback identity and were cleared right after).
int check_timers_are_runtime_only() {
  using namespace engine::runtime;

  std::unique_ptr<World> source(new (std::nothrow) World());
  if (source == nullptr) {
    return 340;
  }
  if (source->timer_manager().set_timeout(1.0F, transient_timer_noop,
                                          nullptr) == kInvalidTimerId) {
    return 341;
  }

  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      buffer(new (std::nothrow)
                 std::array<char, engine::core::JsonWriter::kBufferBytes>());
  if (buffer == nullptr) {
    return 342;
  }
  std::size_t size = 0U;
  if (!save_scene(*source, buffer->data(), buffer->size(), &size)) {
    return 343;
  }
  buffer->at(size < buffer->size() ? size : buffer->size() - 1U) = '\0';
  if (std::strstr(buffer->data(), "\"timers\"") != nullptr) {
    return 344; // the format no longer carries runtime-only timer state
  }

  // A legacy scene with a timers block still loads, but restores nothing.
  const char *legacyScene =
      "{\"version\":1,\"entities\":[],"
      "\"timers\":[{\"id\":1,\"remaining\":0.5,\"interval\":0.5,"
      "\"repeat\":true}]}";
  std::unique_ptr<World> loaded(new (std::nothrow) World());
  if (loaded == nullptr) {
    return 345;
  }
  if (!load_scene(*loaded, legacyScene, std::strlen(legacyScene))) {
    return 346; // legacy blocks must stay loadable, just ignored
  }
  if (loaded->timer_manager().active_count() != 0U) {
    return 347; // nothing restored: no inert, never-firing timers
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
/// Portably opens a file for reading across CRTs.
std::FILE *open_read_file(const char *path) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  return file;
}

/// #208: a save with live state the scene format cannot represent must be
/// refused (not warn-and-succeed), the refusal must be visible through
/// collect_scene_save_blockers, and a refused file save must leave the
/// previous file byte-identical. Covers every joint type, the custom-hull
/// and heightfield payload blockers, the provenance-hull and removed-joint
/// recoveries, and the inactive high-water joint slots left by removal.
int verify_save_refuses_unserializable_state(const char *path) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 400;
  }

  const engine::runtime::Entity first = world->create_scene_object();
  const engine::runtime::Entity second = world->create_scene_object();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 401;
  }

  std::array<char, engine::core::JsonWriter::kBufferBytes> buffer{};
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                   &size)) {
    return 402; // blocker-free world must stay savable
  }
  if (!engine::runtime::save_scene(*world, path)) {
    return 403;
  }
  std::unique_ptr<char[]> baseline{};
  std::size_t baselineSize = 0U;
  {
    std::FILE *file = open_read_file(path);
    if (file == nullptr) {
      return 404;
    }
    static_cast<void>(std::fseek(file, 0, SEEK_END));
    const long fileEnd = std::ftell(file);
    static_cast<void>(std::fseek(file, 0, SEEK_SET));
    if (fileEnd <= 0) {
      static_cast<void>(std::fclose(file));
      return 405;
    }
    baselineSize = static_cast<std::size_t>(fileEnd);
    baseline.reset(new (std::nothrow) char[baselineSize]);
    if ((baseline == nullptr) ||
        (std::fread(baseline.get(), 1U, baselineSize, file) != baselineSize)) {
      static_cast<void>(std::fclose(file));
      return 406;
    }
    static_cast<void>(std::fclose(file));
  }

  // Every joint type blocks the save while active and frees it on removal;
  // removal leaves an inactive high-water slot that must not block.
  const engine::math::Vec3 pivot(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 axis(0.0F, 1.0F, 0.0F);
  const std::array<engine::physics::JointId, 6U> jointIds = {
      engine::runtime::add_distance_joint(*world, first, second, 1.0F),
      engine::runtime::add_hinge_joint(*world, first, second, pivot, axis),
      engine::runtime::add_ball_socket_joint(*world, first, second, pivot),
      engine::runtime::add_slider_joint(*world, first, second, axis),
      engine::runtime::add_spring_joint(*world, first, second, 1.0F, 50.0F,
                                        1.0F),
      engine::runtime::add_fixed_joint(*world, first, second)};
  for (std::size_t i = 0U; i < jointIds.size(); ++i) {
    if (jointIds[i] == engine::physics::kInvalidJointId) {
      return 407;
    }
  }
  if (engine::runtime::collect_scene_save_blockers(*world).activeJoints !=
      jointIds.size()) {
    return 408;
  }
  for (std::size_t i = 0U; i < jointIds.size(); ++i) {
    if (engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                    &size)) {
      return 409; // an active joint of any type must refuse the save
    }
    if (!engine::runtime::remove_joint(*world, jointIds[i])) {
      return 410;
    }
  }
  if (engine::runtime::collect_scene_save_blockers(*world).activeJoints != 0U) {
    return 411;
  }
  if (!engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                   &size)) {
    return 412; // inactive high-water slots must not block
  }

  // A provenance-free custom hull payload blocks; reinstalling the same
  // collider with builder provenance rebuilds the payload and unblocks.
  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_cylinder_hull(&hull)) {
    return 413;
  }
  engine::runtime::Collider hullCollider{};
  hullCollider.shape = engine::runtime::ColliderShape::ConvexHull;
  hullCollider.hullSource = engine::math::HullSource::None;
  if (!world->add_collider(first, hullCollider) ||
      !engine::physics::set_convex_hull_data(world->physics_context(), first,
                                             hull)) {
    return 414;
  }
  if (engine::runtime::collect_scene_save_blockers(*world).customHullPayloads !=
      1U) {
    return 415;
  }
  if (engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                  &size)) {
    return 416; // custom hull payload must refuse the save
  }
  if (engine::runtime::save_scene(*world, path)) {
    return 417; // file-path overload must refuse identically
  }
  {
    // The refused save must leave the previous file byte-identical.
    std::FILE *file = open_read_file(path);
    if (file == nullptr) {
      return 418;
    }
    static_cast<void>(std::fseek(file, 0, SEEK_END));
    const long fileEnd = std::ftell(file);
    static_cast<void>(std::fseek(file, 0, SEEK_SET));
    if ((fileEnd < 0) ||
        (static_cast<std::size_t>(fileEnd) != baselineSize)) {
      static_cast<void>(std::fclose(file));
      return 419;
    }
    std::unique_ptr<char[]> current(new (std::nothrow) char[baselineSize]);
    if ((current == nullptr) ||
        (std::fread(current.get(), 1U, baselineSize, file) != baselineSize)) {
      static_cast<void>(std::fclose(file));
      return 420;
    }
    static_cast<void>(std::fclose(file));
    if (std::memcmp(current.get(), baseline.get(), baselineSize) != 0) {
      return 421;
    }
  }
  if (!world->remove_collider(first)) {
    return 422;
  }
  hullCollider.hullSource = engine::math::HullSource::Cylinder;
  if (!world->add_collider(first, hullCollider)) {
    return 423;
  }
  if (engine::physics::get_convex_hull_data(world->physics_context(), first) ==
      nullptr) {
    return 424; // provenance install must have rebuilt the payload
  }
  if (!engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                   &size)) {
    return 425; // provenance-backed hull payload must stay savable
  }

  // A heightfield payload consumed by a live collider blocks the save.
  engine::runtime::Collider heightfieldCollider{};
  heightfieldCollider.shape = engine::runtime::ColliderShape::Heightfield;
  engine::physics::HeightfieldData heightfield{};
  heightfield.rows = 2U;
  heightfield.columns = 2U;
  if (!world->add_collider(second, heightfieldCollider) ||
      !engine::physics::set_heightfield_data(world->physics_context(), second,
                                             heightfield)) {
    return 426;
  }
  if (engine::runtime::collect_scene_save_blockers(*world)
          .heightfieldPayloads != 1U) {
    return 427;
  }
  if (engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                  &size)) {
    return 428; // heightfield payload must refuse the save
  }
  if (!world->remove_collider(second)) {
    return 429;
  }
  if (!engine::runtime::save_scene(*world, buffer.data(), buffer.size(),
                                   &size)) {
    return 430; // dropping the consuming collider must unblock the save
  }

  return 0;
}

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

  result =
      verify_load_scene_replaces_existing_scene_state(sceneBuffer, sceneSize);
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

  result = verify_over_capacity_authored_data_rejected();
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

  result = verify_camera_parse_failure_rejects_scene();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_point_spot_light_scene_round_trip();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_mesh_material_reference_round_trip();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = verify_collider_scene_round_trip();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = check_every_component_type_survives_load();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = check_animation_authored_fields_round_trip();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  result = check_gravity_round_trip();
  if (result != 0) {
    return result;
  }
  result = check_timers_are_runtime_only();
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  constexpr const char *kBlockerScenePath = "scene_serializer_blocker_tmp.json";
  result = verify_save_refuses_unserializable_state(kBlockerScenePath);
  static_cast<void>(std::remove(kBlockerScenePath));
  if (result != 0) {
    static_cast<void>(std::remove(kScenePath));
    static_cast<void>(std::remove(kLargeScenePath));
    return result;
  }

  static_cast<void>(std::remove(kScenePath));
  static_cast<void>(std::remove(kLargeScenePath));
  return result;
}
