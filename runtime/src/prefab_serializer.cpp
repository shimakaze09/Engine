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

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kPrefabLogChannel)) {
    return false;
  }

  core::JsonWriter w{};
  w.begin_object();
  w.write_uint("version", kPrefabVersion);
  w.write_key("components");
  w.begin_object();

  Transform transform{};
  if (world.get_transform(entity, &transform) &&
      !write_reflected_component(w, kJsonKeyTransform, *descs.transform,
                                 &transform)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write Transform");
    return false;
  }

  RigidBody rigidBody{};
  if (world.get_rigid_body(entity, &rigidBody) &&
      !write_reflected_component(w, kJsonKeyRigidBody, *descs.rigidBody,
                                 &rigidBody)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write RigidBody");
    return false;
  }

  Collider collider{};
  if (world.get_collider(entity, &collider)) {
    if (!write_collider_component(w, collider)) {
      core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                        "save_prefab: invalid Collider component");
      return false;
    }
  }

  NameComponent nameComp{};
  if (world.get_name_component(entity, &nameComp)) {
    w.write_key(kJsonKeyNameComponent);
    w.begin_object();
    w.write_string("name", nameComp.name);
    w.end_object();
  }

  MeshComponent mesh{};
  if (world.get_mesh_component(entity, &mesh)) {
    write_mesh_component(w, mesh);
  }

  FoliagePatchComponent foliage{};
  if (world.get_foliage_patch_component(entity, &foliage)) {
    write_foliage_patch_component(w, foliage);
  }

  LightComponent light{};
  if (world.get_light_component(entity, &light)) {
    write_light_component(w, light);
  }

  PointLightComponent pointLight{};
  if (world.get_point_light_component(entity, &pointLight) &&
      !write_reflected_component(w, kJsonKeyPointLightComponent,
                                 *descs.pointLight, &pointLight)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write PointLightComponent");
    return false;
  }

  SpotLightComponent spotLight{};
  if (world.get_spot_light_component(entity, &spotLight) &&
      !write_reflected_component(w, kJsonKeySpotLightComponent,
                                 *descs.spotLight, &spotLight)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write SpotLightComponent");
    return false;
  }

  SpringArmComponent springArm{};
  if (world.get_spring_arm(entity, &springArm) &&
      !write_reflected_component(w, kJsonKeySpringArmComponent,
                                 *descs.springArm, &springArm)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write SpringArmComponent");
    return false;
  }

  ReflectionProbeComponent reflectionProbe{};
  if (world.get_reflection_probe_component(entity, &reflectionProbe) &&
      !write_reflected_component(w, kJsonKeyReflectionProbeComponent,
                                 *descs.reflectionProbe, &reflectionProbe)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write ReflectionProbeComponent");
    return false;
  }

  SceneCaptureComponent sceneCapture{};
  if (world.get_scene_capture_component(entity, &sceneCapture) &&
      !write_reflected_component(w, kJsonKeySceneCaptureComponent,
                                 *descs.sceneCapture, &sceneCapture)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write SceneCaptureComponent");
    return false;
  }

  CameraComponent camera{};
  if (world.get_camera_component(entity, &camera) &&
      !write_reflected_component(w, kJsonKeyCameraComponent, *descs.camera,
                                 &camera)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write CameraComponent");
    return false;
  }

  ScriptComponent scriptComp{};
  if (world.get_script_component(entity, &scriptComp) &&
      (scriptComp.scriptPath[0] != '\0')) {
    w.write_string(kJsonKeyScriptComponent, scriptComp.scriptPath);
  }

  AnimationComponent animationComp{};
  if (world.get_animation_component(entity, &animationComp) &&
      (animationComp.controllerPath[0] != '\0')) {
    w.write_string(kJsonKeyAnimationComponent, animationComp.controllerPath);
  }

  w.end_object();
  w.end_object();

  if (w.failed()) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: JSON serialization failed");
    return false;
  }

  if (!write_text_file(path, w.result(), w.result_size())) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab: failed to write file");
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

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kPrefabLogChannel)) {
    return kInvalidEntity;
  }

  const Entity entity = world.create_scene_object();
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

  bool hasComponent = false;
  core::JsonValue componentValue{};

  if (!readComponentObject(kJsonKeyTransform, &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid Transform component");
  }
  if (hasComponent) {
    Transform transform{};
    if (!read_reflected_component(parser, componentValue, *descs.transform,
                                  &transform) ||
        !world.add_transform(entity, transform)) {
      return failComponent("instantiate_prefab: failed to add Transform");
    }
  }

  if (!readComponentObject(kJsonKeyRigidBody, &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid RigidBody component");
  }
  if (hasComponent) {
    RigidBody rigidBody{};
    if (!read_reflected_component(parser, componentValue, *descs.rigidBody,
                                  &rigidBody) ||
        !world.add_rigid_body(entity, rigidBody)) {
      return failComponent("instantiate_prefab: failed to add RigidBody");
    }
  }

  if (!readComponentObject(kJsonKeyCollider, &componentValue, &hasComponent)) {
    return failComponent("instantiate_prefab: invalid Collider component");
  }
  if (hasComponent) {
    Collider collider{};
    if (!read_collider_component(parser, componentValue, &collider) ||
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

  if (!readComponentObject(kJsonKeyMeshComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid MeshComponent");
  }
  if (hasComponent) {
    MeshComponent mesh{};
    if (!read_mesh_component(parser, componentValue, &mesh) ||
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
    if (!read_light_component(parser, componentValue, &light) ||
        !world.add_light_component(entity, light)) {
      return failComponent("instantiate_prefab: failed to add LightComponent");
    }
  }

  if (!readComponentObject(kJsonKeyPointLightComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid PointLightComponent");
  }
  if (hasComponent) {
    PointLightComponent pointLight{};
    if (!read_reflected_component(parser, componentValue, *descs.pointLight,
                                  &pointLight) ||
        !world.add_point_light_component(entity, pointLight)) {
      return failComponent(
          "instantiate_prefab: failed to add PointLightComponent");
    }
  }

  if (!readComponentObject(kJsonKeySpotLightComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid SpotLightComponent");
  }
  if (hasComponent) {
    SpotLightComponent spotLight{};
    if (!read_reflected_component(parser, componentValue, *descs.spotLight,
                                  &spotLight) ||
        !world.add_spot_light_component(entity, spotLight)) {
      return failComponent(
          "instantiate_prefab: failed to add SpotLightComponent");
    }
  }

  if (!readComponentObject(kJsonKeySpringArmComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid SpringArmComponent");
  }
  if (hasComponent) {
    SpringArmComponent springArm{};
    if (!read_reflected_component(parser, componentValue, *descs.springArm,
                                  &springArm) ||
        !world.add_spring_arm(entity, springArm)) {
      return failComponent(
          "instantiate_prefab: failed to add SpringArmComponent");
    }
  }

  if (!readComponentObject(kJsonKeyReflectionProbeComponent, &componentValue,
                           &hasComponent)) {
    return failComponent(
        "instantiate_prefab: invalid ReflectionProbeComponent");
  }
  if (hasComponent) {
    ReflectionProbeComponent reflectionProbe{};
    if (!read_reflected_component(parser, componentValue,
                                  *descs.reflectionProbe, &reflectionProbe) ||
        !world.add_reflection_probe_component(entity, reflectionProbe)) {
      return failComponent(
          "instantiate_prefab: failed to add ReflectionProbeComponent");
    }
  }

  if (!readComponentObject(kJsonKeySceneCaptureComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid SceneCaptureComponent");
  }
  if (hasComponent) {
    SceneCaptureComponent sceneCapture{};
    if (!read_reflected_component(parser, componentValue, *descs.sceneCapture,
                                  &sceneCapture) ||
        !world.add_scene_capture_component(entity, sceneCapture)) {
      return failComponent(
          "instantiate_prefab: failed to add SceneCaptureComponent");
    }
  }

  if (!readComponentObject(kJsonKeyCameraComponent, &componentValue,
                           &hasComponent)) {
    return failComponent("instantiate_prefab: invalid CameraComponent");
  }
  if (hasComponent) {
    CameraComponent camera{};
    if (!read_reflected_component(parser, componentValue, *descs.camera,
                                  &camera) ||
        !world.add_camera_component(entity, camera)) {
      return failComponent("instantiate_prefab: failed to add CameraComponent");
    }
  }

  core::JsonValue scriptValue{};
  if (parser.get_object_field(componentsVal, kJsonKeyScriptComponent,
                              &scriptValue)) {
    ScriptComponent script{};
    bool gotPath = false;

    if (scriptValue.type == core::JsonValue::Type::String) {
      gotPath = parser.copy_string_strict(scriptValue, script.scriptPath,
                                          sizeof(script.scriptPath));
    } else if (scriptValue.type == core::JsonValue::Type::Object) {
      core::JsonValue pathValue{};
      if (parser.get_object_field(scriptValue, "scriptPath", &pathValue)) {
        gotPath = parser.copy_string_strict(pathValue, script.scriptPath,
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

  core::JsonValue animationValue{};
  if (parser.get_object_field(componentsVal, kJsonKeyAnimationComponent,
                              &animationValue)) {
    AnimationComponent animation{};
    if ((animationValue.type != core::JsonValue::Type::String) ||
        !parser.copy_string_strict(animationValue, animation.controllerPath,
                                   sizeof(animation.controllerPath)) ||
        (animation.controllerPath[0] == '\0')) {
      return failComponent("instantiate_prefab: invalid AnimationComponent");
    }

    if (!world.add_animation_component(entity, animation)) {
      return failComponent(
          "instantiate_prefab: failed to add AnimationComponent");
    }
  }

  return entity;
}

} // namespace engine::runtime
