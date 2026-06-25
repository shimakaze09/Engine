// Implements prefab serializer behavior for the Engine runtime world.

#include "engine/runtime/prefab_serializer.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/runtime/serialization_keys.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

constexpr const char *kPrefabLogChannel = "prefab";
constexpr std::uint32_t kPrefabVersion = 1U;
[[maybe_unused]] constexpr std::size_t kReadBufferInit = 64U * 1024U;

/// Handles prefab open write.
bool prefab_open_write(const char *path, FILE **outFile) noexcept {
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

/// Handles prefab read file.
bool prefab_read_file(const char *path, std::unique_ptr<char[]> *outBuf,
                      std::size_t *outSize) noexcept {
  if ((path == nullptr) || (outBuf == nullptr) || (outSize == nullptr)) {
    return false;
  }
  outBuf->reset();
  *outSize = 0U;

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long len = std::ftell(file);
  if (len <= 0L) {
    std::fclose(file);
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }

  const std::size_t sz = static_cast<std::size_t>(len);
  std::unique_ptr<char[]> buf(new (std::nothrow) char[sz + 1U]);
  if (buf == nullptr) {
    std::fclose(file);
    return false;
  }

  const std::size_t read = std::fread(buf.get(), 1U, sz, file);
  const bool err = std::ferror(file) != 0;
  std::fclose(file);
  if (err || (read != sz)) {
    return false;
  }

  buf[sz] = '\0';
  *outSize = sz;
  outBuf->swap(buf);
  return true;
}

/// Writes vec3 arr data.
void write_vec3_arr(core::JsonWriter &w, const char *key,
                    const math::Vec3 &v) noexcept {
  w.begin_array(key);
  w.write_float_value(v.x);
  w.write_float_value(v.y);
  w.write_float_value(v.z);
  w.end_array();
}

/// Writes quat arr data.
void write_quat_arr(core::JsonWriter &w, const char *key,
                    const math::Quat &q) noexcept {
  w.begin_array(key);
  w.write_float_value(q.x);
  w.write_float_value(q.y);
  w.write_float_value(q.z);
  w.write_float_value(q.w);
  w.end_array();
}

/// Reads vec3 data.
bool read_vec3(const core::JsonParser &p, const core::JsonValue &v,
               math::Vec3 *out) noexcept {
  if ((out == nullptr) || (v.type != core::JsonValue::Type::Array)) {
    return false;
  }
  float f[3] = {};
  for (std::size_t i = 0U; i < 3U; ++i) {
    core::JsonValue el{};
    if (!p.get_array_element(v, i, &el) || !p.as_float(el, &f[i])) {
      return false;
    }
  }
  *out = math::Vec3(f[0], f[1], f[2]);
  return true;
}

/// Reads quat data.
bool read_quat(const core::JsonParser &p, const core::JsonValue &v,
               math::Quat *out) noexcept {
  if ((out == nullptr) || (v.type != core::JsonValue::Type::Array)) {
    return false;
  }
  float f[4] = {};
  for (std::size_t i = 0U; i < 4U; ++i) {
    core::JsonValue el{};
    if (!p.get_array_element(v, i, &el) || !p.as_float(el, &f[i])) {
      return false;
    }
  }
  *out = math::Quat(f[0], f[1], f[2], f[3]);
  return true;
}

/// Writes foliage patch data.
void write_foliage_patch(core::JsonWriter &w,
                         const FoliagePatchComponent &foliage) noexcept {
  w.write_key(kJsonKeyFoliagePatchComponent);
  w.begin_object();

  w.begin_array("meshAssetIds");
  for (std::size_t i = 0U; i < FoliagePatchComponent::kMaxLods; ++i) {
    w.write_uint64_value(foliage.meshAssetIds[i]);
  }
  w.end_array();

  const std::uint32_t instanceCount =
      (foliage.instanceCount >
       static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances))
          ? static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)
          : foliage.instanceCount;
  w.write_uint("instanceCount", instanceCount);
  w.write_float("density", foliage.density);
  write_vec3_arr(w, "albedo", foliage.albedo);
  w.write_float("roughness", foliage.roughness);
  w.write_float("metallic", foliage.metallic);
  w.write_float("opacity", foliage.opacity);
  w.write_float("windStrength", foliage.windStrength);
  w.write_float("windFrequency", foliage.windFrequency);

  w.begin_array("instances");
  for (std::uint32_t i = 0U; i < instanceCount; ++i) {
    const FoliageInstance &instance = foliage.instances[i];
    w.begin_object();
    write_vec3_arr(w, "offset", instance.offset);
    w.write_float("scale", instance.scale);
    w.write_float("phase", instance.phase);
    w.write_uint("lodIndex", instance.lodIndex);
    w.end_object();
  }
  w.end_array();

  w.end_object();
}

