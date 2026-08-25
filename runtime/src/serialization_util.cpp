// Implements the shared serializer file-IO and JSON field helpers.
// Single source of truth for behavior the scene and prefab serializers used
// to duplicate (REVIEW_FINDINGS S5). Reads are strict: a field that is
// present but malformed fails the read instead of being silently skipped.

#include "serialization_util.h"

#include <new>

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/serialization_keys.h"

namespace engine::runtime {

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
  return core::atomic_write_file(path, text, size);
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
  return parser.as_float_array(arrayValue, outValues, expectedCount);
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

// Fully-qualified reflection registration names for the reflected component
// descriptor lookups below.
constexpr const char *kTransformTypeName = "engine::runtime::Transform";
constexpr const char *kRigidBodyTypeName = "engine::runtime::RigidBody";
constexpr const char *kSpringArmTypeName =
    "engine::runtime::SpringArmComponent";
constexpr const char *kReflectionProbeTypeName =
    "engine::runtime::ReflectionProbeComponent";
constexpr const char *kPointLightTypeName =
    "engine::runtime::PointLightComponent";
constexpr const char *kSpotLightTypeName =
    "engine::runtime::SpotLightComponent";
constexpr const char *kSceneCaptureTypeName =
    "engine::runtime::SceneCaptureComponent";
constexpr const char *kCameraTypeName = "engine::runtime::CameraComponent";

// Object-shape field names for AnimationComponent (issue #253). Named
// rather than repeated as literals because the writer and reader below are
// the only two places they appear, and a silent divergence between them is
// the drift this codec exists to close.
constexpr const char *kAnimationControllerPathField = "controllerPath";
constexpr const char *kAnimationPlayingField = "playing";
constexpr const char *kAnimationPlaybackSpeedField = "playbackSpeed";

bool find_reflected_component_descriptors(
    ReflectedComponentDescriptors *outDescs, const char *logChannel) noexcept {
  if (outDescs == nullptr) {
    return false;
  }
  ensure_runtime_reflection_registered();
  const core::TypeRegistry &registry = core::global_type_registry();
  outDescs->transform = registry.find_type(kTransformTypeName);
  outDescs->rigidBody = registry.find_type(kRigidBodyTypeName);
  outDescs->springArm = registry.find_type(kSpringArmTypeName);
  outDescs->reflectionProbe = registry.find_type(kReflectionProbeTypeName);
  outDescs->pointLight = registry.find_type(kPointLightTypeName);
  outDescs->spotLight = registry.find_type(kSpotLightTypeName);
  outDescs->sceneCapture = registry.find_type(kSceneCaptureTypeName);
  outDescs->camera = registry.find_type(kCameraTypeName);
  if ((outDescs->transform == nullptr) || (outDescs->rigidBody == nullptr) ||
      (outDescs->springArm == nullptr) ||
      (outDescs->reflectionProbe == nullptr) ||
      (outDescs->pointLight == nullptr) || (outDescs->spotLight == nullptr) ||
      (outDescs->sceneCapture == nullptr) || (outDescs->camera == nullptr)) {
    if (logChannel != nullptr) {
      core::log_message(core::LogLevel::Error, logChannel,
                        "missing runtime reflection descriptors");
    }
    return false;
  }
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

// Reflection-path coverage (S7): Transform, RigidBody, SpringArm,
// ReflectionProbe, PointLight, SpotLight, and SceneCapture serialize through
// the field descriptors registered in reflect_types.cpp; the scene and
// prefab serializers both consume these shared codecs. The remaining
// component types stay hand-written deliberately:
//  - Collider: shape has an 8-bit enum representation, so the dedicated reader
//    validates a uint32 temporary before assigning the enum.
//  - MeshComponent: meshAssetId is 64-bit (reflection has no Uint64 field
//    kind) and the reader keeps a legacy "meshId" fallback for content
//    authored before asset ids.
//  - LightComponent: `type` is an enum that must clamp to a valid LightType
//    on load rather than round-tripping arbitrary integers.
//  - FoliagePatchComponent, NameComponent, ScriptComponent,
//    AnimationComponent: fixed-size arrays and bounded strings; reflection
//    has no array/string field kinds (their zero-field descriptors are
//    documented in reflect_types.cpp). AnimationComponent's authored
//    bool/float therefore ride its hand-written codec rather than the
//    reflected path, since one unrepresentable field takes the whole type
//    off it (issue #253).

void write_mesh_component(core::JsonWriter &writer,
                          const MeshComponent &component) noexcept {
  writer.write_key(kJsonKeyMeshComponent);
  writer.begin_object();
  writer.write_uint64("meshAssetId", component.meshAssetId);
  // Written only when set so pre-material files stay byte-identical.
  if (component.materialAssetId != 0ULL) {
    writer.write_uint64("materialAssetId", component.materialAssetId);
  }
  write_vec3(writer, "albedo", component.albedo);
  writer.write_float("roughness", component.roughness);
  writer.write_float("metallic", component.metallic);
  writer.write_float("opacity", component.opacity);
  // Written only when set so pre-capture files stay byte-identical.
  if (component.sceneCaptureSourceId != 0U) {
    writer.write_uint("sceneCaptureSourceId", component.sceneCaptureSourceId);
  }
  writer.end_object();
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
  if (parser.get_object_field(meshObject, "meshAssetId", &meshIdValue)) {
    if (!parser.as_uint64(meshIdValue, &component.meshAssetId)) {
      return false;
    }
  } else if (parser.get_object_field(meshObject, "meshId", &meshIdValue)) {
    // Backward-compatible read path for content authored before asset IDs.
    if (!parser.as_uint64(meshIdValue, &component.meshAssetId)) {
      return false;
    }
  }

  core::JsonValue materialIdValue{};
  if (parser.get_object_field(meshObject, "materialAssetId",
                              &materialIdValue)) {
    if (!parser.as_uint64(materialIdValue, &component.materialAssetId)) {
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

  core::JsonValue captureSourceValue{};
  if (parser.get_object_field(meshObject, "sceneCaptureSourceId",
                              &captureSourceValue)) {
    if (!parser.as_uint(captureSourceValue, &component.sceneCaptureSourceId)) {
      return false;
    }
  }

  *outComponent = component;
  return true;
}

void write_light_component(core::JsonWriter &writer,
                           const LightComponent &component) noexcept {
  writer.write_key(kJsonKeyLightComponent);
  writer.begin_object();
  write_vec3(writer, "color", component.color);
  write_vec3(writer, "direction", component.direction);
  writer.write_float("intensity", component.intensity);
  writer.write_uint("type", static_cast<std::uint32_t>(component.type));
  writer.end_object();
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


// Hull payloads round-trip via HullSource provenance (rebuilt by
// World::add_collider on install); Heightfield payloads are NOT serialized —
// they are reachable only from tests today, and terrain authoring is expected
// to bring its own asset-backed provenance before that changes.
bool write_collider_component(core::JsonWriter &writer,
                              const Collider &component) noexcept {
  const std::uint32_t shape = static_cast<std::uint32_t>(component.shape);
  if (shape > static_cast<std::uint32_t>(ColliderShape::Heightfield)) {
    return false;
  }
  const std::uint32_t hullSource =
      static_cast<std::uint32_t>(component.hullSource);
  if (hullSource > static_cast<std::uint32_t>(HullSource::Pyramid)) {
    return false;
  }

  writer.write_key(kJsonKeyCollider);
  writer.begin_object();
  writer.write_uint("shape", shape);
  writer.write_uint("hullSource", hullSource);
  write_vec3(writer, "localPosition", component.localPosition);
  write_quat(writer, "localRotation", component.localRotation);
  write_vec3(writer, "halfExtents", component.halfExtents);
  writer.write_float("restitution", component.restitution);
  writer.write_float("staticFriction", component.staticFriction);
  writer.write_float("dynamicFriction", component.dynamicFriction);
  writer.write_float("density", component.density);
  writer.write_uint("collisionLayer", component.collisionLayer);
  writer.write_uint("collisionMask", component.collisionMask);
  writer.end_object();
  return !writer.failed();
}

bool read_collider_component(const core::JsonParser &parser,
                             const core::JsonValue &colliderObject,
                             Collider *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (colliderObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  Collider component{};
  core::JsonValue value{};
  std::uint32_t shape = static_cast<std::uint32_t>(component.shape);
  if (parser.get_object_field(colliderObject, "shape", &value)) {
    if (!parser.as_uint(value, &shape) ||
        (shape > static_cast<std::uint32_t>(ColliderShape::Heightfield))) {
      return false;
    }
    component.shape = static_cast<ColliderShape>(shape);
  }
  if (parser.get_object_field(colliderObject, "hullSource", &value)) {
    std::uint32_t hullSource = 0U;
    if (!parser.as_uint(value, &hullSource) ||
        (hullSource > static_cast<std::uint32_t>(HullSource::Pyramid))) {
      return false;
    }
    component.hullSource = static_cast<HullSource>(hullSource);
  }
  if (parser.get_object_field(colliderObject, "localPosition", &value) &&
      !read_vec3(parser, value, &component.localPosition)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "localRotation", &value) &&
      !read_quat(parser, value, &component.localRotation)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "halfExtents", &value) &&
      !read_vec3(parser, value, &component.halfExtents)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "restitution", &value) &&
      !parser.as_float(value, &component.restitution)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "staticFriction", &value) &&
      !parser.as_float(value, &component.staticFriction)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "dynamicFriction", &value) &&
      !parser.as_float(value, &component.dynamicFriction)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "density", &value) &&
      !parser.as_float(value, &component.density)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "collisionLayer", &value) &&
      !parser.as_uint(value, &component.collisionLayer)) {
    return false;
  }
  if (parser.get_object_field(colliderObject, "collisionMask", &value) &&
      !parser.as_uint(value, &component.collisionMask)) {
    return false;
  }

  *outComponent = component;
  return true;
}

