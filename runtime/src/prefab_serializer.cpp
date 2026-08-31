// Implements prefab serializer behavior for the Engine runtime world.

#include "engine/runtime/prefab_serializer.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/physics/physics.h"
#include "component_registry.h"
#include "engine/runtime/serialization_keys.h"
#include "engine/runtime/world.h"
#include "serialization_util.h"

namespace engine::runtime {

// ---- Registry-driven component codec (#166 W5) ----------------------------
// Row membership and order for both prefab directions expand from
// ENGINE_PERSISTENT_COMPONENT_TABLE; each type's prefab wire shape lives
// in the decode/encode pair below (default: object-shaped reflected codec;
// specials: Name's nested object, Script's legacy string-or-object with a
// required path, Animation's string-or-object with a required path, and the
// custom collider/mesh/light/foliage shapes).

/// Decodes one prefab component value into `out`; non-string shapes
/// require a JSON object, matching the pre-registry per-row validation.
template <typename T>
bool decode_prefab_component(const core::JsonParser &parser,
                             const core::JsonValue &value,
                             const ReflectedComponentDescriptors &descs,
                             T *out) noexcept {
  if constexpr (std::is_same_v<T, ScriptComponent>) {
    bool gotPath = false;
    if (value.type == core::JsonValue::Type::String) {
      gotPath = parser.copy_string_strict(value, out->scriptPath,
                                          sizeof(out->scriptPath));
    } else if (value.type == core::JsonValue::Type::Object) {
      core::JsonValue pathValue{};
      if (parser.get_object_field(value, "scriptPath", &pathValue)) {
        gotPath = parser.copy_string_strict(pathValue, out->scriptPath,
                                            sizeof(out->scriptPath));
      }
    } else {
      return false;
    }
    return gotPath && (out->scriptPath[0] != '\0');
  } else if constexpr (std::is_same_v<T, AnimationComponent>) {
    return read_animation_component(parser, value, true, out);
  } else if (value.type != core::JsonValue::Type::Object) {
    return false;
  } else if constexpr (std::is_same_v<T, NameComponent>) {
    core::JsonValue nameValue{};
    if (parser.get_object_field(value, "name", &nameValue)) {
      // Strict copy: an authored name that cannot round-trip through the
      // component's fixed field fails the load instead of silently losing
      // bytes a later save would make permanent.
      return parser.copy_string_strict(nameValue, out->name,
                                       sizeof(out->name));
    }
    return true;
  } else if constexpr (std::is_same_v<T, Collider>) {
    return read_collider_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, MeshComponent>) {
    return read_mesh_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, LightComponent>) {
    return read_light_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, FoliagePatchComponent>) {
    return read_foliage_patch_component(parser, value, out);
  } else {
    return read_reflected_component(parser, value,
                                    component_descriptor(descs, out), out);
  }
}

/// Encodes one prefab component under its registry key; empty
/// Script/Animation paths write nothing, matching the pre-registry writer.
template <typename T>
bool encode_prefab_component(core::JsonWriter &w, const char *key,
                             const ReflectedComponentDescriptors &descs,
                             const T &component) noexcept {
  if constexpr (std::is_same_v<T, NameComponent>) {
    w.write_key(key);
    w.begin_object();
    w.write_string("name", component.name);
    w.end_object();
    return true;
  } else if constexpr (std::is_same_v<T, ScriptComponent>) {
    if (component.scriptPath[0] != '\0') {
      w.write_string(key, component.scriptPath);
    }
    return true;
  } else if constexpr (std::is_same_v<T, AnimationComponent>) {
    write_animation_component(w, key, component);
    return true;
  } else if constexpr (std::is_same_v<T, Collider>) {
    return write_collider_component(w, component);
  } else if constexpr (std::is_same_v<T, MeshComponent>) {
    write_mesh_component(w, component);
    return true;
  } else if constexpr (std::is_same_v<T, LightComponent>) {
    write_light_component(w, component);
    return true;
  } else if constexpr (std::is_same_v<T, FoliagePatchComponent>) {
    write_foliage_patch_component(w, component);
    return true;
  } else {
    return write_reflected_component(
        w, key, component_descriptor(descs, &component), &component);
  }
}


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

  // A save must not claim success while dropping live authored state
  // (audit #208): heightfield samples and provenance-free custom hull
  // payloads have no prefab representation, so their save is refused
  // before the destination is touched. Builder-provenance hulls rebuild
  // from the serialized descriptor on instantiate and stay savable.
  // Joints are world-level runtime state between two bodies, never part
  // of a single-entity component template, so they do not block here.
  Collider collider{};
  const bool hasCollider = world.get_collider(entity, &collider);
  const physics::PhysicsContext &physicsContext = world.physics_context();
  if (hasCollider && (collider.shape == ColliderShape::ConvexHull) &&
      (collider.hullSource == math::HullSource::None) &&
      (physics::get_convex_hull_data(physicsContext, entity) != nullptr)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab refused: the entity's custom convex-hull "
                      "payload cannot be represented by the prefab format");
    return false;
  }
  if (hasCollider && (collider.shape == ColliderShape::Heightfield) &&
      (physics::get_heightfield_data(physicsContext, entity) != nullptr)) {
    core::log_message(core::LogLevel::Error, kPrefabLogChannel,
                      "save_prefab refused: the entity's heightfield payload "
                      "cannot be represented by the prefab format");
    return false;
  }

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kPrefabLogChannel)) {
    return false;
  }

  core::JsonWriter w{};
  w.begin_object();
  w.write_uint(kSchemaVersionKey, kPrefabVersion);
  w.write_key("components");
  w.begin_object();

#define ENGINE_PREFAB_WRITE_ROW(Type, Key, GetFn, AddFn, RemoveFn)             \
  {                                                                            \
    Type component{};                                                          \
    if (world.GetFn(entity, &component) &&                                     \
        !encode_prefab_component(w, Key, descs, component)) {                  \
      core::log_message(core::LogLevel::Error, kPrefabLogChannel,              \
                        "save_prefab: failed to write " #Type);                \
      return false;                                                            \
    }                                                                          \
  }
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PREFAB_WRITE_ROW)
#undef ENGINE_PREFAB_WRITE_ROW

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

  // The revision decides how everything below it is interpreted, so a
  // document this build cannot read is refused before an instance exists.
  if (!schema_version_supported(parser, root, kPrefabVersion, "prefab",
                                kPrefabLogChannel)) {
    return kInvalidEntity;
  }

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


#define ENGINE_PREFAB_READ_ROW(Type, Key, GetFn, AddFn, RemoveFn)              \
  {                                                                            \
    core::JsonValue value{};                                                   \
    if (parser.get_object_field(componentsVal, Key, &value)) {                 \
      Type component{};                                                        \
      if (!decode_prefab_component(parser, value, descs, &component) ||        \
          !world.AddFn(entity, component)) {                                   \
        return failComponent("instantiate_prefab: failed to load " #Type);     \
      }                                                                        \
    }                                                                          \
  }
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PREFAB_READ_ROW)
#undef ENGINE_PREFAB_READ_ROW

  return entity;
}

} // namespace engine::runtime