/// Reads foliage patch data.
bool read_foliage_patch(const core::JsonParser &p, const core::JsonValue &v,
                        FoliagePatchComponent *out) noexcept {
  if ((out == nullptr) || (v.type != core::JsonValue::Type::Object)) {
    return false;
  }

  FoliagePatchComponent foliage{};
  core::JsonValue field{};
  if (p.get_object_field(v, "meshAssetIds", &field) &&
      (field.type == core::JsonValue::Type::Array)) {
    const std::size_t count = p.array_size(field);
    const std::size_t capped =
        (count < FoliagePatchComponent::kMaxLods)
            ? count
            : FoliagePatchComponent::kMaxLods;
    for (std::size_t i = 0U; i < capped; ++i) {
      core::JsonValue element{};
      if (!p.get_array_element(field, i, &element) ||
          !p.as_uint64(element, &foliage.meshAssetIds[i])) {
        return false;
      }
    }
  }

  if (p.get_object_field(v, "density", &field)) {
    if (!p.as_float(field, &foliage.density)) {
      return false;
    }
  }
  if (p.get_object_field(v, "albedo", &field)) {
    if (!read_vec3(p, field, &foliage.albedo)) {
      return false;
    }
  }
  if (p.get_object_field(v, "roughness", &field)) {
    if (!p.as_float(field, &foliage.roughness)) {
      return false;
    }
  }
  if (p.get_object_field(v, "metallic", &field)) {
    if (!p.as_float(field, &foliage.metallic)) {
      return false;
    }
  }
  if (p.get_object_field(v, "opacity", &field)) {
    if (!p.as_float(field, &foliage.opacity)) {
      return false;
    }
  }
  if (p.get_object_field(v, "windStrength", &field)) {
    if (!p.as_float(field, &foliage.windStrength)) {
      return false;
    }
  }
  if (p.get_object_field(v, "windFrequency", &field)) {
    if (!p.as_float(field, &foliage.windFrequency)) {
      return false;
    }
  }

  std::uint32_t requestedCount =
      static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances);
  if (p.get_object_field(v, "instanceCount", &field)) {
    if (!p.as_uint(field, &requestedCount)) {
      return false;
    }
  }

  core::JsonValue instances{};
  if (p.get_object_field(v, "instances", &instances) &&
      (instances.type == core::JsonValue::Type::Array)) {
    std::size_t count = p.array_size(instances);
    if (count > FoliagePatchComponent::kMaxInstances) {
      count = FoliagePatchComponent::kMaxInstances;
    }
    if (count > requestedCount) {
      count = requestedCount;
    }

    for (std::size_t i = 0U; i < count; ++i) {
      core::JsonValue instanceValue{};
      if (!p.get_array_element(instances, i, &instanceValue) ||
          (instanceValue.type != core::JsonValue::Type::Object)) {
        return false;
      }

      FoliageInstance instance{};
      if (p.get_object_field(instanceValue, "offset", &field)) {
        if (!read_vec3(p, field, &instance.offset)) {
          return false;
        }
      }
      if (p.get_object_field(instanceValue, "scale", &field)) {
        if (!p.as_float(field, &instance.scale)) {
          return false;
        }
      }
      if (p.get_object_field(instanceValue, "phase", &field)) {
        if (!p.as_float(field, &instance.phase)) {
          return false;
        }
      }
      if (p.get_object_field(instanceValue, "lodIndex", &field)) {
        if (!p.as_uint(field, &instance.lodIndex)) {
          return false;
        }
      }
      foliage.instances[i] = instance;
    }
    foliage.instanceCount = static_cast<std::uint32_t>(count);
  } else {
    if (requestedCount >
        static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)) {
      requestedCount =
          static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances);
    }
    foliage.instanceCount = requestedCount;
  }

  *out = foliage;
  return true;
}

} // namespace

