// Implements scene serializer behavior for the Engine runtime world.

#include "engine/runtime/scene_serializer.h"

#include "engine/runtime/animation_system.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/reflect.h"
#include "engine/math/quat.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/serialization_keys.h"
#include "engine/runtime/world.h"
#include "component_registry.h"
#include "serialization_util.h"

namespace engine::runtime {

namespace {

constexpr const char *kSceneLogChannel = "scene";
constexpr const char *kVersionKey = "version";
constexpr std::uint32_t kCurrentSceneVersion = 2U;
constexpr const char *kEntitiesKey = "entities";
constexpr const char *kComponentsKey = "components";
constexpr const char *kPersistentIdKey = "persistentId";
constexpr const char *kNameFieldKey = "name";
constexpr const char *kGravityKey = "gravity";

// File IO and vec/quat/foliage JSON helpers are shared with the prefab
// serializer via serialization_util.h.

bool log_scene_error(const char *message) noexcept {
  if (message != nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel, message);
  }

  return false;
}

bool deserialize_scene_entities(const core::JsonParser &parser,
                                const core::JsonValue &entities,
                                const ReflectedComponentDescriptors &descs,
                                World &targetWorld) noexcept {
  const core::TypeDescriptor &transformDesc = *descs.transform;
  const core::TypeDescriptor &rigidBodyDesc = *descs.rigidBody;
  const core::TypeDescriptor &springArmDesc = *descs.springArm;
  const core::TypeDescriptor &reflectionProbeDesc = *descs.reflectionProbe;
  const core::TypeDescriptor &pointLightDesc = *descs.pointLight;
  const core::TypeDescriptor &spotLightDesc = *descs.spotLight;
  const std::size_t entityCount = parser.array_size(entities);
  for (std::size_t i = 0U; i < entityCount; ++i) {
    core::JsonValue entityValue{};
    if (!parser.get_array_element(entities, i, &entityValue) ||
        (entityValue.type != core::JsonValue::Type::Object)) {
      return log_scene_error("entity entry must be an object");
    }

    PersistentId persistentId = kInvalidPersistentId;
    core::JsonValue persistentIdValue{};
    if (parser.get_object_field(entityValue, kPersistentIdKey,
                                &persistentIdValue)) {
      if (!parser.as_uint(persistentIdValue, &persistentId)) {
        return log_scene_error("persistentId must be a uint");
      }

      if ((persistentId != kInvalidPersistentId) &&
          (targetWorld.find_entity_by_persistent_id(persistentId) !=
           kInvalidEntity)) {
        return log_scene_error("duplicate persistentId in scene");
      }
    }

    const Entity entity =
        (persistentId != kInvalidPersistentId)
            ? targetWorld.create_scene_object_with_persistent_id(persistentId)
            : targetWorld.create_scene_object();
    if (entity == kInvalidEntity) {
      return log_scene_error("failed to allocate entity while loading scene");
    }

    core::JsonValue components{};
    if (!parser.get_object_field(entityValue, kComponentsKey, &components)) {
      continue;
    }

    if (components.type != core::JsonValue::Type::Object) {
      targetWorld.destroy_entity(entity);
      return log_scene_error("components field must be an object");
    }

    core::JsonValue transformValue{};
    if (parser.get_object_field(components, kJsonKeyTransform,
                                &transformValue)) {
      Transform transform{};
      if (!read_reflected_component(parser, transformValue, transformDesc,
                                    &transform) ||
          !targetWorld.add_transform(entity, transform)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load Transform component");
      }
    }

    core::JsonValue rigidBodyValue{};
    if (parser.get_object_field(components, kJsonKeyRigidBody,
                                &rigidBodyValue)) {
      RigidBody rigidBody{};
      if (!read_reflected_component(parser, rigidBodyValue, rigidBodyDesc,
                                    &rigidBody) ||
          !targetWorld.add_rigid_body(entity, rigidBody)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load RigidBody component");
      }
    }

    core::JsonValue colliderValue{};
    if (parser.get_object_field(components, kJsonKeyCollider, &colliderValue)) {
      Collider collider{};
      if (!read_collider_component(parser, colliderValue, &collider) ||
          !targetWorld.add_collider(entity, collider)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load Collider component");
      }
    }

    core::JsonValue meshValue{};
    if (parser.get_object_field(components, kJsonKeyMeshComponent, &meshValue)) {
      MeshComponent mesh{};
      if (!read_mesh_component(parser, meshValue, &mesh) ||
          !targetWorld.add_mesh_component(entity, mesh)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load MeshComponent component");
      }
    }

    core::JsonValue foliageValue{};
    if (parser.get_object_field(components, kJsonKeyFoliagePatchComponent,
                                &foliageValue)) {
      FoliagePatchComponent foliage{};
      if (!read_foliage_patch_component(parser, foliageValue, &foliage) ||
          !targetWorld.add_foliage_patch_component(entity, foliage)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error(
            "failed to load FoliagePatchComponent component");
      }
    }

    core::JsonValue lightValue{};
    if (parser.get_object_field(components, kJsonKeyLightComponent,
                                &lightValue)) {
      LightComponent light{};
      if (!read_light_component(parser, lightValue, &light) ||
          !targetWorld.add_light_component(entity, light)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load LightComponent component");
      }
    }

    core::JsonValue plVal{};
    if (parser.get_object_field(components, kJsonKeyPointLightComponent, &plVal)) {
      PointLightComponent pc{};
      if (!read_reflected_component(parser, plVal, pointLightDesc, &pc) ||
          !targetWorld.add_point_light_component(entity, pc)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load PointLightComponent component");
      }
    }

    core::JsonValue slVal{};
    if (parser.get_object_field(components, kJsonKeySpotLightComponent, &slVal)) {
      SpotLightComponent sc{};
      if (!read_reflected_component(parser, slVal, spotLightDesc, &sc) ||
          !targetWorld.add_spot_light_component(entity, sc)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load SpotLightComponent component");
      }
    }

    core::JsonValue reflectionProbeValue{};
    if (parser.get_object_field(components, kJsonKeyReflectionProbeComponent,
                                &reflectionProbeValue)) {
      ReflectionProbeComponent reflectionProbe{};
      if (!read_reflected_component(parser, reflectionProbeValue,
                                    reflectionProbeDesc, &reflectionProbe) ||
          !targetWorld.add_reflection_probe_component(entity,
                                                      reflectionProbe)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error(
            "failed to load ReflectionProbeComponent component");
      }
    }

    core::JsonValue sceneCaptureValue{};
    if (parser.get_object_field(components, kJsonKeySceneCaptureComponent,
                                &sceneCaptureValue)) {
      SceneCaptureComponent sceneCapture{};
      if (!read_reflected_component(parser, sceneCaptureValue,
                                    *descs.sceneCapture, &sceneCapture) ||
          !targetWorld.add_scene_capture_component(entity, sceneCapture)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error(
            "failed to load SceneCaptureComponent component");
      }
    }

    core::JsonValue nameValue{};
    if (parser.get_object_field(components, kNameFieldKey, &nameValue)) {
      NameComponent nameComponent{};
      if (!parser.copy_string(nameValue, nameComponent.name,
                              sizeof(nameComponent.name))) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to parse name component string");
      }

      if (!targetWorld.add_name_component(entity, nameComponent)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load NameComponent");
      }
    }

    core::JsonValue scriptValue{};
    if (parser.get_object_field(components, kJsonKeyScriptComponent,
                                &scriptValue)) {
      ScriptComponent scriptComp{};
      if (!parser.copy_string_strict(scriptValue, scriptComp.scriptPath,
                                     sizeof(scriptComp.scriptPath))) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to parse ScriptComponent path");
      }

      if (!targetWorld.add_script_component(entity, scriptComp)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load ScriptComponent");
      }
    }

