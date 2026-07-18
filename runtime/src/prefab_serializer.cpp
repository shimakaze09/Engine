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
#include "serialization_util.h"

namespace engine::runtime {

namespace {

constexpr const char *kPrefabLogChannel = "prefab";
constexpr std::uint32_t kPrefabVersion = 1U;

// File IO and vec/quat/foliage JSON helpers are shared with the scene
// serializer via serialization_util.h.

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
    write_vec3(w, "position", transform.position);
    write_quat(w, "rotation", transform.rotation);
    write_vec3(w, "scale", transform.scale);
    w.end_object();
  }

  // RigidBody
  RigidBody rigidBody{};
  if (world.get_rigid_body(entity, &rigidBody)) {
    w.write_key(kJsonKeyRigidBody);
    w.begin_object();
    write_vec3(w, "velocity", rigidBody.velocity);
    write_vec3(w, "acceleration", rigidBody.acceleration);
    write_vec3(w, "angularVelocity", rigidBody.angularVelocity);
    w.write_float("inverseMass", rigidBody.inverseMass);
    w.write_float("inverseInertia", rigidBody.inverseInertia);
    w.end_object();
  }

  // Collider
  Collider collider{};
  if (world.get_collider(entity, &collider)) {
    w.write_key(kJsonKeyCollider);
    w.begin_object();
    write_vec3(w, "halfExtents", collider.halfExtents);
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
    // Written only when set so pre-material prefabs stay byte-identical.
    if (mesh.materialAssetId != 0ULL) {
      w.write_uint64("materialAssetId", mesh.materialAssetId);
    }
    write_vec3(w, "albedo", mesh.albedo);
    w.write_float("roughness", mesh.roughness);
    w.write_float("metallic", mesh.metallic);
    w.write_float("opacity", mesh.opacity);
    w.end_object();
  }

  // FoliagePatchComponent
  FoliagePatchComponent foliage{};
  if (world.get_foliage_patch_component(entity, &foliage)) {
    write_foliage_patch_component(w, foliage);
  }

  // LightComponent
  LightComponent light{};
  if (world.get_light_component(entity, &light)) {
    w.write_key(kJsonKeyLightComponent);
    w.begin_object();
    write_vec3(w, "color", light.color);
    write_vec3(w, "direction", light.direction);
    w.write_float("intensity", light.intensity);
    w.write_uint("type", static_cast<std::uint32_t>(light.type));
    w.end_object();
  }

  // PointLightComponent
  PointLightComponent pointLight{};
  if (world.get_point_light_component(entity, &pointLight)) {
    w.write_key("PointLightComponent");
    w.begin_object();
    write_vec3(w, "color", pointLight.color);
    w.write_float("intensity", pointLight.intensity);
    w.write_float("radius", pointLight.radius);
    w.end_object();
  }

  // SpotLightComponent
  SpotLightComponent spotLight{};
  if (world.get_spot_light_component(entity, &spotLight)) {
    w.write_key("SpotLightComponent");
    w.begin_object();
    write_vec3(w, "color", spotLight.color);
    write_vec3(w, "direction", spotLight.direction);
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
    write_vec3(w, "boxExtents", reflectionProbe.boxExtents);
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
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
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

Entity instantiate_prefab(World &world, const char *path) noexcept {
  if (path == nullptr) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "instantiate_prefab: null path");
    return kInvalidEntity;
  }

  std::unique_ptr<char[]> buf;
  std::size_t sz = 0U;
  if (!read_text_file(path, &buf, &sz)) {
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
        !readUint64Field(componentValue, "materialAssetId",
                         &mesh.materialAssetId) ||
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
    if (!read_foliage_patch_component(parser, componentValue, &foliage) ||
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
