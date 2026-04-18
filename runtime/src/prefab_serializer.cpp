#include "engine/runtime/prefab_serializer.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

constexpr const char *kPrefabLogChannel = "prefab";
constexpr std::uint32_t kPrefabVersion = 1U;
[[maybe_unused]] constexpr std::size_t kReadBufferInit = 64U * 1024U;

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

void write_vec3_arr(core::JsonWriter &w, const char *key,
                    const math::Vec3 &v) noexcept {
  w.begin_array(key);
  w.write_float_value(v.x);
  w.write_float_value(v.y);
  w.write_float_value(v.z);
  w.end_array();
}

void write_quat_arr(core::JsonWriter &w, const char *key,
                    const math::Quat &q) noexcept {
  w.begin_array(key);
  w.write_float_value(q.x);
  w.write_float_value(q.y);
  w.write_float_value(q.z);
  w.write_float_value(q.w);
  w.end_array();
}

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

} // namespace

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
    w.write_key("Transform");
    w.begin_object();
    write_vec3_arr(w, "position", transform.position);
    write_quat_arr(w, "rotation", transform.rotation);
    write_vec3_arr(w, "scale", transform.scale);
    w.end_object();
  }

  // RigidBody
  RigidBody rigidBody{};
  if (world.get_rigid_body(entity, &rigidBody)) {
    w.write_key("RigidBody");
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
    w.write_key("Collider");
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
    w.write_key("NameComponent");
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

  // LightComponent
  LightComponent light{};
  if (world.get_light_component(entity, &light)) {
    w.write_key("LightComponent");
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

  // ScriptComponent— plain string format (matches scene serializer).
  ScriptComponent scriptComp{};
  if (world.get_script_component(entity, &scriptComp) &&
      (scriptComp.scriptPath[0] != '\0')) {
    w.write_string("ScriptComponent", scriptComp.scriptPath);
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

  // Transform
  core::JsonValue tval{};
  if (parser.get_object_field(componentsVal, "Transform", &tval) &&
      (tval.type == core::JsonValue::Type::Object)) {
    Transform t{};
    core::JsonValue v{};
    if (parser.get_object_field(tval, "position", &v)) {
      static_cast<void>(read_vec3(parser, v, &t.position));
    }
    if (parser.get_object_field(tval, "rotation", &v)) {
      static_cast<void>(read_quat(parser, v, &t.rotation));
    }
    if (parser.get_object_field(tval, "scale", &v)) {
      static_cast<void>(read_vec3(parser, v, &t.scale));
    }
    static_cast<void>(world.add_transform(entity, t));
  }

  // RigidBody
  core::JsonValue rbval{};
  if (parser.get_object_field(componentsVal, "RigidBody", &rbval) &&
      (rbval.type == core::JsonValue::Type::Object)) {
    RigidBody rb{};
    core::JsonValue v{};
    if (parser.get_object_field(rbval, "velocity", &v)) {
      static_cast<void>(read_vec3(parser, v, &rb.velocity));
    }
    if (parser.get_object_field(rbval, "acceleration", &v)) {
      static_cast<void>(read_vec3(parser, v, &rb.acceleration));
    }
    if (parser.get_object_field(rbval, "angularVelocity", &v)) {
      static_cast<void>(read_vec3(parser, v, &rb.angularVelocity));
    }
    if (parser.get_object_field(rbval, "inverseMass", &v)) {
      static_cast<void>(parser.as_float(v, &rb.inverseMass));
    }
    if (parser.get_object_field(rbval, "inverseInertia", &v)) {
      static_cast<void>(parser.as_float(v, &rb.inverseInertia));
    }
    static_cast<void>(world.add_rigid_body(entity, rb));
  }

  // Collider
  core::JsonValue cval{};
  if (parser.get_object_field(componentsVal, "Collider", &cval) &&
      (cval.type == core::JsonValue::Type::Object)) {
    Collider col{};
    core::JsonValue v{};
    if (parser.get_object_field(cval, "halfExtents", &v)) {
      static_cast<void>(read_vec3(parser, v, &col.halfExtents));
    }
    if (parser.get_object_field(cval, "restitution", &v)) {
      static_cast<void>(parser.as_float(v, &col.restitution));
    }
    if (parser.get_object_field(cval, "staticFriction", &v)) {
      static_cast<void>(parser.as_float(v, &col.staticFriction));
    }
    if (parser.get_object_field(cval, "dynamicFriction", &v)) {
      static_cast<void>(parser.as_float(v, &col.dynamicFriction));
    }
    if (parser.get_object_field(cval, "density", &v)) {
      static_cast<void>(parser.as_float(v, &col.density));
    }
    if (parser.get_object_field(cval, "collisionLayer", &v)) {
      static_cast<void>(parser.as_uint(v, &col.collisionLayer));
    }
    if (parser.get_object_field(cval, "collisionMask", &v)) {
      static_cast<void>(parser.as_uint(v, &col.collisionMask));
    }
    static_cast<void>(world.add_collider(entity, col));
  }

  // NameComponent
  core::JsonValue nval{};
  if (parser.get_object_field(componentsVal, "NameComponent", &nval) &&
      (nval.type == core::JsonValue::Type::Object)) {
    NameComponent nc{};
    core::JsonValue v{};
    if (parser.get_object_field(nval, "name", &v)) {
      const char *namePtr = nullptr;
      std::size_t nameLen = 0U;
      if (parser.as_string(v, &namePtr, &nameLen) && (namePtr != nullptr)) {
        const std::size_t copyLen =
            (nameLen < sizeof(nc.name) - 1U) ? nameLen : (sizeof(nc.name) - 1U);
        std::memcpy(nc.name, namePtr, copyLen);
        nc.name[copyLen] = '\0';
      }
    }
    static_cast<void>(world.add_name_component(entity, nc));
  }

  // MeshComponent
  core::JsonValue mval{};
  if (parser.get_object_field(componentsVal, "MeshComponent", &mval) &&
      (mval.type == core::JsonValue::Type::Object)) {
    MeshComponent mesh{};
    core::JsonValue v{};
    if (parser.get_object_field(mval, "meshAssetId", &v)) {
      static_cast<void>(parser.as_uint64(v, &mesh.meshAssetId));
    }
    if (parser.get_object_field(mval, "albedo", &v)) {
      static_cast<void>(read_vec3(parser, v, &mesh.albedo));
    }
    if (parser.get_object_field(mval, "roughness", &v)) {
      static_cast<void>(parser.as_float(v, &mesh.roughness));
    }
    if (parser.get_object_field(mval, "metallic", &v)) {
      static_cast<void>(parser.as_float(v, &mesh.metallic));
    }
    if (parser.get_object_field(mval, "opacity", &v)) {
      static_cast<void>(parser.as_float(v, &mesh.opacity));
    }
    static_cast<void>(world.add_mesh_component(entity, mesh));
  }

  // LightComponent
  core::JsonValue lval{};
  if (parser.get_object_field(componentsVal, "LightComponent", &lval) &&
      (lval.type == core::JsonValue::Type::Object)) {
    LightComponent lc{};
    core::JsonValue v{};
    if (parser.get_object_field(lval, "color", &v)) {
      static_cast<void>(read_vec3(parser, v, &lc.color));
    }
    if (parser.get_object_field(lval, "direction", &v)) {
      static_cast<void>(read_vec3(parser, v, &lc.direction));
    }
    if (parser.get_object_field(lval, "intensity", &v)) {
      static_cast<void>(parser.as_float(v, &lc.intensity));
    }
    if (parser.get_object_field(lval, "type", &v)) {
      std::uint32_t typeVal = 0U;
      if (parser.as_uint(v, &typeVal)) {
        lc.type = (typeVal == 1U) ? LightType::Point : LightType::Directional;
      }
    }
    static_cast<void>(world.add_light_component(entity, lc));
  }

  // PointLightComponent
  core::JsonValue plVal{};
  if (parser.get_object_field(componentsVal, "PointLightComponent", &plVal) &&
      (plVal.type == core::JsonValue::Type::Object)) {
    PointLightComponent pc{};
    core::JsonValue v{};
    if (parser.get_object_field(plVal, "color", &v)) {
      static_cast<void>(read_vec3(parser, v, &pc.color));
    }
    if (parser.get_object_field(plVal, "intensity", &v)) {
      static_cast<void>(parser.as_float(v, &pc.intensity));
    }
    if (parser.get_object_field(plVal, "radius", &v)) {
      static_cast<void>(parser.as_float(v, &pc.radius));
    }
    static_cast<void>(world.add_point_light_component(entity, pc));
  }

  // SpotLightComponent
  core::JsonValue slVal{};
  if (parser.get_object_field(componentsVal, "SpotLightComponent", &slVal) &&
      (slVal.type == core::JsonValue::Type::Object)) {
    SpotLightComponent sc{};
    core::JsonValue v{};
    if (parser.get_object_field(slVal, "color", &v)) {
      static_cast<void>(read_vec3(parser, v, &sc.color));
    }
    if (parser.get_object_field(slVal, "direction", &v)) {
      static_cast<void>(read_vec3(parser, v, &sc.direction));
    }
    if (parser.get_object_field(slVal, "intensity", &v)) {
      static_cast<void>(parser.as_float(v, &sc.intensity));
    }
    if (parser.get_object_field(slVal, "radius", &v)) {
      static_cast<void>(parser.as_float(v, &sc.radius));
    }
    if (parser.get_object_field(slVal, "innerConeAngle", &v)) {
      static_cast<void>(parser.as_float(v, &sc.innerConeAngle));
    }
    if (parser.get_object_field(slVal, "outerConeAngle", &v)) {
      static_cast<void>(parser.as_float(v, &sc.outerConeAngle));
    }
    static_cast<void>(world.add_spot_light_component(entity, sc));
  }

  // ScriptComponent— accepts plain string (current) or legacy object format.
  core::JsonValue sval{};
  if (parser.get_object_field(componentsVal, "ScriptComponent", &sval)) {
    const char *pathPtr = nullptr;
    std::size_t pathLen = 0U;
    bool gotPath = false;

    if (sval.type == core::JsonValue::Type::String) {
      // Current format: "ScriptComponent": "path.lua"
      gotPath = parser.as_string(sval, &pathPtr, &pathLen);
    } else if (sval.type == core::JsonValue::Type::Object) {
      // Legacy format: "ScriptComponent": { "scriptPath": "path.lua" }
      core::JsonValue v{};
      if (parser.get_object_field(sval, "scriptPath", &v)) {
        gotPath = parser.as_string(v, &pathPtr, &pathLen);
      }
    }

    if (gotPath && (pathPtr != nullptr) && (pathLen > 0U)) {
      ScriptComponent sc{};
      const std::size_t copyLen = (pathLen < sizeof(sc.scriptPath) - 1U)
                                      ? pathLen
                                      : (sizeof(sc.scriptPath) - 1U);
      std::memcpy(sc.scriptPath, pathPtr, copyLen);
      sc.scriptPath[copyLen] = '\0';
      static_cast<void>(world.add_script_component(entity, sc));
    }
  }

  return entity;
}

} // namespace engine::runtime