    core::JsonValue animationValue{};
    if (parser.get_object_field(components, kJsonKeyAnimationComponent,
                                &animationValue)) {
      AnimationComponent animationComp{};
      if (!parser.copy_string_strict(animationValue,
                                     animationComp.controllerPath,
                                     sizeof(animationComp.controllerPath))) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to parse AnimationComponent path");
      }

      if (!targetWorld.add_animation_component(entity, animationComp)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load AnimationComponent");
      }
    }

    core::JsonValue springArmValue{};
    if (parser.get_object_field(components, kJsonKeySpringArmComponent,
                                &springArmValue)) {
      SpringArmComponent springArm{};
      if (!read_reflected_component(parser, springArmValue, springArmDesc,
                                    &springArm) ||
          !targetWorld.add_spring_arm(entity, springArm)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load SpringArmComponent");
      }
    }
  }

  return true;
}

/// Counts alive entities carrying the component through the public get
/// accessor, so commit invariants can compare worlds without new World API.
template <typename Component>
std::size_t count_components(
    const World &world,
    bool (World::*getComponent)(Entity, Component *) const noexcept) noexcept {
  std::size_t count = 0U;
  world.for_each_alive([&](Entity entity) noexcept {
    Component component{};
    if ((world.*getComponent)(entity, &component)) {
      ++count;
    }
  });
  return count;
}

