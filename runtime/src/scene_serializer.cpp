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
#include "engine/runtime/world.h"

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
constexpr const char *kSpringArmTypeName = "engine::runtime::SpringArmComponent";
constexpr const char *kNameFieldKey = "name";
constexpr const char *kMeshAssetIdKey = "meshAssetId";

bool open_file_for_read(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }

  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "rb") == 0;
#else
  *outFile = std::fopen(path, "rb");
  return *outFile != nullptr;
#endif
}

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }

  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

bool read_text_file(const char *path, std::unique_ptr<char[]> *outBuffer,
                    std::size_t *outSize) noexcept {
  if ((path == nullptr) || (outBuffer == nullptr) || (outSize == nullptr)) {
    return false;
  }

  outBuffer->reset();
  *outSize = 0U;

  FILE *file = nullptr;
  if (!open_file_for_read(path, &file) || (file == nullptr)) {
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }

  const long fileLength = std::ftell(file);
  if (fileLength <= 0L) {
    std::fclose(file);
    return false;
  }

  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }

  const std::size_t fileSize = static_cast<std::size_t>(fileLength);
  std::unique_ptr<char[]> buffer(new (std::nothrow) char[fileSize + 1U]);
  if (buffer == nullptr) {
    std::fclose(file);
    return false;
  }

  const std::size_t readCount = std::fread(buffer.get(), 1U, fileSize, file);
  const bool hitError = std::ferror(file) != 0;
  std::fclose(file);

  if (hitError || (readCount != fileSize)) {
    return false;
  }

  buffer[fileSize] = '\0';
  *outSize = fileSize;
  outBuffer->swap(buffer);
  return true;
}

bool write_text_file(const char *path, const char *text,
                     std::size_t size) noexcept {
  if ((path == nullptr) || (text == nullptr) || (size == 0U)) {
    return false;
  }

  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }

  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void write_vec2(core::JsonWriter &writer, const char *key,
                const math::Vec2 &value) noexcept {
  writer.begin_array(key);
  writer.write_float_value(value.x);
  writer.write_float_value(value.y);
  writer.end_array();
}

void write_vec3(core::JsonWriter &writer, const char *key,
                const math::Vec3 &value) noexcept {
  writer.begin_array(key);
  writer.write_float_value(value.x);
  writer.write_float_value(value.y);
  writer.write_float_value(value.z);
  writer.end_array();
}

void write_vec4(core::JsonWriter &writer, const char *key,
                const math::Vec4 &value) noexcept {
  writer.begin_array(key);
  writer.write_float_value(value.x);
  writer.write_float_value(value.y);
  writer.write_float_value(value.z);
  writer.write_float_value(value.w);
  writer.end_array();
}

void write_quat(core::JsonWriter &writer, const char *key,
                const math::Quat &value) noexcept {
  writer.begin_array(key);
  writer.write_float_value(value.x);
  writer.write_float_value(value.y);
  writer.write_float_value(value.z);
  writer.write_float_value(value.w);
  writer.end_array();
}

bool read_float_array(const core::JsonParser &parser,
                      const core::JsonValue &arrayValue, float *outValues,
                      std::size_t expectedCount) noexcept {
  if ((outValues == nullptr) ||
      (arrayValue.type != core::JsonValue::Type::Array)) {
    return false;
  }

  if (parser.array_size(arrayValue) != expectedCount) {
    return false;
  }

  for (std::size_t i = 0U; i < expectedCount; ++i) {
    core::JsonValue element{};
    if (!parser.get_array_element(arrayValue, i, &element)) {
      return false;
    }

    if (!parser.as_float(element, &outValues[i])) {
      return false;
    }
  }

  return true;
}

bool read_vec2(const core::JsonParser &parser, const core::JsonValue &value,
               math::Vec2 *outVec) noexcept {
  if (outVec == nullptr) {
    return false;
  }

  float fields[2] = {};
  if (!read_float_array(parser, value, fields, 2U)) {
    return false;
  }

  outVec->x = fields[0];
  outVec->y = fields[1];
  return true;
}

bool read_vec3(const core::JsonParser &parser, const core::JsonValue &value,
               math::Vec3 *outVec) noexcept {
  if (outVec == nullptr) {
    return false;
  }

  float fields[3] = {};
  if (!read_float_array(parser, value, fields, 3U)) {
    return false;
  }

  outVec->x = fields[0];
  outVec->y = fields[1];
  outVec->z = fields[2];
  return true;
}

