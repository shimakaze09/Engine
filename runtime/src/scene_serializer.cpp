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
constexpr const char *kPointLightTypeName =
    "engine::runtime::PointLightComponent";
constexpr const char *kSpotLightTypeName =
    "engine::runtime::SpotLightComponent";
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

// Reflection-path coverage (S7): Transform, RigidBody, Collider, SpringArm,
// ReflectionProbe, PointLight, and SpotLight serialize through the field
// descriptors registered in reflect_types.cpp. The remaining component types
// stay hand-written deliberately:
//  - MeshComponent: meshAssetId is 64-bit (reflection has no Uint64 field
//    kind) and the reader keeps a legacy "meshId" fallback for scenes
//    authored before asset ids.
//  - LightComponent: `type` is an enum that must clamp to a valid LightType
//    on load rather than round-tripping arbitrary integers.
//  - FoliagePatchComponent, NameComponent, ScriptComponent: fixed-size
//    arrays and bounded strings; reflection has no array/string field kinds
//    (their zero-field descriptors are documented in reflect_types.cpp).

/// Handles log scene error.
bool log_scene_error(const char *message) noexcept {
  if (message != nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel, message);
  }

  return false;
}

/// Bundles the reflection descriptors used by scene component serialization.
struct SceneComponentDescriptors final {
  const core::TypeDescriptor *transform = nullptr;
  const core::TypeDescriptor *rigidBody = nullptr;
  const core::TypeDescriptor *collider = nullptr;
  const core::TypeDescriptor *springArm = nullptr;
  const core::TypeDescriptor *reflectionProbe = nullptr;
  const core::TypeDescriptor *pointLight = nullptr;
  const core::TypeDescriptor *spotLight = nullptr;
};

/// Looks up every reflected scene-component descriptor; logs and fails when
/// any registration is missing.
bool find_scene_descriptors(SceneComponentDescriptors *outDescs) noexcept {
  if (outDescs == nullptr) {
    return false;
  }
  ensure_runtime_reflection_registered();
  const core::TypeRegistry &registry = core::global_type_registry();
  outDescs->transform = registry.find_type(kTransformTypeName);
  outDescs->rigidBody = registry.find_type(kRigidBodyTypeName);
  outDescs->collider = registry.find_type(kColliderTypeName);
  outDescs->springArm = registry.find_type(kSpringArmTypeName);
  outDescs->reflectionProbe = registry.find_type(kReflectionProbeTypeName);
  outDescs->pointLight = registry.find_type(kPointLightTypeName);
  outDescs->spotLight = registry.find_type(kSpotLightTypeName);
  if ((outDescs->transform == nullptr) || (outDescs->rigidBody == nullptr) ||
      (outDescs->collider == nullptr) || (outDescs->springArm == nullptr) ||
      (outDescs->reflectionProbe == nullptr) ||
      (outDescs->pointLight == nullptr) || (outDescs->spotLight == nullptr)) {
    return log_scene_error("missing runtime reflection descriptors");
  }
  return true;
}

/// Handles deserialize scene entities.
bool deserialize_scene_entities(const core::JsonParser &parser,
                                const core::JsonValue &entities,
                                const SceneComponentDescriptors &descs,
                                World &targetWorld) noexcept {
  const core::TypeDescriptor &transformDesc = *descs.transform;
  const core::TypeDescriptor &rigidBodyDesc = *descs.rigidBody;
  const core::TypeDescriptor &colliderDesc = *descs.collider;
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
      if (!read_reflected_component(parser, plVal, pointLightDesc, &pc) ||
          !targetWorld.add_point_light_component(entity, pc)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load PointLightComponent component");
      }
    }

    // SpotLightComponent
    core::JsonValue slVal{};
    if (parser.get_object_field(components, "SpotLightComponent", &slVal)) {
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

    // One line per copyable component type; copy_component supplies the
    // guard/copy body once.
    const auto copy = [&](auto getComponent, auto addComponent) noexcept {
      return copy_component(sourceWorld, targetWorld, sourceEntity,
                            targetEntity, getComponent, addComponent);
    };
    success =
        copy(&World::get_transform, &World::add_transform) &&
        copy(&World::get_rigid_body, &World::add_rigid_body) &&
        copy(&World::get_collider, &World::add_collider) &&
        copy(&World::get_mesh_component, &World::add_mesh_component) &&
        copy(&World::get_foliage_patch_component,
             &World::add_foliage_patch_component) &&
        copy(&World::get_light_component, &World::add_light_component) &&
        copy(&World::get_point_light_component,
             &World::add_point_light_component) &&
        copy(&World::get_spot_light_component,
             &World::add_spot_light_component) &&
        copy(&World::get_reflection_probe_component,
             &World::add_reflection_probe_component) &&
        copy(&World::get_name_component, &World::add_name_component) &&
        copy(&World::get_script_component, &World::add_script_component) &&
        copy(&World::get_spring_arm, &World::add_spring_arm);
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

  SceneComponentDescriptors descs{};
  if (!find_scene_descriptors(&descs)) {
    return false;
  }
  const core::TypeDescriptor *transformDesc = descs.transform;
  const core::TypeDescriptor *rigidBodyDesc = descs.rigidBody;
  const core::TypeDescriptor *colliderDesc = descs.collider;
  const core::TypeDescriptor *springArmDesc = descs.springArm;
  const core::TypeDescriptor *reflectionProbeDesc = descs.reflectionProbe;

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
    if (world.get_point_light_component(entity, &pointLight) &&
        !write_reflected_component(writer, "PointLightComponent",
                                   *descs.pointLight, &pointLight)) {
      writeFailed = true;
      return;
    }

    SpotLightComponent spotLight{};
    if (world.get_spot_light_component(entity, &spotLight) &&
        !write_reflected_component(writer, "SpotLightComponent",
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

  SceneComponentDescriptors descs{};
  if (!find_scene_descriptors(&descs)) {
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