void write_foliage_patch_component(
    core::JsonWriter &writer, const FoliagePatchComponent &component) noexcept {
  writer.write_key(kJsonKeyFoliagePatchComponent);
  writer.begin_object();

  writer.begin_array("meshAssetIds");
  for (std::size_t i = 0U; i < FoliagePatchComponent::kMaxLods; ++i) {
    writer.write_uint64_value(component.meshAssetIds[i]);
  }
  writer.end_array();

  const std::uint32_t instanceCount =
      (component.instanceCount >
       static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances))
          ? static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)
          : component.instanceCount;
  writer.write_uint("instanceCount", instanceCount);
  writer.write_float("density", component.density);
  write_vec3(writer, "albedo", component.albedo);
  writer.write_float("roughness", component.roughness);
  writer.write_float("metallic", component.metallic);
  writer.write_float("opacity", component.opacity);
  writer.write_float("windStrength", component.windStrength);
  writer.write_float("windFrequency", component.windFrequency);

  writer.begin_array("instances");
  for (std::uint32_t i = 0U; i < instanceCount; ++i) {
    const FoliageInstance &instance = component.instances[i];
    writer.begin_object();
    write_vec3(writer, "offset", instance.offset);
    writer.write_float("scale", instance.scale);
    writer.write_float("phase", instance.phase);
    writer.write_uint("lodIndex", instance.lodIndex);
    writer.end_object();
  }
  writer.end_array();

  writer.end_object();
}

