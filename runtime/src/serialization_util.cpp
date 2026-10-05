// Implements the shared serializer file-IO and JSON field helpers.
// Single source of truth for behavior the scene and prefab serializers used
// to duplicate (REVIEW_FINDINGS S5). Reads are strict: a field that is
// present but malformed fails the read instead of being silently skipped.

#include "serialization_util.h"

#include "engine/core/vfs.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "engine/content/asset_identity.h"
#include "engine/content/asset_ref_json.h"
#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/serialization_keys.h"

namespace engine::runtime {
namespace {

/// These codecs serve both the scene and the prefab serializer, so their
/// refusals carry a channel of their own rather than either format's.
constexpr const char *kSerializationLogChannel = "serialization";

/// Reads an optional float field strictly: absent leaves the caller's
/// default, present-and-valid assigns, present-but-malformed refuses and
/// names the field.
///
/// Four fields used to discard the parse result instead — the only ones in
/// these readers that did. A hand-edited or bit-rotted `"roughness": "0.5"`
/// loaded as the default with no diagnostic, and the next save persisted
/// that default over the authored value, so the original was destroyed by
/// opening and saving the file.
bool read_optional_float_strict(const core::JsonParser &parser,
                                const core::JsonValue &object, const char *key,
                                float *out) noexcept {
  core::JsonValue value{};
  if (!parser.get_object_field(object, key, &value)) {
    return true;
  }
  if (parser.as_float(value, out)) {
    return true;
  }

  // Naming the field is what keeps the refusal recoverable: the author has
  // to know which one to correct, and the document is otherwise unchanged.
  char message[160] = {};
  static_cast<void>(std::snprintf(
      message, sizeof(message),
      "'%s' is present but not a number; refusing the load rather than "
      "substituting a default that would overwrite it on the next save",
      key));
  core::log_message(core::LogLevel::Error, kSerializationLogChannel, message);
  return false;
}

} // namespace

bool schema_version_supported(const core::JsonParser &parser,
                              const core::JsonValue &root,
                              std::uint32_t currentVersion, const char *noun,
                              const char *channel,
                              std::uint32_t *outVersion) noexcept {
  // An absent key cannot be the current revision, so it is refused below
  // like any other wrong value; a present one is read strictly, so a
  // value of the wrong JSON type is a refusal rather than a silent
  // fall back.
  std::uint32_t version = 0U;
  core::JsonValue versionValue{};
  if (parser.get_object_field(root, kSchemaVersionKey, &versionValue) &&
      !parser.as_uint(versionValue, &version)) {
    char message[128] = {};
    static_cast<void>(std::snprintf(message, sizeof(message),
                                    "%s version must be an unsigned integer",
                                    noun));
    core::log_message(core::LogLevel::Error, channel, message);
    return false;
  }

  if (version != currentVersion) {
    char message[160] = {};
    static_cast<void>(std::snprintf(
        message, sizeof(message),
        "unsupported %s version %u; this build reads revision %u only", noun,
        version, currentVersion));
    core::log_message(core::LogLevel::Error, channel, message);
    return false;
  }

  if (outVersion != nullptr) {
    *outVersion = version;
  }
  return true;
}

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

const char *resolve_document_path(const char *path, char *buffer,
                                  std::size_t capacity) noexcept {
  if ((path == nullptr) || (buffer == nullptr) || (capacity == 0U)) {
    return path;
  }
  return core::vfs_resolve_os_path(path, buffer, capacity) ? buffer : path;
}

bool read_text_file(const char *path, std::unique_ptr<char[]> *outBuffer,
                    std::size_t *outSize) noexcept {
  if ((path == nullptr) || (outBuffer == nullptr) || (outSize == nullptr)) {
    return false;
  }

  outBuffer->reset();
  *outSize = 0U;

  char resolved[kMaxDocumentOsPath] = {};
  FILE *file = nullptr;
  if (!open_file_for_read(
          resolve_document_path(path, resolved, sizeof(resolved)), &file) ||
      (file == nullptr)) {
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
  char resolved[kMaxDocumentOsPath] = {};
  return core::atomic_write_file(
      resolve_document_path(path, resolved, sizeof(resolved)), text, size);
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
constexpr const char *kCharacterControllerTypeName =
    "engine::runtime::CharacterControllerComponent";
constexpr const char *kNavAgentTypeName = "engine::runtime::NavAgentComponent";

// Object-shape field names for AnimationComponent. Named
// rather than repeated as literals because the writer and reader below are
// the only two places they appear, and a silent divergence between them is
// the drift this codec exists to close.
/// Wire keys for the authored asset references a component carries. Named
/// once because each appears in exactly two places — the writer and the
/// reader — and a silent divergence between them is what this codec
/// exists to prevent.
constexpr const char *kMeshRefField = "mesh";
constexpr const char *kMaterialRefField = "material";
constexpr const char *kEnvironmentRefField = "environment";
constexpr const char *kFoliageMeshRefsField = "meshes";

constexpr const char *kAnimationControllerPathField = "controllerPath";

// NavMeshSurfaceComponent's field names, shared by its writer and reader.
constexpr const char *kNavHalfExtentsField = "halfExtents";
constexpr const char *kNavCellSizeField = "cellSize";
constexpr const char *kNavAgentRadiusField = "agentRadius";
constexpr const char *kNavAgentHeightField = "agentHeight";
constexpr const char *kNavMaxClimbField = "maxClimb";
constexpr const char *kNavMaxSlopeField = "maxSlopeDegrees";
constexpr const char *kNavMeshPathField = "navMeshPath";
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
  outDescs->characterController =
      registry.find_type(kCharacterControllerTypeName);
  outDescs->navAgent = registry.find_type(kNavAgentTypeName);
  if ((outDescs->transform == nullptr) || (outDescs->rigidBody == nullptr) ||
      (outDescs->springArm == nullptr) ||
      (outDescs->reflectionProbe == nullptr) ||
      (outDescs->pointLight == nullptr) || (outDescs->spotLight == nullptr) ||
      (outDescs->sceneCapture == nullptr) || (outDescs->camera == nullptr) ||
      (outDescs->characterController == nullptr) ||
      (outDescs->navAgent == nullptr)) {
    if (logChannel != nullptr) {
      core::log_message(core::LogLevel::Error, logChannel,
                        "missing runtime reflection descriptors");
    }
    return false;
  }
  return true;
}

namespace {

// True when every byte of the field is zero: the value an optional field
// is omitted at and read back as. A field lying outside the type is not
// zero, so the write below reaches its own refusal.
bool field_bytes_are_zero(const core::TypeDescriptor &descriptor,
                          const core::TypeField &field,
                          const void *instance) noexcept {
  if ((field.offset > descriptor.size) ||
      (field.size > (descriptor.size - field.offset))) {
    return false;
  }
  const auto *bytes =
      static_cast<const unsigned char *>(instance) + field.offset;
  for (std::size_t i = 0U; i < field.size; ++i) {
    if (bytes[i] != 0U) {
      return false;
    }
  }
  return true;
}

} // namespace

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
    if (field.key == nullptr) {
      continue;
    }
    if (field.omitWhenZero &&
        field_bytes_are_zero(descriptor, field, instance)) {
      continue;
    }

    switch (field.kind) {
    case core::TypeField::Kind::Float: {
      const float *value = descriptor.field_ptr<float>(instance, field);
      if (value == nullptr) {
        return false;
      }

      writer.write_float(field.key, *value);
      break;
    }
    case core::TypeField::Kind::Uint32: {
      const std::uint32_t *value =
          descriptor.field_ptr<std::uint32_t>(instance, field);
      if (value == nullptr) {
        return false;
      }

      writer.write_uint(field.key, *value);
      break;
    }
    case core::TypeField::Kind::Bool: {
      const bool *value = descriptor.field_ptr<bool>(instance, field);
      if (value == nullptr) {
        return false;
      }

      writer.write_bool(field.key, *value);
      break;
    }
    case core::TypeField::Kind::Vec2: {
      const math::Vec2 *value =
          descriptor.field_ptr<math::Vec2>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_vec2(writer, field.key, *value);
      break;
    }
    case core::TypeField::Kind::Vec3: {
      const math::Vec3 *value =
          descriptor.field_ptr<math::Vec3>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_vec3(writer, field.key, *value);
      break;
    }
    case core::TypeField::Kind::Vec4: {
      const math::Vec4 *value =
          descriptor.field_ptr<math::Vec4>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_vec4(writer, field.key, *value);
      break;
    }
    case core::TypeField::Kind::Quat: {
      const math::Quat *value =
          descriptor.field_ptr<math::Quat>(instance, field);
      if (value == nullptr) {
        return false;
      }

      write_quat(writer, field.key, *value);
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
    if (field.key == nullptr) {
      continue;
    }

    core::JsonValue fieldValue{};
    if (!parser.get_object_field(componentObject, field.key, &fieldValue)) {
      // An optional field is written only while nonzero, so its absence
      // means zero whatever the instance held.
      if (field.omitWhenZero) {
        if ((field.offset > descriptor.size) ||
            (field.size > (descriptor.size - field.offset))) {
          return false;
        }
        std::memset(static_cast<unsigned char *>(instance) + field.offset, 0,
                    field.size);
      }
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
      if (value == nullptr) {
        return false;
      }
      if (!read_vec3(parser, fieldValue, value)) {
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
// off it.

void write_sky_light_component(core::JsonWriter &writer,
                               const SkyLightComponent &component) noexcept {
  writer.write_key(kJsonKeySkyLightComponent);
  writer.begin_object();
  content::write_asset_ref(writer, kEnvironmentRefField,
                           component.environmentRef);
  writer.end_object();
}

bool read_sky_light_component(const core::JsonParser &parser,
                              const core::JsonValue &value,
                              SkyLightComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (value.type != core::JsonValue::Type::Object)) {
    return false;
  }
  SkyLightComponent component{};
  core::JsonValue refValue{};
  if (parser.get_object_field(value, kEnvironmentRefField, &refValue) &&
      !content::read_asset_ref(parser, refValue, &component.environmentRef)) {
    return false;
  }
  *outComponent = component;
  return true;
}

void write_mesh_component(core::JsonWriter &writer,
                          const MeshComponent &component) noexcept {
  writer.write_key(kJsonKeyMeshComponent);
  writer.begin_object();
  content::write_asset_ref(writer, kMeshRefField, component.meshRef);
  content::write_asset_ref(writer, kMaterialRefField, component.materialRef);
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

  core::JsonValue refValue{};
  if (parser.get_object_field(meshObject, kMeshRefField, &refValue) &&
      !content::read_asset_ref(parser, refValue, &component.meshRef)) {
    return false;
  }
  if (parser.get_object_field(meshObject, kMaterialRefField, &refValue) &&
      !content::read_asset_ref(parser, refValue, &component.materialRef)) {
    return false;
  }

  core::JsonValue albedoValue{};
  if (parser.get_object_field(meshObject, "albedo", &albedoValue)) {
    if (!read_vec3(parser, albedoValue, &component.albedo)) {
      return false;
    }
  }

  if (!read_optional_float_strict(parser, meshObject, "roughness",
                                  &component.roughness) ||
      !read_optional_float_strict(parser, meshObject, "metallic",
                                  &component.metallic) ||
      !read_optional_float_strict(parser, meshObject, "opacity",
                                  &component.opacity)) {
    return false;
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

  if (!read_optional_float_strict(parser, lightObject, "intensity",
                                  &component.intensity)) {
    return false;
  }

  core::JsonValue typeValue{};
  std::uint32_t type = static_cast<std::uint32_t>(LightType::Directional);
  if (parser.get_object_field(lightObject, "type", &typeValue)) {
    if (!parser.as_uint(typeValue, &type)) {
      return false;
    }
  }
  // A type this build does not know (a newer build's light, a hand edit)
  // is refused by name: mapping it to Directional would light the level
  // with a sun and write that back as the light's type on the next save.
  if (!math::light_type_known(type)) {
    char message[160] = {};
    static_cast<void>(
        std::snprintf(message, sizeof(message),
                      "Light 'type' %u is not a light type this build knows (0 "
                      "Directional, 1 Point); refusing the load",
                      static_cast<unsigned int>(type)));
    core::log_message(core::LogLevel::Error, kSerializationLogChannel, message);
    return false;
  }
  component.type = static_cast<LightType>(type);

  *outComponent = component;
  return true;
}

// Hull payloads round-trip via HullSource provenance (rebuilt by
// World::add_collider on install); a TriMesh's triangles round-trip as the
// reference to the mesh asset they come from ("mesh", written only for a
// TriMesh, so every other collider's bytes are unchanged), which the
// collider mesh pass builds them from; Heightfield payloads are NOT serialized
// — they are reachable only from tests today, and terrain authoring is expected
// to bring its own asset-backed provenance before that changes.
bool write_collider_component(core::JsonWriter &writer,
                              const Collider &component) noexcept {
  const std::uint32_t shape = static_cast<std::uint32_t>(component.shape);
  if (shape > static_cast<std::uint32_t>(ColliderShape::TriMesh)) {
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
  // Written only for a trigger, so solid colliders saved before triggers
  // existed stay byte-identical with no schema version change.
  if (component.isTrigger) {
    writer.write_bool("isTrigger", true);
  }
  if (component.shape == ColliderShape::TriMesh) {
    content::write_asset_ref(writer, kMeshRefField, component.meshRef);
  }
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
        (shape > static_cast<std::uint32_t>(ColliderShape::TriMesh))) {
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
  if (parser.get_object_field(colliderObject, "isTrigger", &value) &&
      !parser.as_bool(value, &component.isTrigger)) {
    return false;
  }
  // Only a TriMesh names a mesh; a mesh on any other shape contradicts it.
  if (parser.get_object_field(colliderObject, kMeshRefField, &value) &&
      ((component.shape != ColliderShape::TriMesh) ||
       !content::read_asset_ref(parser, value, &component.meshRef))) {
    return false;
  }

  *outComponent = component;
  return true;
}

void write_tag_set_component(core::JsonWriter &writer, const char *key,
                             const TagSetComponent &component) noexcept {
  writer.begin_array(key);
  const std::size_t count = (component.count < TagSetComponent::kMaxTags)
                                ? component.count
                                : TagSetComponent::kMaxTags;
  for (std::size_t i = 0U; i < count; ++i) {
    writer.write_string_value(component.tags[i]);
  }
  writer.end_array();
}

bool read_tag_set_component(const core::JsonParser &parser,
                            const core::JsonValue &tagArray,
                            TagSetComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (tagArray.type != core::JsonValue::Type::Array)) {
    return false;
  }
  const std::size_t count = parser.array_size(tagArray);
  if (count > TagSetComponent::kMaxTags) {
    return false;
  }
  TagSetComponent component{};
  for (std::size_t i = 0U; i < count; ++i) {
    core::JsonValue element{};
    char tag[TagSetComponent::kMaxTagLength + 1U] = {};
    if (!parser.get_array_element(tagArray, i, &element) ||
        (element.type != core::JsonValue::Type::String) ||
        !parser.copy_string_strict(element, tag, sizeof(tag)) ||
        (tag_set_add(&component, tag) != TagSetAdd::Added)) {
      return false;
    }
  }
  *outComponent = component;
  return true;
}

void write_foliage_patch_component(
    core::JsonWriter &writer, const FoliagePatchComponent &component) noexcept {
  writer.write_key(kJsonKeyFoliagePatchComponent);
  writer.begin_object();

  // Every LOD slot is written, nil included, so a patch's LOD ordering
  // survives a round trip even when a middle slot names nothing.
  writer.begin_array(kFoliageMeshRefsField);
  for (std::size_t i = 0U; i < FoliagePatchComponent::kMaxLods; ++i) {
    // A slot naming nothing writes the empty string rather than being
    // skipped: the array is positional, so every slot has to occupy its
    // index. Formatting itself has no failing case on a buffer sized for
    // the longest form, so an empty slot here always means a nil
    // reference and never a dropped one.
    char text[content::kAssetRefTextLength + 1U] = {};
    if (core::asset_ref_is_valid(component.meshRefs[i])) {
      static_cast<void>(content::format_asset_ref(component.meshRefs[i], text,
                                                  sizeof(text)));
    }
    writer.write_string_value(text);
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

  if (parser.get_object_field(foliageObject, kFoliageMeshRefsField, &value) &&
      (value.type == core::JsonValue::Type::Array)) {
    // More LOD slots than the component can hold cannot round-trip: the
    // load is refused whole rather than dropping the authored tail.
    const std::size_t count = parser.array_size(value);
    if (count > FoliagePatchComponent::kMaxLods) {
      return false;
    }
    for (std::size_t i = 0U; i < count; ++i) {
      core::JsonValue element{};
      char text[content::kAssetRefTextLength + 1U] = {};
      if (!parser.get_array_element(value, i, &element) ||
          !parser.copy_string_strict(element, text, sizeof(text))) {
        return false;
      }
      // An empty slot is an authored "no LOD here", which the writer
      // emits for a nil reference; anything else must parse.
      if ((text[0] != '\0') &&
          !content::parse_asset_ref(text, &component.meshRefs[i])) {
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
  bool hasRequestedCount = false;
  if (parser.get_object_field(foliageObject, "instanceCount", &value)) {
    if (!parser.as_uint(value, &requestedCount)) {
      return false;
    }
    hasRequestedCount = true;
  }

  core::JsonValue instancesValue{};
  if (parser.get_object_field(foliageObject, "instances", &instancesValue) &&
      (instancesValue.type == core::JsonValue::Type::Array)) {
    const std::size_t count = parser.array_size(instancesValue);
    // Over-capacity or internally inconsistent instance data is refused
    // whole: entries beyond the fixed capacity cannot round-trip, and a
    // declared count that disagrees with the array in either direction
    // means the document does not describe one authored set — silently
    // normalizing would make whichever half is wrong permanent on the
    // next save. A document without the count field keeps loading by the
    // array alone (the count is derivable, so its absence is not a
    // conflict).
    if (count > FoliagePatchComponent::kMaxInstances) {
      return false;
    }
    if (hasRequestedCount &&
        (static_cast<std::size_t>(requestedCount) != count)) {
      return false;
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
    // No instances array: the declared count alone drives the patch, and
    // one beyond capacity is refused rather than clamped.
    if (requestedCount >
        static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)) {
      return false;
    }
    component.instanceCount = requestedCount;
  }

  *outComponent = component;
  return true;
}

void write_nav_mesh_surface_component(
    core::JsonWriter &writer,
    const NavMeshSurfaceComponent &component) noexcept {
  writer.write_key(kJsonKeyNavMeshSurfaceComponent);
  writer.begin_object();
  write_vec3(writer, kNavHalfExtentsField, component.halfExtents);
  writer.write_float(kNavCellSizeField, component.cellSize);
  writer.write_float(kNavAgentRadiusField, component.agentRadius);
  writer.write_float(kNavAgentHeightField, component.agentHeight);
  writer.write_float(kNavMaxClimbField, component.maxClimb);
  writer.write_float(kNavMaxSlopeField, component.maxSlopeDegrees);
  if (component.navMeshPath[0] != '\0') {
    writer.write_string(kNavMeshPathField, component.navMeshPath);
  }
  writer.end_object();
}

bool read_nav_mesh_surface_component(
    const core::JsonParser &parser, const core::JsonValue &value,
    NavMeshSurfaceComponent *outComponent) noexcept {
  if ((outComponent == nullptr) ||
      (value.type != core::JsonValue::Type::Object)) {
    return false;
  }
  NavMeshSurfaceComponent component{};
  core::JsonValue field{};
  if (parser.get_object_field(value, kNavHalfExtentsField, &field) &&
      !read_vec3(parser, field, &component.halfExtents)) {
    return false;
  }
  if (!read_optional_float_strict(parser, value, kNavCellSizeField,
                                  &component.cellSize) ||
      !read_optional_float_strict(parser, value, kNavAgentRadiusField,
                                  &component.agentRadius) ||
      !read_optional_float_strict(parser, value, kNavAgentHeightField,
                                  &component.agentHeight) ||
      !read_optional_float_strict(parser, value, kNavMaxClimbField,
                                  &component.maxClimb) ||
      !read_optional_float_strict(parser, value, kNavMaxSlopeField,
                                  &component.maxSlopeDegrees)) {
    return false;
  }
  // The path names the file a load reads, so one that does not fit whole
  // is refused rather than cut to the name of some other file.
  if (parser.get_object_field(value, kNavMeshPathField, &field) &&
      !parser.copy_string_strict(field, component.navMeshPath,
                                 sizeof(component.navMeshPath))) {
    return false;
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

namespace {

/// Keys earlier formats wrote that this build drops on purpose, matched
/// against the end of a member's path. ReflectionProbeComponent's
/// needsBake was a runtime dirty flag and brdfLutResolution a renderer
/// constant; neither is authored state, so old scenes carrying them load
/// silently.
constexpr const char *kRetiredKeyPaths[] = {
    "components.ReflectionProbeComponent.needsBake",
    "components.ReflectionProbeComponent.brdfLutResolution",
};

constexpr std::size_t kUnreadKeysLogged = 32U;

bool is_retired_key_path(const char *path) noexcept {
  const std::size_t pathLength = std::strlen(path);
  for (const char *retired : kRetiredKeyPaths) {
    const std::size_t retiredLength = std::strlen(retired);
    if ((pathLength >= retiredLength) &&
        (std::memcmp(path + (pathLength - retiredLength), retired,
                     retiredLength) == 0) &&
        ((pathLength == retiredLength) ||
         (path[pathLength - retiredLength - 1U] == '.'))) {
      return true;
    }
  }
  return false;
}

/// What report_unread_document_keys hands its visitor.
struct UnreadKeyReport final {
  const char *noun = nullptr;
  const char *documentPath = nullptr;
  const char *channel = nullptr;
  core::ValidationReport *report = nullptr;
  std::size_t reported = 0U;
};

void report_unread_key(const char *path, void *userData) noexcept {
  auto *state = static_cast<UnreadKeyReport *>(userData);
  if (is_retired_key_path(path)) {
    return;
  }
  ++state->reported;
  if (state->report != nullptr) {
    static_cast<void>(state->report->add(core::ValidationSeverity::Warning,
                                         "unknown_key", path, 0U));
  }
  if (state->reported <= kUnreadKeysLogged) {
    char message[512] = {};
    std::snprintf(message, sizeof(message),
                  "%s '%s': key '%s' is not read by this build and will be "
                  "lost on the next save",
                  state->noun, state->documentPath, path);
    core::log_message(core::LogLevel::Warning, state->channel, message);
  }
}

} // namespace

std::size_t report_unread_document_keys(
    const core::JsonValue &root, const core::JsonReadTracker &tracker,
    const char *noun, const char *documentPath, const char *channel,
    core::ValidationReport *report) noexcept {
  UnreadKeyReport state{};
  state.noun = (noun != nullptr) ? noun : "document";
  state.documentPath = (documentPath != nullptr) ? documentPath : "<memory>";
  state.channel = channel;
  state.report = report;
  if (!tracker.armed()) {
    char message[320] = {};
    std::snprintf(message, sizeof(message),
                  "%s '%s': unknown keys not checked (out of memory)",
                  state.noun, state.documentPath);
    core::log_message(core::LogLevel::Info, channel, message);
    return 0U;
  }
  static_cast<void>(core::json_visit_unread_members(
      root, tracker, &report_unread_key, &state));
  if (state.reported > kUnreadKeysLogged) {
    char message[320] = {};
    std::snprintf(message, sizeof(message),
                  "%s '%s': %zu keys are not read by this build; further keys "
                  "are not listed",
                  state.noun, state.documentPath, state.reported);
    core::log_message(core::LogLevel::Warning, channel, message);
  }
  return state.reported;
}

} // namespace engine::runtime