/// Saves the requested resource for prefab.
bool save_prefab(const World &world, Entity entity, const char *path) noexcept {
  if (!world.is_alive(entity) || (path == nullptr)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: invalid entity or null path");
    return false;
  }

  core::JsonWriter w{};
  w.begin_object();
  w.write_uint("version", kPrefabVersion);
  w.write_key("components");
  w.begin_object();

  // Transform
  Transform transform{};
  if (world.get_transform(entity, &transform)) {
    w.write_key(kJsonKeyTransform);
    w.begin_object();
    write_vec3_arr(w, "position", transform.position);
    write_quat_arr(w, "rotation", transform.rotation);
    write_vec3_arr(w, "scale", transform.scale);
    w.end_object();
  }

  // RigidBody
  RigidBody rigidBody{};
  if (world.get_rigid_body(entity, &rigidBody)) {
    w.write_key(kJsonKeyRigidBody);
    w.begin_object();
    write_vec3_arr(w, "velocity", rigidBody.velocity);
    write_vec3_arr(w, "acceleration", rigidBody.acceleration);
    write_vec3_arr(w, "angularVelocity", rigidBody.angularVelocity);
    w.write_float("inverseMass", rigidBody.inverseMass);
    w.write_float("inverseInertia", rigidBody.inverseInertia);
    w.end_object();
  }

  // Collider
  Collider collider{};
  if (world.get_collider(entity, &collider)) {
    w.write_key(kJsonKeyCollider);
    w.begin_object();
    write_vec3_arr(w, "halfExtents", collider.halfExtents);
    w.write_float("restitution", collider.restitution);
    w.write_float("staticFriction", collider.staticFriction);
    w.write_float("dynamicFriction", collider.dynamicFriction);
    w.write_float("density", collider.density);
    w.write_uint("collisionLayer", collider.collisionLayer);
    w.write_uint("collisionMask", collider.collisionMask);
    w.end_object();
  }

  // NameComponent
  NameComponent nameComp{};
  if (world.get_name_component(entity, &nameComp)) {
    w.write_key(kJsonKeyNameComponent);
    w.begin_object();
    w.write_string("name", nameComp.name);
    w.end_object();
  }

  // MeshComponent
  MeshComponent mesh{};
  if (world.get_mesh_component(entity, &mesh)) {
    w.write_key("MeshComponent");
    w.begin_object();
    w.write_uint64("meshAssetId", mesh.meshAssetId);
    write_vec3_arr(w, "albedo", mesh.albedo);
    w.write_float("roughness", mesh.roughness);
    w.write_float("metallic", mesh.metallic);
    w.write_float("opacity", mesh.opacity);
    w.end_object();
  }

  // FoliagePatchComponent
  FoliagePatchComponent foliage{};
  if (world.get_foliage_patch_component(entity, &foliage)) {
    write_foliage_patch(w, foliage);
  }

  // LightComponent
  LightComponent light{};
  if (world.get_light_component(entity, &light)) {
    w.write_key(kJsonKeyLightComponent);
    w.begin_object();
    write_vec3_arr(w, "color", light.color);
    write_vec3_arr(w, "direction", light.direction);
    w.write_float("intensity", light.intensity);
    w.write_uint("type", static_cast<std::uint32_t>(light.type));
    w.end_object();
  }

  // PointLightComponent
  PointLightComponent pointLight{};
  if (world.get_point_light_component(entity, &pointLight)) {
    w.write_key("PointLightComponent");
    w.begin_object();
    write_vec3_arr(w, "color", pointLight.color);
    w.write_float("intensity", pointLight.intensity);
    w.write_float("radius", pointLight.radius);
    w.end_object();
  }

  // SpotLightComponent
  SpotLightComponent spotLight{};
  if (world.get_spot_light_component(entity, &spotLight)) {
    w.write_key("SpotLightComponent");
    w.begin_object();
    write_vec3_arr(w, "color", spotLight.color);
    write_vec3_arr(w, "direction", spotLight.direction);
    w.write_float("intensity", spotLight.intensity);
    w.write_float("radius", spotLight.radius);
    w.write_float("innerConeAngle", spotLight.innerConeAngle);
    w.write_float("outerConeAngle", spotLight.outerConeAngle);
    w.end_object();
  }

  // ReflectionProbeComponent
  ReflectionProbeComponent reflectionProbe{};
  if (world.get_reflection_probe_component(entity, &reflectionProbe)) {
    w.write_key(kJsonKeyReflectionProbeComponent);
    w.begin_object();
    write_vec3_arr(w, "boxExtents", reflectionProbe.boxExtents);
    w.write_float("radius", reflectionProbe.radius);
    w.write_float("intensity", reflectionProbe.intensity);
    w.write_uint("prefilteredResolution",
                 reflectionProbe.prefilteredResolution);
    w.write_uint("irradianceResolution",
                 reflectionProbe.irradianceResolution);
    w.write_uint("brdfLutResolution", reflectionProbe.brdfLutResolution);
    w.write_uint("mipLevels", reflectionProbe.mipLevels);
    w.write_bool("boxProjection", reflectionProbe.boxProjection);
    w.write_bool("needsBake", reflectionProbe.needsBake);
    w.end_object();
  }

  // ScriptComponent— plain string format (matches scene serializer).
  ScriptComponent scriptComp{};
  if (world.get_script_component(entity, &scriptComp) &&
      (scriptComp.scriptPath[0] != '\0')) {
    w.write_string(kJsonKeyScriptComponent, scriptComp.scriptPath);
  }

  w.end_object(); // components
  w.end_object(); // root

  if (w.failed()) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: JSON serialization failed");
    return false;
  }

  FILE *file = nullptr;
  if (!prefab_open_write(path, &file) || (file == nullptr)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to open file for writing");
    return false;
  }

  const std::size_t sz = w.result_size();
  const std::size_t written = std::fwrite(w.result(), 1U, sz, file);
  std::fclose(file);
  if (written != sz) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: file write incomplete");
    return false;
  }

  return true;
}