bool read_foliage_patch_component(
    const core::JsonParser &parser, const core::JsonValue &foliageObject,
    FoliagePatchComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (foliageObject.type != core::JsonValue::Type::Object)) {
    return false;
  }

  FoliagePatchComponent component{};
  core::JsonValue value{};

  if (parser.get_object_field(foliageObject, "meshAssetIds", &value) &&
      (value.type == core::JsonValue::Type::Array)) {
    const std::size_t meshCount = parser.array_size(value);
    const std::size_t count = (meshCount < FoliagePatchComponent::kMaxLods)
                                  ? meshCount
                                  : FoliagePatchComponent::kMaxLods;
    for (std::size_t i = 0U; i < count; ++i) {
      core::JsonValue element{};
      if (!parser.get_array_element(value, i, &element) ||
          !parser.as_uint64(element, &component.meshAssetIds[i])) {
        return false;
      }
    }
  }

  if (parser.get_object_field(foliageObject, "density", &value) &&
      !parser.as_float(value, &component.density)) {
    return false;
  }
  if (parser.get_object_field(foliageObject, "albedo", &value) &&
      !read_vec3(parser, value, &component.albedo)) {
    return false;
  }
  if (parser.get_object_field(foliageObject, "roughness", &value) &&
      !parser.as_float(value, &component.roughness)) {
    return false;
  }
  if (parser.get_object_field(foliageObject, "metallic", &value) &&
      !parser.as_float(value, &component.metallic)) {
    return false;
  }
  if (parser.get_object_field(foliageObject, "opacity", &value) &&
      !parser.as_float(value, &component.opacity)) {
    return false;
  }
  if (parser.get_object_field(foliageObject, "windStrength", &value) &&
      !parser.as_float(value, &component.windStrength)) {
    return false;
  }
  if (parser.get_object_field(foliageObject, "windFrequency", &value) &&
      !parser.as_float(value, &component.windFrequency)) {
    return false;
  }

  std::uint32_t requestedCount =
      static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances);
  if (parser.get_object_field(foliageObject, "instanceCount", &value) &&
      !parser.as_uint(value, &requestedCount)) {
    return false;
  }

  core::JsonValue instancesValue{};
  if (parser.get_object_field(foliageObject, "instances", &instancesValue) &&
      (instancesValue.type == core::JsonValue::Type::Array)) {
    std::size_t count = parser.array_size(instancesValue);
    if (count > FoliagePatchComponent::kMaxInstances) {
      count = FoliagePatchComponent::kMaxInstances;
    }
    if (count > requestedCount) {
      count = requestedCount;
    }

    for (std::size_t i = 0U; i < count; ++i) {
      core::JsonValue instanceValue{};
      if (!parser.get_array_element(instancesValue, i, &instanceValue) ||
          (instanceValue.type != core::JsonValue::Type::Object)) {
        return false;
      }

      FoliageInstance instance{};
      if (parser.get_object_field(instanceValue, "offset", &value) &&
          !read_vec3(parser, value, &instance.offset)) {
        return false;
      }
      if (parser.get_object_field(instanceValue, "scale", &value) &&
          !parser.as_float(value, &instance.scale)) {
        return false;
      }
      if (parser.get_object_field(instanceValue, "phase", &value) &&
          !parser.as_float(value, &instance.phase)) {
        return false;
      }
      if (parser.get_object_field(instanceValue, "lodIndex", &value) &&
          !parser.as_uint(value, &instance.lodIndex)) {
        return false;
      }
      component.instances[i] = instance;
    }
    component.instanceCount = static_cast<std::uint32_t>(count);
  } else {
    if (requestedCount >
        static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)) {
      requestedCount =
          static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances);
    }
    component.instanceCount = requestedCount;
  }

  *outComponent = component;
  return true;
}