bool read_vec4(const core::JsonParser &parser, const core::JsonValue &value,
               math::Vec4 *outVec) noexcept {
  if (outVec == nullptr) {
    return false;
  }

  float fields[4] = {};
  if (!read_float_array(parser, value, fields, 4U)) {
    return false;
  }

  outVec->x = fields[0];
  outVec->y = fields[1];
  outVec->z = fields[2];
  outVec->w = fields[3];
  return true;
}

bool read_quat(const core::JsonParser &parser, const core::JsonValue &value,
               math::Quat *outQuat) noexcept {
  if (outQuat == nullptr) {
    return false;
  }

  float fields[4] = {};
  if (!read_float_array(parser, value, fields, 4U)) {
    return false;
  }

  outQuat->x = fields[0];
  outQuat->y = fields[1];
  outQuat->z = fields[2];
  outQuat->w = fields[3];
  return true;
}

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
    if (!parser.as_uint(meshIdValue, &component.meshAssetId)) {
      return false;
    }
  } else if (parser.get_object_field(meshObject, "meshId", &meshIdValue)) {
    // Backward-compatible read path for scenes authored before asset IDs.
    if (!parser.as_uint(meshIdValue, &component.meshAssetId)) {
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

bool log_scene_error(const char *message) noexcept {
  if (message != nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel, message);
  }

  return false;
}

bool deserialize_scene_entities(const core::JsonParser &parser,
                                const core::JsonValue &entities,
                                const core::TypeDescriptor &transformDesc,
                                const core::TypeDescriptor &rigidBodyDesc,
                                const core::TypeDescriptor &colliderDesc,
                                const core::TypeDescriptor &springArmDesc,
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
    if (parser.get_object_field(components, "Transform", &transformValue)) {
      Transform transform{};
      if (!read_reflected_component(parser, transformValue, transformDesc,
                                    &transform) ||
          !targetWorld.add_transform(entity, transform)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load Transform component");
      }
    }

    core::JsonValue rigidBodyValue{};
    if (parser.get_object_field(components, "RigidBody", &rigidBodyValue)) {
      RigidBody rigidBody{};
      if (!read_reflected_component(parser, rigidBodyValue, rigidBodyDesc,
                                    &rigidBody) ||
          !targetWorld.add_rigid_body(entity, rigidBody)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load RigidBody component");
      }
    }

    core::JsonValue colliderValue{};
    if (parser.get_object_field(components, "Collider", &colliderValue)) {
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

    core::JsonValue lightValue{};
    if (parser.get_object_field(components, "LightComponent", &lightValue)) {
      LightComponent light{};
      if (!read_light_component(parser, lightValue, &light) ||
          !targetWorld.add_light_component(entity, light)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load LightComponent component");
      }
    }

    core::JsonValue nameValue{};
    if (parser.get_object_field(components, kNameFieldKey, &nameValue)) {
      const char *nameBegin = nullptr;
      std::size_t nameLength = 0U;
      if (!parser.as_string(nameValue, &nameBegin, &nameLength)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to parse name component string");
      }

      NameComponent nameComponent{};
      const std::size_t maxLength = sizeof(nameComponent.name) - 1U;
      const std::size_t copyLength =
          (nameLength > maxLength) ? maxLength : nameLength;
      if ((copyLength > 0U) && (nameBegin != nullptr)) {
        std::memcpy(nameComponent.name, nameBegin, copyLength);
      }
      nameComponent.name[copyLength] = '\0';

      if (!targetWorld.add_name_component(entity, nameComponent)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to load NameComponent");
      }
    }

    core::JsonValue scriptValue{};
    if (parser.get_object_field(components, "ScriptComponent", &scriptValue)) {
      const char *pathBegin = nullptr;
      std::size_t pathLength = 0U;
      if (!parser.as_string(scriptValue, &pathBegin, &pathLength)) {
        targetWorld.destroy_entity(entity);
        return log_scene_error("failed to parse ScriptComponent path");
      }

      ScriptComponent scriptComp{};
      const std::size_t maxLen = sizeof(scriptComp.scriptPath) - 1U;
      const std::size_t copyLen = (pathLength > maxLen) ? maxLen : pathLength;
      if ((copyLen > 0U) && (pathBegin != nullptr)) {
        std::memcpy(scriptComp.scriptPath, pathBegin, copyLen);
      }
      scriptComp.scriptPath[copyLen] = '\0';

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

    LightComponent light{};
    if (sourceWorld.get_light_component(sourceEntity, &light) &&
        !targetWorld.add_light_component(targetEntity, light)) {
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

bool serialize_scene_to_writer(const World &world,
                               core::JsonWriter *outWriter) noexcept {
  if (outWriter == nullptr) {
    return false;
  }

  if (world.current_phase() != WorldPhase::Idle) {
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
  if ((transformDesc == nullptr) || (rigidBodyDesc == nullptr) ||
      (colliderDesc == nullptr) || (springArmDesc == nullptr)) {
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
      if (!write_reflected_component(writer, "Transform", *transformDesc,
                                     &transform)) {
        writeFailed = true;
        return;
      }
    }

    RigidBody rigidBody{};
    if (world.get_rigid_body(entity, &rigidBody) &&
        !write_reflected_component(writer, "RigidBody", *rigidBodyDesc,
                                   &rigidBody)) {
      writeFailed = true;
      return;
    }

    Collider collider{};
    if (world.get_collider(entity, &collider) &&
        !write_reflected_component(writer, "Collider", *colliderDesc,
                                   &collider)) {
      writeFailed = true;
      return;
    }

    MeshComponent mesh{};
    if (world.get_mesh_component(entity, &mesh)) {
      writer.write_key("MeshComponent");
      writer.begin_object();
      writer.write_uint(kMeshAssetIdKey, mesh.meshAssetId);
      write_vec3(writer, "albedo", mesh.albedo);
      writer.write_float("roughness", mesh.roughness);
      writer.write_float("metallic", mesh.metallic);
      writer.write_float("opacity", mesh.opacity);
      writer.end_object();
    }

    LightComponent light{};
    if (world.get_light_component(entity, &light)) {
      writer.write_key("LightComponent");
      writer.begin_object();
      write_vec3(writer, "color", light.color);
      write_vec3(writer, "direction", light.direction);
      writer.write_float("intensity", light.intensity);
      writer.write_uint("type", static_cast<std::uint32_t>(light.type));
      writer.end_object();
    }

    NameComponent name{};
    if (world.get_name_component(entity, &name)) {
      writer.write_string(kNameFieldKey, name.name);
    }

    ScriptComponent script{};
    if (world.get_script_component(entity, &script) &&
        (script.scriptPath[0] != '\0')) {
      writer.write_string("ScriptComponent", script.scriptPath);
    }

    SpringArmComponent springArm{};
    if (world.get_spring_arm(entity, &springArm) &&
        !write_reflected_component(writer, "SpringArmComponent",
                                   *springArmDesc, &springArm)) {
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

void reset_world(World &world) noexcept {
  if (world.alive_entity_count() == 0U) {
    return;
  }
  thread_local static std::array<Entity, World::kMaxEntities> toDestroy{};
  std::size_t count = 0U;
  world.for_each_alive([&](Entity e) noexcept { toDestroy[count++] = e; });
  for (std::size_t i = 0U; i < count; ++i) {
    static_cast<void>(world.destroy_entity(toDestroy[i]));
  }
}

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

bool load_scene(World &world, const char *buffer, std::size_t size) noexcept {
  if ((buffer == nullptr) || (size == 0U)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "load_scene called with invalid input buffer");
    return false;
  }

  if (world.current_phase() != WorldPhase::Idle) {
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
  if ((transformDesc == nullptr) || (rigidBodyDesc == nullptr) ||
      (colliderDesc == nullptr) || (springArmDesc == nullptr)) {
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
                                  *rigidBodyDesc, *colliderDesc,
                                  *springArmDesc, *stagedWorld)) {
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

  reset_world(world);

  if (!copy_world_contents(*stagedWorld, world)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to commit loaded scene");
    reset_world(world);
    return false;
  }

  if ((world.alive_entity_count() != stagedWorld->alive_entity_count()) ||
      (world.transform_count() != stagedWorld->transform_count()) ||
      (world.rigid_body_count() != stagedWorld->rigid_body_count()) ||
      (world.collider_count() != stagedWorld->collider_count())) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "scene commit invariant mismatch after copy");
    reset_world(world);
    return false;
  }

  return true;
}

} // namespace engine::runtime