/// True when source and target agree on the per-type component count for
/// every persistent-component registry row; a mismatch means the commit copy
/// lost or invented data. Expanded from ENGINE_PERSISTENT_COMPONENT_TABLE so
/// a new component type cannot be silently missed here.
bool world_component_counts_match(const World &sourceWorld,
                                  const World &targetWorld) noexcept {
  const auto matches = [&](auto getComponent) noexcept {
    return count_components(sourceWorld, getComponent) ==
           count_components(targetWorld, getComponent);
  };
  bool countsMatch = true;
#define ENGINE_PCR_COUNT_MATCH(Type, Key, GetFn, AddFn)                        \
  countsMatch = countsMatch && matches(&World::GetFn);
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_COUNT_MATCH)
#undef ENGINE_PCR_COUNT_MATCH
  return countsMatch;
}

/// Copies one component type between worlds through the World get/add pair.
/// Returns true when the source entity has no such component or the copy
/// succeeded; false only when a present component fails to add.
template <typename Component>
bool copy_component(
    const World &sourceWorld, World &targetWorld, Entity sourceEntity,
    Entity targetEntity,
    bool (World::*getComponent)(Entity, Component *) const noexcept,
    bool (World::*addComponent)(Entity, const Component &) noexcept) noexcept {
  Component component{};
  if ((sourceWorld.*getComponent)(sourceEntity, &component) &&
      !(targetWorld.*addComponent)(targetEntity, component)) {
    return false;
  }
  return true;
}

bool copy_world_contents(const World &sourceWorld,
                         World &targetWorld) noexcept {
  bool success = true;

  sourceWorld.for_each_alive([&](Entity sourceEntity) noexcept {
    if (!success) {
      return;
    }

    const PersistentId persistentId = sourceWorld.persistent_id(sourceEntity);
    const Entity targetEntity =
        targetWorld.create_entity_with_persistent_id(persistentId);
    if (targetEntity == kInvalidEntity) {
      success = false;
      return;
    }

    // One registry row per copyable component type; copy_component supplies
    // the guard/copy body once and ENGINE_PERSISTENT_COMPONENT_TABLE keeps
    // the row set complete by construction.
    const auto copy = [&](auto getComponent, auto addComponent) noexcept {
      return copy_component(sourceWorld, targetWorld, sourceEntity,
                            targetEntity, getComponent, addComponent);
    };
    success = true;
#define ENGINE_PCR_COPY_COMPONENT(Type, Key, GetFn, AddFn)                     \
  success = success && copy(&World::GetFn, &World::AddFn);
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_COPY_COMPONENT)
#undef ENGINE_PCR_COPY_COMPONENT
  });

  // Copy timer timing metadata (callbacks must be re-wired by caller).
  if (success) {
    TimerManager::TimerSnapshot snaps[TimerManager::kMaxTimers]{};
    const std::size_t count =
        sourceWorld.timer_manager().snapshot(snaps, TimerManager::kMaxTimers);
    if (count > 0U) {
      targetWorld.timer_manager().restore(snaps, count);
    }
  }

  // World gravity rides the commit copy like the components do.
  if (success) {
    math::Vec3 gravity(0.0F, -9.8F, 0.0F);
    if (get_gravity(sourceWorld, &gravity.x, &gravity.y, &gravity.z)) {
      set_gravity(targetWorld, gravity.x, gravity.y, gravity.z);
    }
  }

  return success;
}

