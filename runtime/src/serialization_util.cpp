// Implements the shared serializer file-IO and JSON field helpers.
// Single source of truth for behavior the scene and prefab serializers used
// to duplicate (REVIEW_FINDINGS S5). Reads are strict: a field that is
// present but malformed fails the read instead of being silently skipped.

#include "serialization_util.h"

#include <new>

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

} // namespace engine::runtime