void write_animation_component(core::JsonWriter &writer, const char *key,
                               const AnimationComponent &component) noexcept {
  if ((key == nullptr) || (component.controllerPath[0] == '\0')) {
    return;
  }

  // Compared against a default-constructed component rather than literals so
  // the shape follows the component's own defaults if they ever change.
  const AnimationComponent defaults{};
  if ((component.playing == defaults.playing) &&
      (component.playbackSpeed == defaults.playbackSpeed)) {
    writer.write_string(key, component.controllerPath);
    return;
  }

  writer.write_key(key);
  writer.begin_object();
  writer.write_string(kAnimationControllerPathField, component.controllerPath);
  writer.write_bool(kAnimationPlayingField, component.playing);
  writer.write_float(kAnimationPlaybackSpeedField, component.playbackSpeed);
  writer.end_object();
}

bool read_animation_component(const core::JsonParser &parser,
                              const core::JsonValue &value,
                              bool requireNonEmptyPath,
                              AnimationComponent *outComponent) noexcept {
  if (outComponent == nullptr) {
    return false;
  }

  AnimationComponent component{};

  if (value.type == core::JsonValue::Type::String) {
    if (!parser.copy_string_strict(value, component.controllerPath,
                                   sizeof(component.controllerPath))) {
      return false;
    }
  } else if (value.type == core::JsonValue::Type::Object) {
    core::JsonValue field{};
    if (!parser.get_object_field(value, kAnimationControllerPathField,
                                 &field) ||
        !parser.copy_string_strict(field, component.controllerPath,
                                   sizeof(component.controllerPath))) {
      return false;
    }
    if (parser.get_object_field(value, kAnimationPlayingField, &field) &&
        !parser.as_bool(field, &component.playing)) {
      return false;
    }
    if (parser.get_object_field(value, kAnimationPlaybackSpeedField, &field) &&
        !parser.as_float(field, &component.playbackSpeed)) {
      return false;
    }
  } else {
    return false;
  }

  if (requireNonEmptyPath && (component.controllerPath[0] == '\0')) {
    return false;
  }

  // `component` starts default-constructed, so the runtime slots the format
  // never carries land on their defaults rather than on stale values.
  *outComponent = component;
  return true;
}

} // namespace engine::runtime