bool serialize_scene_to_writer(const World &world,
                               core::JsonWriter *outWriter) noexcept {
  if (outWriter == nullptr) {
    return false;
  }

  if (world.current_phase() != WorldPhase::Input) {
    core::log_message(core::LogLevel::Warning, kSceneLogChannel,
                      "save_scene requires world Idle phase");
    return false;
  }

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kSceneLogChannel)) {
    return false;
  }
  const core::TypeDescriptor *transformDesc = descs.transform;
  const core::TypeDescriptor *rigidBodyDesc = descs.rigidBody;
  const core::TypeDescriptor *springArmDesc = descs.springArm;
  const core::TypeDescriptor *reflectionProbeDesc = descs.reflectionProbe;

  core::JsonWriter &writer = *outWriter;
  writer.reset();
  writer.begin_object();
  writer.write_uint(kVersionKey, kCurrentSceneVersion);

  // World gravity is authored state; written only when it differs from the
  // default so existing scenes stay byte-identical.
  math::Vec3 gravity(0.0F, -9.8F, 0.0F);
  static_cast<void>(get_gravity(world, &gravity.x, &gravity.y, &gravity.z));
  if ((gravity.x != 0.0F) || (gravity.y != -9.8F) || (gravity.z != 0.0F)) {
    write_vec3(writer, kGravityKey, gravity);
  }

  writer.begin_array(kEntitiesKey);

  std::size_t unserializedHullCount = 0U;
  std::size_t unserializedHeightfieldCount = 0U;
  bool writeFailed = false;
  world.for_each_alive([&](Entity entity) {
    if (writeFailed) {
      return;
    }

    writer.begin_object();
    writer.write_uint(kPersistentIdKey, world.persistent_id(entity));
    writer.write_uint("index", entity.index);
    writer.write_uint("generation", entity.generation);

    writer.write_key(kComponentsKey);
    writer.begin_object();

    Transform transform{};
    if (world.get_transform(entity, &transform)) {
      if (!write_reflected_component(writer, kJsonKeyTransform, *transformDesc,
                                     &transform)) {
        writeFailed = true;
        return;
      }
    }

    RigidBody rigidBody{};
    if (world.get_rigid_body(entity, &rigidBody) &&
        !write_reflected_component(writer, kJsonKeyRigidBody, *rigidBodyDesc,
                                   &rigidBody)) {
      writeFailed = true;
      return;
    }

    Collider collider{};
    if (world.get_collider(entity, &collider)) {
      if (!write_collider_component(writer, collider)) {
        writeFailed = true;
        return;
      }
      if ((collider.shape == ColliderShape::ConvexHull) &&
          (collider.hullSource == math::HullSource::None)) {
        ++unserializedHullCount;
      }
      if (collider.shape == ColliderShape::Heightfield) {
        ++unserializedHeightfieldCount;
      }
    }

    MeshComponent mesh{};
    if (world.get_mesh_component(entity, &mesh)) {
      write_mesh_component(writer, mesh);
    }

    FoliagePatchComponent foliage{};
    if (world.get_foliage_patch_component(entity, &foliage)) {
      write_foliage_patch_component(writer, foliage);
    }

    LightComponent light{};
    if (world.get_light_component(entity, &light)) {
      write_light_component(writer, light);
    }

    PointLightComponent pointLight{};
    if (world.get_point_light_component(entity, &pointLight) &&
        !write_reflected_component(writer, kJsonKeyPointLightComponent,
                                   *descs.pointLight, &pointLight)) {
      writeFailed = true;
      return;
    }

    SpotLightComponent spotLight{};
    if (world.get_spot_light_component(entity, &spotLight) &&
        !write_reflected_component(writer, kJsonKeySpotLightComponent,
                                   *descs.spotLight, &spotLight)) {
      writeFailed = true;
      return;
    }

    ReflectionProbeComponent reflectionProbe{};
    if (world.get_reflection_probe_component(entity, &reflectionProbe) &&
        !write_reflected_component(writer, kJsonKeyReflectionProbeComponent,
                                   *reflectionProbeDesc, &reflectionProbe)) {
      writeFailed = true;
      return;
    }

    SceneCaptureComponent sceneCapture{};
    if (world.get_scene_capture_component(entity, &sceneCapture) &&
        !write_reflected_component(writer, kJsonKeySceneCaptureComponent,
                                   *descs.sceneCapture, &sceneCapture)) {
      writeFailed = true;
      return;
    }

    NameComponent name{};
    if (world.get_name_component(entity, &name)) {
      writer.write_string(kNameFieldKey, name.name);
    }

    ScriptComponent script{};
    if (world.get_script_component(entity, &script) &&
        (script.scriptPath[0] != '\0')) {
      writer.write_string(kJsonKeyScriptComponent, script.scriptPath);
    }

    AnimationComponent animation{};
    if (world.get_animation_component(entity, &animation) &&
        (animation.controllerPath[0] != '\0')) {
      writer.write_string(kJsonKeyAnimationComponent,
                          animation.controllerPath);
    }

    SpringArmComponent springArm{};
    if (world.get_spring_arm(entity, &springArm) &&
        !write_reflected_component(writer, kJsonKeySpringArmComponent, *springArmDesc,
                                   &springArm)) {
      writeFailed = true;
      return;
    }

    writer.end_object();
    writer.end_object();
    writeFailed = writer.failed();
  });

  writer.end_array();

  // Serialize active timers (timing metadata only; callbacks must be re-wired).
  {
    TimerManager::TimerSnapshot snaps[TimerManager::kMaxTimers]{};
    const std::size_t timerCount =
        world.timer_manager().snapshot(snaps, TimerManager::kMaxTimers);
    if (timerCount > 0U) {
      writer.begin_array("timers");
      for (std::size_t i = 0U; i < timerCount; ++i) {
        if (!snaps[i].active) {
          continue;
        }
        writer.begin_object();
        writer.write_uint("id", snaps[i].timerId);
        writer.write_float("remaining", snaps[i].remainingSeconds);
        writer.write_float("interval", snaps[i].intervalSeconds);
        writer.write_bool("repeat", snaps[i].repeat);
        writer.end_object();
      }
      writer.end_array();
    }
  }

  writer.end_object();

  if (writeFailed || !writer.ok()) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to build scene JSON");
    return false;
  }

  // Unsupported-state visibility (audit H-01): these carry runtime-only
  // payloads the format cannot reproduce, so their loss is announced with
  // precise counts instead of happening silently.
  if (unserializedHullCount > 0U) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "%zu convex-hull payload(s) without builder provenance "
                  "are not serialized",
                  unserializedHullCount);
    core::log_message(core::LogLevel::Warning, kSceneLogChannel, message);
  }
  if (unserializedHeightfieldCount > 0U) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "%zu heightfield payload(s) are not serialized",
                  unserializedHeightfieldCount);
    core::log_message(core::LogLevel::Warning, kSceneLogChannel, message);
  }
  // jointCount is a slot high-water mark; removed joints leave inactive
  // slots behind, so only active joints are worth warning about.
  const auto &physicsContext = world.physics_context();
  std::size_t activeJointCount = 0U;
  if (physicsContext.shapeStore != nullptr) {
    const auto &joints = physicsContext.shapeStore->joints;
    for (std::size_t i = 0U; i < physicsContext.jointCount; ++i) {
      if (joints[i].active) {
        ++activeJointCount;
      }
    }
  }
  if (activeJointCount > 0U) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "%zu physics joint(s) are not serialized", activeJointCount);
    core::log_message(core::LogLevel::Warning, kSceneLogChannel, message);
  }

  return true;
}

} // namespace