/// Handles instantiate prefab.
Entity instantiate_prefab(World &world, const char *path) noexcept {
  if (path == nullptr) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: null path");
    return kInvalidEntity;
  }

  std::unique_ptr<char[]> buf;
  std::size_t sz = 0U;
  if (!prefab_read_file(path, &buf, &sz)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: failed to read file");
    return kInvalidEntity;
  }

  core::JsonParser parser{};
  if (!parser.parse(buf.get(), sz)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: JSON parse error");
    return kInvalidEntity;
  }
  const core::JsonValue *rootPtr = parser.root();
  if ((rootPtr == nullptr) ||
      (rootPtr->type != core::JsonValue::Type::Object)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: JSON root is not an object");
    return kInvalidEntity;
  }
  const core::JsonValue root = *rootPtr;

  core::JsonValue componentsVal{};
  if (!parser.get_object_field(root, "components", &componentsVal) ||
      (componentsVal.type != core::JsonValue::Type::Object)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: missing 'components' key");
    return kInvalidEntity;
  }

  const Entity entity = world.create_entity();
  if (entity == kInvalidEntity) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: create_entity failed");
    return kInvalidEntity;
  }

  auto failComponent = [&](const char *message) noexcept -> Entity {
    world.destroy_entity(entity);
    core::log_message(core::LogLevel::Error, kPrefabLogChannel, message);
    return kInvalidEntity;
  };

  auto readComponentObject = [&](const char *key, core::JsonValue *outValue,
                                 bool *outPresent) noexcept -> bool {
    if ((key == nullptr) || (outValue == nullptr) || (outPresent == nullptr)) {
      return false;
    }
    *outPresent = false;
    core::JsonValue value{};
    if (!parser.get_object_field(componentsVal, key, &value)) {
      return true;
    }
    if (value.type != core::JsonValue::Type::Object) {
      return false;
    }
    *outPresent = true;
    *outValue = value;
    return true;
  };

  auto readVec3Field = [&](const core::JsonValue &object, const char *key,
                           math::Vec3 *out) noexcept -> bool {
    core::JsonValue value{};
    return !parser.get_object_field(object, key, &value) ||
           read_vec3(parser, value, out);
  };
  auto readQuatField = [&](const core::JsonValue &object, const char *key,
                           math::Quat *out) noexcept -> bool {
    core::JsonValue value{};
    return !parser.get_object_field(object, key, &value) ||
           read_quat(parser, value, out);
  };
  auto readFloatField = [&](const core::JsonValue &object, const char *key,
                            float *out) noexcept -> bool {
    core::JsonValue value{};
    return !parser.get_object_field(object, key, &value) ||
           parser.as_float(value, out);
  };
  auto readUintField = [&](const core::JsonValue &object, const char *key,
                           std::uint32_t *out) noexcept -> bool {
    core::JsonValue value{};
    return !parser.get_object_field(object, key, &value) ||
           parser.as_uint(value, out);
  };
  auto readUint64Field = [&](const core::JsonValue &object, const char *key,
                             std::uint64_t *out) noexcept -> bool {
    core::JsonValue value{};
    return !parser.get_object_field(object, key, &value) ||
           parser.as_uint64(value, out);
  };
  auto readBoolField = [&](const core::JsonValue &object, const char *key,
                           bool *out) noexcept -> bool {
    core::JsonValue value{};
    return !parser.get_object_field(object, key, &value) ||
           parser.as_bool(value, out);
  };

  bool hasComponent = false;
  core::JsonValue componentValue{};

  if (!readComponentObject(kJsonKeyTransform, &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid Transform component");
  }
  if (hasComponent) {
    Transform transform{};
    if (!readVec3Field(componentValue, "position", &transform.position) ||
        !readQuatField(componentValue, "rotation", &transform.rotation) ||
        !readVec3Field(componentValue, "scale", &transform.scale) ||
        !world.add_transform(entity, transform)) {
      return failComponent("instantiate_prefab: failed to add Transform");
    }
  }

  if (!readComponentObject(kJsonKeyRigidBody, &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid RigidBody component");
  }
  if (hasComponent) {
    RigidBody rigidBody{};
    if (!readVec3Field(componentValue, "velocity", &rigidBody.velocity) ||
        !readVec3Field(componentValue, "acceleration",
                       &rigidBody.acceleration) ||
        !readVec3Field(componentValue, "angularVelocity",
                       &rigidBody.angularVelocity) ||
        !readFloatField(componentValue, "inverseMass",
                        &rigidBody.inverseMass) ||
        !readFloatField(componentValue, "inverseInertia",
                        &rigidBody.inverseInertia) ||
        !world.add_rigid_body(entity, rigidBody)) {
      return failComponent("instantiate_prefab: failed to add RigidBody");
    }
  }

  if (!readComponentObject(kJsonKeyCollider, &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid Collider component");
  }
  if (hasComponent) {
    Collider collider{};
    if (!readVec3Field(componentValue, "halfExtents",
                       &collider.halfExtents) ||
        !readFloatField(componentValue, "restitution",
                        &collider.restitution) ||
        !readFloatField(componentValue, "staticFriction",
                        &collider.staticFriction) ||
        !readFloatField(componentValue, "dynamicFriction",
                        &collider.dynamicFriction) ||
        !readFloatField(componentValue, "density", &collider.density) ||
        !readUintField(componentValue, "collisionLayer",
                       &collider.collisionLayer) ||
        !readUintField(componentValue, "collisionMask",
                       &collider.collisionMask) ||
        !world.add_collider(entity, collider)) {
      return failComponent("instantiate_prefab: failed to add Collider");
    }
  }

  if (!readComponentObject(kJsonKeyNameComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid NameComponent");
  }
  if (hasComponent) {
    NameComponent nameComponent{};
    core::JsonValue nameValue{};
    if (parser.get_object_field(componentValue, "name", &nameValue)) {
      if (!parser.copy_string(nameValue, nameComponent.name,
                              sizeof(nameComponent.name))) {
        return failComponent("instantiate_prefab: invalid NameComponent name");
      }
    }
    if (!world.add_name_component(entity, nameComponent)) {
      return failComponent("instantiate_prefab: failed to add NameComponent");
    }
  }

  if (!readComponentObject("MeshComponent", &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid MeshComponent");
  }
  if (hasComponent) {
    MeshComponent mesh{};
    if (!readUint64Field(componentValue, "meshAssetId", &mesh.meshAssetId) ||
        !readVec3Field(componentValue, "albedo", &mesh.albedo) ||
        !readFloatField(componentValue, "roughness", &mesh.roughness) ||
        !readFloatField(componentValue, "metallic", &mesh.metallic) ||
        !readFloatField(componentValue, "opacity", &mesh.opacity) ||
        !world.add_mesh_component(entity, mesh)) {
      return failComponent("instantiate_prefab: failed to add MeshComponent");
    }
  }

  if (!readComponentObject(kJsonKeyFoliagePatchComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid FoliagePatchComponent");
  }
  if (hasComponent) {
    FoliagePatchComponent foliage{};
    if (!read_foliage_patch(parser, componentValue, &foliage) ||
        !world.add_foliage_patch_component(entity, foliage)) {
      return failComponent(
          "instantiate_prefab: failed to add FoliagePatchComponent");
    }
  }

  if (!readComponentObject(kJsonKeyLightComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid LightComponent");
  }
  if (hasComponent) {
    LightComponent light{};
    std::uint32_t typeVal = 0U;
    if (!readVec3Field(componentValue, "color", &light.color) ||
        !readVec3Field(componentValue, "direction", &light.direction) ||
        !readFloatField(componentValue, "intensity", &light.intensity) ||
        !readUintField(componentValue, "type", &typeVal)) {
      return failComponent("instantiate_prefab: failed to add LightComponent");
    }
    light.type = (typeVal == 1U) ? LightType::Point : LightType::Directional;
    if (!world.add_light_component(entity, light)) {
      return failComponent("instantiate_prefab: failed to add LightComponent");
    }
  }

  if (!readComponentObject("PointLightComponent", &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid PointLightComponent");
  }
  if (hasComponent) {
    PointLightComponent pointLight{};
    if (!readVec3Field(componentValue, "color", &pointLight.color) ||
        !readFloatField(componentValue, "intensity", &pointLight.intensity) ||
        !readFloatField(componentValue, "radius", &pointLight.radius) ||
        !world.add_point_light_component(entity, pointLight)) {
      return failComponent(
          "instantiate_prefab: failed to add PointLightComponent");
    }
  }

  if (!readComponentObject("SpotLightComponent", &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid SpotLightComponent");
  }
  if (hasComponent) {
    SpotLightComponent spotLight{};
    if (!readVec3Field(componentValue, "color", &spotLight.color) ||
        !readVec3Field(componentValue, "direction", &spotLight.direction) ||
        !readFloatField(componentValue, "intensity", &spotLight.intensity) ||
        !readFloatField(componentValue, "radius", &spotLight.radius) ||
        !readFloatField(componentValue, "innerConeAngle",
                        &spotLight.innerConeAngle) ||
        !readFloatField(componentValue, "outerConeAngle",
                        &spotLight.outerConeAngle) ||
        !world.add_spot_light_component(entity, spotLight)) {
      return failComponent(
          "instantiate_prefab: failed to add SpotLightComponent");
    }
  }

  if (!readComponentObject(kJsonKeyReflectionProbeComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid ReflectionProbeComponent");
  }
  if (hasComponent) {
    ReflectionProbeComponent reflectionProbe{};
    if (!readVec3Field(componentValue, "boxExtents",
                       &reflectionProbe.boxExtents) ||
        !readFloatField(componentValue, "radius", &reflectionProbe.radius) ||
        !readFloatField(componentValue, "intensity",
                        &reflectionProbe.intensity) ||
        !readUintField(componentValue, "prefilteredResolution",
                       &reflectionProbe.prefilteredResolution) ||
        !readUintField(componentValue, "irradianceResolution",
                       &reflectionProbe.irradianceResolution) ||
        !readUintField(componentValue, "brdfLutResolution",
                       &reflectionProbe.brdfLutResolution) ||
        !readUintField(componentValue, "mipLevels",
                       &reflectionProbe.mipLevels) ||
        !readBoolField(componentValue, "boxProjection",
                       &reflectionProbe.boxProjection) ||
        !readBoolField(componentValue, "needsBake",
                       &reflectionProbe.needsBake) ||
        !world.add_reflection_probe_component(entity, reflectionProbe)) {
      return failComponent(
          "instantiate_prefab: failed to add ReflectionProbeComponent");
    }
  }

  core::JsonValue scriptValue{};
  if (parser.get_object_field(componentsVal, kJsonKeyScriptComponent,
                              &scriptValue)) {
    ScriptComponent script{};
    bool gotPath = false;

    if (scriptValue.type == core::JsonValue::Type::String) {
      gotPath = parser.copy_string(scriptValue, script.scriptPath,
                                   sizeof(script.scriptPath));
    } else if (scriptValue.type == core::JsonValue::Type::Object) {
      core::JsonValue pathValue{};
      if (parser.get_object_field(scriptValue, "scriptPath", &pathValue)) {
        gotPath = parser.copy_string(pathValue, script.scriptPath,
                                     sizeof(script.scriptPath));
      }
    } else {
      return failComponent("instantiate_prefab: invalid ScriptComponent");
    }

    if (!gotPath || (script.scriptPath[0] == '\0')) {
      return failComponent("instantiate_prefab: invalid ScriptComponent path");
    }

    if (!world.add_script_component(entity, script)) {
      return failComponent("instantiate_prefab: failed to add ScriptComponent");
    }
  }

  return entity;
}

} // namespace engine::runtime
