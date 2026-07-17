// Implements scene serializer behavior for the Engine runtime world.

#include "engine/runtime/scene_serializer.h"

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
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/serialization_keys.h"
#include "engine/runtime/world.h"
#include "serialization_util.h"

namespace engine::runtime {

namespace {

constexpr const char *kSceneLogChannel = "scene";
constexpr const char *kVersionKey = "version";
constexpr std::uint32_t kCurrentSceneVersion = 2U;
constexpr const char *kEntitiesKey = "entities";
constexpr const char *kComponentsKey = "components";
constexpr const char *kPersistentIdKey = "persistentId";
constexpr const char *kTransformTypeName = "engine::runtime::Transform";
constexpr const char *kRigidBodyTypeName = "engine::runtime::RigidBody";
constexpr const char *kColliderTypeName = "engine::runtime::Collider";
constexpr const char *kSpringArmTypeName =
    "engine::runtime::SpringArmComponent";
constexpr const char *kReflectionProbeTypeName =
    "engine::runtime::ReflectionProbeComponent";
constexpr const char *kNameFieldKey = "name";
constexpr const char *kMeshAssetIdKey = "meshAssetId";

// File IO and vec/quat/foliage JSON helpers are shared with the prefab
// serializer via serialization_util.h.

/// Writes reflected component data.
bool write_reflected_component(core::JsonWriter &writer,
                               const char *componentName,
                               const core::TypeDescriptor &descriptor,
                               const void *instance) noexcept {
  if ((componentName == nullptr) || (instance == nullptr)) {
    return false;
  }

  writer.write_key(componentName);
  writer.begin_object();

  for (std::size_t i = 0U; i < descriptor.fieldCount; ++i) {
    const core::TypeField &field = descriptor.fields[i];
    if (field.name == nullptr) {
      continue;
    }

    switch (field.kind) {
    case core::TypeField::Kind::Float: {
      const float *value = descriptor.field_ptr<float>(instance, field);
      if (value == nullptr) {
        return false;
      }

      writer.write_float(field.name, *value);
      break;
    }
    case core::TypeField::Kind::Uint32: {
      const std::uint32_t *value =
          descriptor.field_ptr<std::uint32_t>(instance, field);
      if (value == nullptr) {
        return false;
      }

      writer.write_uint(field.name, *value);
      break;
    }
    case core::TypeField::Kind::Bool: {
      const bool *value = descriptor.field_ptr<bool>(instance, field);
      if (value == nullptr) {
        return false;
      }

      writer.write_bool(field.name, *value);
      break;
    }
    case core::TypeField::Kind::Vec2: {
      const math::Vec2 *value =
          descriptor.field_ptr<math::Vec2>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_vec2(writer, field.name, *value);
      break;
    }
    case core::TypeField::Kind::Vec3: {
      const math::Vec3 *value =
          descriptor.field_ptr<math::Vec3>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_vec3(writer, field.name, *value);
      break;
    }
    case core::TypeField::Kind::Vec4: {
      const math::Vec4 *value =
          descriptor.field_ptr<math::Vec4>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_vec4(writer, field.name, *value);
      break;
    }
    case core::TypeField::Kind::Quat: {
      const math::Quat *value =
          descriptor.field_ptr<math::Quat>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_quat(writer, field.name, *value);
      break;
    }
    case core::TypeField::Kind::Int32:
      // Current scene components do not contain signed integer fields.
      return false;
    }

    if (writer.failed()) {
      return false;
    }
  }

  writer.end_object();
  return !writer.failed();
}

/// Reads reflected component data.
bool read_reflected_component(const core::JsonParser &parser,
                              const core::JsonValue &componentObject,
                              const core::TypeDescriptor &descriptor,
                              void *instance) noexcept {
  if ((instance == nullptr) ||
      (componentObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  for (std::size_t i = 0U; i < descriptor.fieldCount; ++i) {
    const core::TypeField &field = descriptor.fields[i];
    if (field.name == nullptr) {
      continue;
    }

    core::JsonValue fieldValue{};
    if (!parser.get_object_field(componentObject, field.name, &fieldValue)) {
      continue;
    }

    switch (field.kind) {
    case core::TypeField::Kind::Float: {
      float *value = descriptor.field_ptr<float>(instance, field);
      if ((value == nullptr) || !parser.as_float(fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Uint32: {
      std::uint32_t *value =
          descriptor.field_ptr<std::uint32_t>(instance, field);
      if ((value == nullptr) || !parser.as_uint(fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Bool: {
      bool *value = descriptor.field_ptr<bool>(instance, field);
      if ((value == nullptr) || !parser.as_bool(fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Vec2: {
      math::Vec2 *value = descriptor.field_ptr<math::Vec2>(instance, field);
      if ((value == nullptr) || !read_vec2(parser, fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Vec3: {
      math::Vec3 *value = descriptor.field_ptr<math::Vec3>(instance, field);
      if ((value == nullptr) || !read_vec3(parser, fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Vec4: {
      math::Vec4 *value = descriptor.field_ptr<math::Vec4>(instance, field);
      if ((value == nullptr) || !read_vec4(parser, fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Quat: {
      math::Quat *value = descriptor.field_ptr<math::Quat>(instance, field);
      if ((value == nullptr) || !read_quat(parser, fieldValue, value)) {
        return false;
      }
      break;
    }
    case core::TypeField::Kind::Int32:
      // Current scene components do not contain signed integer fields.
      return false;
    }
  }

  return true;
}

/// Reads mesh component data.
bool read_mesh_component(const core::JsonParser &parser,
                         const core::JsonValue &meshObject,
                         MeshComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (meshObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  MeshComponent component{};

  core::JsonValue meshIdValue{};
  if (parser.get_object_field(meshObject, kMeshAssetIdKey, &meshIdValue)) {
    if (!parser.as_uint64(meshIdValue, &component.meshAssetId)) {
      return false;
    }
  } else if (parser.get_object_field(meshObject, "meshId", &meshIdValue)) {
    // Backward-compatible read path for scenes authored before asset IDs.
    if (!parser.as_uint64(meshIdValue, &component.meshAssetId)) {
      return false;
    }
  }

  core::JsonValue albedoValue{};
  if (parser.get_object_field(meshObject, "albedo", &albedoValue)) {
    if (!read_vec3(parser, albedoValue, &component.albedo)) {
      return false;
    }
  }

  core::JsonValue roughnessValue{};
  if (parser.get_object_field(meshObject, "roughness", &roughnessValue)) {
    static_cast<void>(parser.as_float(roughnessValue, &component.roughness));
  }

  core::JsonValue metallicValue{};
  if (parser.get_object_field(meshObject, "metallic", &metallicValue)) {
    static_cast<void>(parser.as_float(metallicValue, &component.metallic));
  }

  core::JsonValue opacityValue{};
  if (parser.get_object_field(meshObject, "opacity", &opacityValue)) {
    static_cast<void>(parser.as_float(opacityValue, &component.opacity));
  }

  *outComponent = component;
  return true;
}

/// Reads light component data.
bool read_light_component(const core::JsonParser &parser,
                          const core::JsonValue &lightObject,
                          LightComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (lightObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  LightComponent component{};

  core::JsonValue colorValue{};
  if (parser.get_object_field(lightObject, "color", &colorValue)) {
    if (!read_vec3(parser, colorValue, &component.color)) {
      return false;
    }
  }

  core::JsonValue dirValue{};
  if (parser.get_object_field(lightObject, "direction", &dirValue)) {
    if (!read_vec3(parser, dirValue, &component.direction)) {
      return false;
    }
  }

  core::JsonValue intensityValue{};
  if (parser.get_object_field(lightObject, "intensity", &intensityValue)) {
    static_cast<void>(parser.as_float(intensityValue, &component.intensity));
  }

  core::JsonValue typeValue{};
  std::uint32_t type = static_cast<std::uint32_t>(LightType::Directional);
  if (parser.get_object_field(lightObject, "type", &typeValue)) {
    if (!parser.as_uint(typeValue, &type)) {
      return false;
    }
  }
  component.type = (type == static_cast<std::uint32_t>(LightType::Point))
                       ? LightType::Point
                       : LightType::Directional;

  *outComponent = component;
  return true;
}

bool read_point_light_component(const core::JsonParser &parser,
                                const core::JsonValue &lightObject,
                                PointLightComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (lightObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  PointLightComponent component{};
  core::JsonValue value{};
  if (parser.get_object_field(lightObject, "color", &value) &&
      !read_vec3(parser, value, &component.color)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "intensity", &value) &&
      !parser.as_float(value, &component.intensity)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "radius", &value) &&
      !parser.as_float(value, &component.radius)) {
    return false;
  }

  *outComponent = component;
  return true;
}

bool read_spot_light_component(const core::JsonParser &parser,
                               const core::JsonValue &lightObject,
                               SpotLightComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (lightObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  SpotLightComponent component{};
  core::JsonValue value{};
  if (parser.get_object_field(lightObject, "color", &value) &&
      !read_vec3(parser, value, &component.color)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "direction", &value) &&
      !read_vec3(parser, value, &component.direction)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "intensity", &value) &&
      !parser.as_float(value, &component.intensity)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "radius", &value) &&
      !parser.as_float(value, &component.radius)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "innerConeAngle", &value) &&
      !parser.as_float(value, &component.innerConeAngle)) {
    return false;
  }
  if (parser.get_object_field(lightObject, "outerConeAngle", &value) &&
      !parser.as_float(value, &component.outerConeAngle)) {
    return false;
  }

  *outComponent = component;
  return true;
}

/// Handles log scene error.
bool log_scene_error(const char *message) noexcept {
  if (message != nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel, message);
  }

  return false;
}

/// Handles deserialize scene entities.
bool deserialize_scene_entities(const core::JsonParser &parser,
                                const core::JsonValue &entities,
                                const core::TypeDescriptor &transformDesc,
                                const core::TypeDescriptor &rigidBodyDesc,
                                const core::TypeDescriptor &colliderDesc,
                                const core::TypeDescriptor &springArmDesc,
                                const core::TypeDescriptor &reflectionProbeDesc,
                                World &targetWorld) noexcept {
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
            ? targetWorld.create_entity_with_persistent_id(persistentId)
            : targetWorld.create_entity();
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
    if (parser.get_object_field(components, kJsonKeyTransform, &transformValue)) {
      Transform transform{};
      if (!read_reflected_component(parser, transformValue, transformDesc,
                                    &transform) ||
          !targetWorld.add_transform(entity, transform)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load Transform component");
      }
    }

    core::JsonValue rigidBodyValue{};
    if (parser.get_object_field(components, kJsonKeyRigidBody, &rigidBodyValue)) {
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
      if (!read_reflected_component(parser, colliderValue, colliderDesc,
                                    &collider) ||
          !targetWorld.add_collider(entity, collider)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load Collider component");
      }
    }

    core::JsonValue meshValue{};
    if (parser.get_object_field(components, "MeshComponent", &meshValue)) {
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
    if (parser.get_object_field(components, kJsonKeyLightComponent, &lightValue)) {
      LightComponent light{};
      if (!read_light_component(parser, lightValue, &light) ||
          !targetWorld.add_light_component(entity, light)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load LightComponent component");
      }
    }

    // PointLightComponent
    core::JsonValue plVal{};
    if (parser.get_object_field(components, "PointLightComponent", &plVal)) {
      PointLightComponent pc{};
      if (!read_point_light_component(parser, plVal, &pc) ||
          !targetWorld.add_point_light_component(entity, pc)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load PointLightComponent component");
      }
    }

    // SpotLightComponent
    core::JsonValue slVal{};
    if (parser.get_object_field(components, "SpotLightComponent", &slVal)) {
      SpotLightComponent sc{};
      if (!read_spot_light_component(parser, slVal, &sc) ||
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
      if (!parser.copy_string(scriptValue, scriptComp.scriptPath,
                              sizeof(scriptComp.scriptPath))) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to parse ScriptComponent path");
      }

      if (!targetWorld.add_script_component(entity, scriptComp)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load ScriptComponent");
      }
    }

    core::JsonValue springArmValue{};
    if (parser.get_object_field(components, "SpringArmComponent",
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

/// Handles copy world contents.
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

    Transform transform{};
    if (sourceWorld.get_transform(sourceEntity, &transform) &&
        !targetWorld.add_transform(targetEntity, transform)) {
      success = false;
      return;
    }

    RigidBody rigidBody{};
    if (sourceWorld.get_rigid_body(sourceEntity, &rigidBody) &&
        !targetWorld.add_rigid_body(targetEntity, rigidBody)) {
      success = false;
      return;
    }

    Collider collider{};
    if (sourceWorld.get_collider(sourceEntity, &collider) &&
        !targetWorld.add_collider(targetEntity, collider)) {
      success = false;
      return;
    }

    MeshComponent mesh{};
    if (sourceWorld.get_mesh_component(sourceEntity, &mesh) &&
        !targetWorld.add_mesh_component(targetEntity, mesh)) {
      success = false;
      return;
    }

    FoliagePatchComponent foliage{};
    if (sourceWorld.get_foliage_patch_component(sourceEntity, &foliage) &&
        !targetWorld.add_foliage_patch_component(targetEntity, foliage)) {
      success = false;
      return;
    }

    LightComponent light{};
    if (sourceWorld.get_light_component(sourceEntity, &light) &&
        !targetWorld.add_light_component(targetEntity, light)) {
      success = false;
      return;
    }

    PointLightComponent pointLight{};
    if (sourceWorld.get_point_light_component(sourceEntity, &pointLight) &&
        !targetWorld.add_point_light_component(targetEntity, pointLight)) {
      success = false;
      return;
    }

    SpotLightComponent spotLight{};
    if (sourceWorld.get_spot_light_component(sourceEntity, &spotLight) &&
        !targetWorld.add_spot_light_component(targetEntity, spotLight)) {
      success = false;
      return;
    }

    ReflectionProbeComponent reflectionProbe{};
    if (sourceWorld.get_reflection_probe_component(sourceEntity,
                                                   &reflectionProbe) &&
        !targetWorld.add_reflection_probe_component(targetEntity,
                                                   reflectionProbe)) {
      success = false;
      return;
    }

    NameComponent name{};
    if (sourceWorld.get_name_component(sourceEntity, &name) &&
        !targetWorld.add_name_component(targetEntity, name)) {
      success = false;
      return;
    }

    ScriptComponent script{};
    if (sourceWorld.get_script_component(sourceEntity, &script) &&
        !targetWorld.add_script_component(targetEntity, script)) {
      success = false;
      return;
    }

    SpringArmComponent springArm{};
    if (sourceWorld.get_spring_arm(sourceEntity, &springArm) &&
        !targetWorld.add_spring_arm(targetEntity, springArm)) {
      success = false;
      return;
    }
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

  return success;
}

/// Handles serialize scene to writer.
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

  ensure_runtime_reflection_registered();
  const core::TypeRegistry &registry = core::global_type_registry();
  const core::TypeDescriptor *transformDesc =
      registry.find_type(kTransformTypeName);
  const core::TypeDescriptor *rigidBodyDesc =
      registry.find_type(kRigidBodyTypeName);
  const core::TypeDescriptor *colliderDesc =
      registry.find_type(kColliderTypeName);
  const core::TypeDescriptor *springArmDesc =
      registry.find_type(kSpringArmTypeName);
  const core::TypeDescriptor *reflectionProbeDesc =
      registry.find_type(kReflectionProbeTypeName);
  if ((transformDesc == nullptr) || (rigidBodyDesc == nullptr) ||
      (colliderDesc == nullptr) || (springArmDesc == nullptr) ||
      (reflectionProbeDesc == nullptr)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "missing runtime reflection descriptors");
    return false;
  }

  core::JsonWriter &writer = *outWriter;
  writer.reset();
  writer.begin_object();
  writer.write_uint(kVersionKey, kCurrentSceneVersion);
  writer.begin_array(kEntitiesKey);

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
    if (world.get_collider(entity, &collider) &&
        !write_reflected_component(writer, kJsonKeyCollider, *colliderDesc,
                                   &collider)) {
      writeFailed = true;
      return;
    }

    MeshComponent mesh{};
    if (world.get_mesh_component(entity, &mesh)) {
      writer.write_key("MeshComponent");
      writer.begin_object();
      writer.write_uint64(kMeshAssetIdKey, mesh.meshAssetId);
      write_vec3(writer, "albedo", mesh.albedo);
      writer.write_float("roughness", mesh.roughness);
      writer.write_float("metallic", mesh.metallic);
      writer.write_float("opacity", mesh.opacity);
      writer.end_object();
    }

    FoliagePatchComponent foliage{};
    if (world.get_foliage_patch_component(entity, &foliage)) {
      write_foliage_patch_component(writer, foliage);
    }

    LightComponent light{};
    if (world.get_light_component(entity, &light)) {
      writer.write_key(kJsonKeyLightComponent);
      writer.begin_object();
      write_vec3(writer, "color", light.color);
      write_vec3(writer, "direction", light.direction);
      writer.write_float("intensity", light.intensity);
      writer.write_uint("type", static_cast<std::uint32_t>(light.type));
      writer.end_object();
    }

    PointLightComponent pointLight{};
    if (world.get_point_light_component(entity, &pointLight)) {
      writer.write_key("PointLightComponent");
      writer.begin_object();
      write_vec3(writer, "color", pointLight.color);
      writer.write_float("intensity", pointLight.intensity);
      writer.write_float("radius", pointLight.radius);
      writer.end_object();
    }

    SpotLightComponent spotLight{};
    if (world.get_spot_light_component(entity, &spotLight)) {
      writer.write_key("SpotLightComponent");
      writer.begin_object();
      write_vec3(writer, "color", spotLight.color);
      write_vec3(writer, "direction", spotLight.direction);
      writer.write_float("intensity", spotLight.intensity);
      writer.write_float("radius", spotLight.radius);
      writer.write_float("innerConeAngle", spotLight.innerConeAngle);
      writer.write_float("outerConeAngle", spotLight.outerConeAngle);
      writer.end_object();
    }

    ReflectionProbeComponent reflectionProbe{};
    if (world.get_reflection_probe_component(entity, &reflectionProbe) &&
        !write_reflected_component(writer, kJsonKeyReflectionProbeComponent,
                                   *reflectionProbeDesc, &reflectionProbe)) {
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

    SpringArmComponent springArm{};
    if (world.get_spring_arm(entity, &springArm) &&
        !write_reflected_component(writer, "SpringArmComponent", *springArmDesc,
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

  return true;
}

} // namespace

/// Resets this object back to its reusable empty state for world.
void reset_world(World &world) noexcept {
  if (world.alive_entity_count() > 0U) {
    thread_local static std::array<Entity, World::kMaxEntities> toDestroy{};
    std::size_t count = 0U;
    world.for_each_alive([&](Entity e) noexcept { toDestroy[count++] = e; });
    for (std::size_t i = 0U; i < count; ++i) {
      static_cast<void>(world.destroy_entity(toDestroy[i]));
    }
  }

  world.timer_manager().clear();
  world.camera_manager().clear();
  world.game_mode().reset();
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
bool load_scene(World &world, const char *path) noexcept {
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

  return load_scene(world, fileBuffer.get(), fileSize);
}

/// Loads the requested resource for scene.
bool load_scene(World &world, const char *buffer, std::size_t size) noexcept {
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

  ensure_runtime_reflection_registered();
  const core::TypeRegistry &registry = core::global_type_registry();
  const core::TypeDescriptor *transformDesc =
      registry.find_type(kTransformTypeName);
  const core::TypeDescriptor *rigidBodyDesc =
      registry.find_type(kRigidBodyTypeName);
  const core::TypeDescriptor *colliderDesc =
      registry.find_type(kColliderTypeName);
  const core::TypeDescriptor *springArmDesc =
      registry.find_type(kSpringArmTypeName);
  const core::TypeDescriptor *reflectionProbeDesc =
      registry.find_type(kReflectionProbeTypeName);
  if ((transformDesc == nullptr) || (rigidBodyDesc == nullptr) ||
      (colliderDesc == nullptr) || (springArmDesc == nullptr) ||
      (reflectionProbeDesc == nullptr)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "missing runtime reflection descriptors");
    return false;
  }

  std::unique_ptr<World> stagedWorld(new (std::nothrow) World());
  if (stagedWorld == nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to allocate staged world for scene load");
    return false;
  }

  if (!deserialize_scene_entities(parser, entities, *transformDesc,
                                  *rigidBodyDesc, *colliderDesc, *springArmDesc,
                                  *reflectionProbeDesc,
                                  *stagedWorld)) {
    return false;
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
      core::JsonValue remainVal{};
      if (parser.get_object_field(timerVal, "remaining", &remainVal)) {
        static_cast<void>(parser.as_float(remainVal, &snap.remainingSeconds));
      }
      core::JsonValue intervalVal{};
      if (parser.get_object_field(timerVal, "interval", &intervalVal)) {
        static_cast<void>(parser.as_float(intervalVal, &snap.intervalSeconds));
      }
      core::JsonValue repeatVal{};
      if (parser.get_object_field(timerVal, "repeat", &repeatVal)) {
        static_cast<void>(parser.as_bool(repeatVal, &snap.repeat));
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
      (committedWorld->transform_count() != stagedWorld->transform_count()) ||
      (committedWorld->rigid_body_count() != stagedWorld->rigid_body_count()) ||
      (committedWorld->collider_count() != stagedWorld->collider_count())) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "scene commit invariant mismatch after copy");
    return false;
  }

  world = *committedWorld;
  return true;
}

} // namespace engine::runtime