/// Resets this object back to its reusable empty state for world.
/// Declared reset order (audit H-18, extended by #198): beforeTeardown
/// first — while the outgoing entities and their script modules are still
/// alive, this is where process_pending_scene_op dispatches on_end_play —
/// then entities, the phase-independent destructive teardown, so component
/// removal releases its physics/camera bookkeeping while those managers
/// still exist, then timers, cameras, game mode, the content epoch, and
/// last the animation controller registry, which must only reset once no
/// component can still hold a controllerSlot into it.
void reset_world(World &world, SceneTeardownHook beforeTeardown) noexcept {
  if (beforeTeardown != nullptr) {
    beforeTeardown();
  }
  world.reset_all_entities();
  if (world.alive_entity_count() != 0U) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "reset_world left surviving entities");
  }

  world.timer_manager().clear();
  world.camera_manager().clear();
  world.game_mode().reset();
  world.mark_content_replaced(world.content_epoch());
  reset_anim_controllers();
}

/// Saves the requested resource for scene.
bool save_scene(const World &world, const char *path) noexcept {
  if (path == nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "save_scene called with null path");
    return false;
  }

  core::JsonWriter writer{};
  if (!serialize_scene_to_writer(world, &writer)) {
    return false;
  }

  if (!write_text_file(path, writer.result(), writer.result_size())) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to write scene file");
    return false;
  }

  return true;
}

/// Saves the requested resource for scene.
bool save_scene(const World &world, char *buffer, std::size_t capacity,
                std::size_t *outSize) noexcept {
  if ((buffer == nullptr) || (outSize == nullptr) || (capacity < 2U)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "save_scene called with invalid output buffer");
    return false;
  }

  core::JsonWriter writer{};
  if (!serialize_scene_to_writer(world, &writer)) {
    return false;
  }

  const std::size_t resultSize = writer.result_size();
  if ((resultSize + 1U) > capacity) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "output scene buffer capacity is too small");
    return false;
  }

  std::memcpy(buffer, writer.result(), resultSize);
  buffer[resultSize] = '\0';
  *outSize = resultSize;
  return true;
}

/// Loads the requested resource for scene.
bool load_scene(World &world, const char *path,
                SceneTeardownHook beforeTeardown) noexcept {
  if (path == nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "load_scene called with null path");
    return false;
  }

  std::size_t fileSize = 0U;
  std::unique_ptr<char[]> fileBuffer{};
  if (!read_text_file(path, &fileBuffer, &fileSize)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to read scene file");
    return false;
  }

  return load_scene(world, fileBuffer.get(), fileSize, beforeTeardown);
}

/// Loads the requested resource for scene. Declared reset order (#198):
/// every validation and staging step below runs on a scratch World and
/// never touches the caller's world, so a failure returns false with the
/// outgoing scene completely untouched and beforeTeardown never called;
/// only once the replacement content is fully staged and invariant-checked
/// does beforeTeardown fire (outgoing entities and modules still alive)
/// immediately before the destructive `world = *committedWorld` commit.
bool load_scene(World &world, const char *buffer, std::size_t size,
                SceneTeardownHook beforeTeardown) noexcept {
  if ((buffer == nullptr) || (size == 0U)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "load_scene called with invalid input buffer");
    return false;
  }

  if (world.current_phase() != WorldPhase::Input) {
    core::log_message(core::LogLevel::Warning, kSceneLogChannel,
                      "load_scene requires world Idle phase");
    return false;
  }

  core::JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "malformed scene JSON");
    return false;
  }

  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "scene root must be an object");
    return false;
  }

  std::uint32_t sceneVersion = 1U;
  core::JsonValue versionValue{};
  if (parser.get_object_field(*root, kVersionKey, &versionValue)) {
    if (!parser.as_uint(versionValue, &sceneVersion)) {
      core::log_message(core::LogLevel::Error, kSceneLogChannel,
                        "scene version must be an unsigned integer");
      return false;
    }
  }

  if ((sceneVersion == 0U) || (sceneVersion > kCurrentSceneVersion)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "unsupported scene version");
    return false;
  }

  core::JsonValue entities{};
  if (!parser.get_object_field(*root, kEntitiesKey, &entities) ||
      (entities.type != core::JsonValue::Type::Array)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "scene JSON must include entities array");
    return false;
  }

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kSceneLogChannel)) {
    return false;
  }

  std::unique_ptr<World> stagedWorld(new (std::nothrow) World());
  if (stagedWorld == nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to allocate staged world for scene load");
    return false;
  }

  if (!deserialize_scene_entities(parser, entities, descs, *stagedWorld)) {
    return false;
  }

  // World gravity: optional root field, default when absent.
  core::JsonValue gravityValue{};
  if (parser.get_object_field(*root, kGravityKey, &gravityValue)) {
    math::Vec3 gravity{};
    if (!read_vec3(parser, gravityValue, &gravity)) {
      return log_scene_error("invalid gravity field");
    }
    set_gravity(*stagedWorld, gravity.x, gravity.y, gravity.z);
  }

  // Restore timers from scene JSON (timing metadata only).
  core::JsonValue timersArray{};
  if (parser.get_object_field(*root, "timers", &timersArray) &&
      (timersArray.type == core::JsonValue::Type::Array)) {
    const std::size_t timerCount = parser.array_size(timersArray);
    for (std::size_t i = 0U; i < timerCount; ++i) {
      core::JsonValue timerVal{};
      if (!parser.get_array_element(timersArray, i, &timerVal) ||
          (timerVal.type != core::JsonValue::Type::Object)) {
        continue;
      }

      TimerManager::TimerSnapshot snap{};
      snap.active = true;

      core::JsonValue idVal{};
      if (parser.get_object_field(timerVal, "id", &idVal)) {
        std::uint32_t timerId = 0U;
        if (parser.as_uint(idVal, &timerId)) {
          snap.timerId = static_cast<TimerId>(timerId);
        }
      }
      // A rejected numeric token must not fall back to the zero default:
      // remaining 0 fires immediately and interval 0 with repeat spins, so
      // an unparsable field drops the timer instead.
      bool timerFieldsValid = true;
      core::JsonValue remainVal{};
      if (parser.get_object_field(timerVal, "remaining", &remainVal)) {
        timerFieldsValid = parser.as_float(remainVal, &snap.remainingSeconds);
      }
      core::JsonValue intervalVal{};
      if (timerFieldsValid &&
          parser.get_object_field(timerVal, "interval", &intervalVal)) {
        timerFieldsValid = parser.as_float(intervalVal, &snap.intervalSeconds);
      }
      core::JsonValue repeatVal{};
      if (timerFieldsValid &&
          parser.get_object_field(timerVal, "repeat", &repeatVal)) {
        timerFieldsValid = parser.as_bool(repeatVal, &snap.repeat);
      }
      if (!timerFieldsValid) {
        core::log_message(core::LogLevel::Warning, kSceneLogChannel,
                          "scene load dropped a timer with unparsable timing");
        continue;
      }

      static_cast<void>(stagedWorld->timer_manager().restore(&snap, 1U));
    }
  }

  std::unique_ptr<World> committedWorld(new (std::nothrow) World());
  if (committedWorld == nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to allocate committed world for scene load");
    return false;
  }

  if (!copy_world_contents(*stagedWorld, *committedWorld)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to commit loaded scene");
    return false;
  }

  if ((committedWorld->alive_entity_count() !=
       stagedWorld->alive_entity_count()) ||
      !world_component_counts_match(*stagedWorld, *committedWorld)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "scene commit invariant mismatch after copy");
    return false;
  }

  const std::uint32_t previousEpoch = world.content_epoch();

  // Staging fully succeeded and passed its invariant checks, so the
  // transition is now guaranteed to commit: dispatch on_end_play to the
  // still-alive outgoing world before it is overwritten (#198).
  if (beforeTeardown != nullptr) {
    beforeTeardown();
  }

  world = *committedWorld;
  world.mark_content_replaced(previousEpoch);
  // The replaced world's components are gone, so their cached animation
  // controllers are released; the loaded scene's components re-acquire
  // lazily on the next animation update (controllerSlot is runtime state
  // and never serialized).
  reset_anim_controllers();
  return true;
}

} // namespace engine::runtime
