// Implements scene serializer behavior for the Engine runtime world.

#include "engine/runtime/scene_serializer.h"

#include "engine/runtime/animation_system.h"

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
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/serialization_keys.h"
#include "engine/runtime/world.h"
#include "component_registry.h"
#include "serialization_util.h"

namespace engine::runtime {

namespace {

constexpr const char *kSceneLogChannel = "scene";
constexpr const char *kVersionKey = "version";
constexpr std::uint32_t kCurrentSceneVersion = 2U;
constexpr const char *kEntitiesKey = "entities";
constexpr const char *kComponentsKey = "components";
constexpr const char *kPersistentIdKey = "persistentId";
constexpr const char *kNameFieldKey = "name";
constexpr const char *kGravityKey = "gravity";

// File IO and vec/quat/foliage JSON helpers are shared with the prefab
// serializer via serialization_util.h.

bool log_scene_error(const char *message) noexcept {
  if (message != nullptr) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel, message);
  }

  return false;
}


// ---- Registry-driven component codec (#166 W5) ----------------------------
// Membership and row order for both serializer directions expand from
// ENGINE_PERSISTENT_COMPONENT_TABLE; only each type's wire shape lives
// below. A new registry row without a descriptor/decode/encode path fails
// to compile, so a persistent component cannot be silently absent from the
// scene format.

/// Scene key for a row: the registry key, except NameComponent's
/// pre-registry bare "name" field (kept byte-stable; migration is #252).
template <typename T>
const char *scene_component_key(const char *registryKey) noexcept {
  if constexpr (std::is_same_v<T, NameComponent>) {
    return kNameFieldKey;
  } else {
    return registryKey;
  }
}

/// Decodes one component value into `out`; the default path is the
/// reflected codec, with the custom wire shapes (collider payloads, mesh
/// LODs, light enums, foliage arrays, the bare Name/Script strings, and
/// Animation's string-or-object) enumerated explicitly.
template <typename T>
bool decode_scene_component(const core::JsonParser &parser,
                            const core::JsonValue &value,
                            const ReflectedComponentDescriptors &descs,
                            T *out) noexcept {
  if constexpr (std::is_same_v<T, Collider>) {
    return read_collider_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, MeshComponent>) {
    return read_mesh_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, LightComponent>) {
    return read_light_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, FoliagePatchComponent>) {
    return read_foliage_patch_component(parser, value, out);
  } else if constexpr (std::is_same_v<T, NameComponent>) {
    return parser.copy_string(value, out->name, sizeof(out->name));
  } else if constexpr (std::is_same_v<T, ScriptComponent>) {
    return parser.copy_string_strict(value, out->scriptPath,
                                     sizeof(out->scriptPath));
  } else if constexpr (std::is_same_v<T, AnimationComponent>) {
    return read_animation_component(parser, value, false, out);
  } else {
    return read_reflected_component(parser, value,
                                    component_descriptor(descs, out), out);
  }
}

/// Encodes one component under its key; mirrors decode_scene_component's
/// shape split. Empty Script/Animation paths write nothing, matching the
/// pre-registry writer.
template <typename T>
bool encode_scene_component(core::JsonWriter &writer, const char *key,
                            const ReflectedComponentDescriptors &descs,
                            const T &component) noexcept {
  if constexpr (std::is_same_v<T, Collider>) {
    return write_collider_component(writer, component);
  } else if constexpr (std::is_same_v<T, MeshComponent>) {
    write_mesh_component(writer, component);
    return true;
  } else if constexpr (std::is_same_v<T, LightComponent>) {
    write_light_component(writer, component);
    return true;
  } else if constexpr (std::is_same_v<T, FoliagePatchComponent>) {
    write_foliage_patch_component(writer, component);
    return true;
  } else if constexpr (std::is_same_v<T, NameComponent>) {
    writer.write_string(key, component.name);
    return true;
  } else if constexpr (std::is_same_v<T, ScriptComponent>) {
    if (component.scriptPath[0] != '\0') {
      writer.write_string(key, component.scriptPath);
    }
    return true;
  } else if constexpr (std::is_same_v<T, AnimationComponent>) {
    write_animation_component(writer, key, component);
    return true;
  } else {
    return write_reflected_component(
        writer, key, component_descriptor(descs, &component), &component);
  }
}

bool deserialize_scene_entities(const core::JsonParser &parser,
                                const core::JsonValue &entities,
                                const ReflectedComponentDescriptors &descs,
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
            ? targetWorld.create_scene_object_with_persistent_id(persistentId)
            : targetWorld.create_scene_object();
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

    const char *rowError = nullptr;
#define ENGINE_SCENE_READ_ROW(Type, Key, GetFn, AddFn, RemoveFn)               \
  if (rowError == nullptr) {                                                   \
    core::JsonValue value{};                                                   \
    if (parser.get_object_field(components, scene_component_key<Type>(Key),    \
                                &value)) {                                     \
      Type component{};                                                        \
      if (!decode_scene_component(parser, value, descs, &component) ||         \
          !targetWorld.AddFn(entity, component)) {                             \
        rowError = "failed to load " #Type;                                    \
      }                                                                        \
    }                                                                          \
  }
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_SCENE_READ_ROW)
#undef ENGINE_SCENE_READ_ROW
    if (rowError != nullptr) {
      targetWorld.destroy_entity(entity);
      return log_scene_error(rowError);
    }
  }

  return true;
}

/// Counts alive entities carrying the component through the public get
/// accessor, so commit invariants can compare worlds without new World API.
template <typename Component>
std::size_t count_components(
    const World &world,
    bool (World::*getComponent)(Entity, Component *) const noexcept) noexcept {
  std::size_t count = 0U;
  world.for_each_alive([&](Entity entity) noexcept {
    Component component{};
    if ((world.*getComponent)(entity, &component)) {
      ++count;
    }
  });
  return count;
}

/// True when source and target agree on the per-type component count for
/// every persistent-component registry row; a mismatch means the commit copy
/// lost or invented data. Expanded from ENGINE_PERSISTENT_COMPONENT_TABLE so
/// a new component type cannot be silently missed here.
bool world_component_counts_match(const World &sourceWorld,
                                  const World &targetWorld) noexcept {
  const auto matches = [&](auto getComponent) noexcept {
    return count_components(sourceWorld, getComponent) ==
           count_components(targetWorld, getComponent);
  };
  bool countsMatch = true;
#define ENGINE_PCR_COUNT_MATCH(Type, Key, GetFn, AddFn, RemoveFn)              \
  countsMatch = countsMatch && matches(&World::GetFn);
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_COUNT_MATCH)
#undef ENGINE_PCR_COUNT_MATCH
  return countsMatch;
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

    // One registry row per copyable component type; copy_component supplies
    // the guard/copy body once and ENGINE_PERSISTENT_COMPONENT_TABLE keeps
    // the row set complete by construction.
    const auto copy = [&](auto getComponent, auto addComponent) noexcept {
      return copy_component(sourceWorld, targetWorld, sourceEntity,
                            targetEntity, getComponent, addComponent);
    };
    success = true;
#define ENGINE_PCR_COPY_COMPONENT(Type, Key, GetFn, AddFn, RemoveFn)           \
  success = success && copy(&World::GetFn, &World::AddFn);
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_COPY_COMPONENT)
#undef ENGINE_PCR_COPY_COMPONENT
  });

  // Timers deliberately do not ride the commit copy: they are runtime-only
  // state a freshly staged world never carries (issue #209).

  // World gravity rides the commit copy like the components do.
  if (success) {
    math::Vec3 gravity(0.0F, -9.8F, 0.0F);
    if (get_gravity(sourceWorld, &gravity.x, &gravity.y, &gravity.z)) {
      set_gravity(targetWorld, gravity.x, gravity.y, gravity.z);
    }
  }

  return success;
}

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

  // A save must not claim success while dropping live authored state
  // (audit #208, supersedes H-01's warn-then-succeed): refuse before the
  // destination file or buffer is touched.
  const SceneSaveBlockers blockers = collect_scene_save_blockers(world);
  if ((blockers.customHullPayloads > 0U) ||
      (blockers.heightfieldPayloads > 0U) || (blockers.activeJoints > 0U)) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "save_scene refused: %zu custom convex-hull payload(s), "
                  "%zu heightfield payload(s), and %zu active physics "
                  "joint(s) cannot be represented by the scene format; "
                  "remove them (or stop the running simulation) and save "
                  "again",
                  blockers.customHullPayloads, blockers.heightfieldPayloads,
                  blockers.activeJoints);
    core::log_message(core::LogLevel::Error, kSceneLogChannel, message);
    return false;
  }

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kSceneLogChannel)) {
    return false;
  }

  core::JsonWriter &writer = *outWriter;
  writer.reset();
  writer.begin_object();
  writer.write_uint(kVersionKey, kCurrentSceneVersion);

  // World gravity is authored state; written only when it differs from the
  // default so existing scenes stay byte-identical.
  math::Vec3 gravity(0.0F, -9.8F, 0.0F);
  static_cast<void>(get_gravity(world, &gravity.x, &gravity.y, &gravity.z));
  if ((gravity.x != 0.0F) || (gravity.y != -9.8F) || (gravity.z != 0.0F)) {
    write_vec3(writer, kGravityKey, gravity);
  }

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

#define ENGINE_SCENE_WRITE_ROW(Type, Key, GetFn, AddFn, RemoveFn)              \
    {                                                                          \
      Type component{};                                                        \
      if (world.GetFn(entity, &component) &&                                   \
          !encode_scene_component(writer, scene_component_key<Type>(Key),      \
                                  descs, component)) {                         \
        writeFailed = true;                                                    \
        return;                                                                \
      }                                                                        \
    }
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_SCENE_WRITE_ROW)
#undef ENGINE_SCENE_WRITE_ROW

    writer.end_object();
    writer.end_object();
    writeFailed = writer.failed();
  });

  writer.end_array();

  // Timers are runtime-only, per-scene state and are deliberately not
  // serialized (issue #209): a callback has no stable identity the format
  // could restore, and the canonical load path clears the scripting timer
  // registry anyway. Scripts re-arm their timers in on_begin_play.
  writer.end_object();

  if (writeFailed || !writer.ok()) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "failed to build scene JSON");
    return false;
  }

  return true;
}

} // namespace

/// Collects the live state save_scene would refuse to drop (#208). Hull
/// payloads with builder provenance rebuild from the serialized descriptor
/// on every install path, so only provenance-free payloads count.
SceneSaveBlockers collect_scene_save_blockers(const World &world) noexcept {
  SceneSaveBlockers blockers{};
  const physics::PhysicsContext &context = world.physics_context();
  const physics::PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return blockers;
  }

  // Payload tables are compacted on removal, but a payload only blocks
  // while a live collider of the matching shape still consumes it.
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    const Entity entity = store->convexHullEntity[i];
    Collider collider{};
    if (world.is_alive(entity) && world.get_collider(entity, &collider) &&
        (collider.shape == ColliderShape::ConvexHull) &&
        (collider.hullSource == math::HullSource::None)) {
      ++blockers.customHullPayloads;
    }
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    const Entity entity = store->heightfieldEntity[i];
    Collider collider{};
    if (world.is_alive(entity) && world.get_collider(entity, &collider) &&
        (collider.shape == ColliderShape::Heightfield)) {
      ++blockers.heightfieldPayloads;
    }
  }
  // jointCount is a slot high-water mark; removed joints leave inactive
  // slots behind, so only active slots hold droppable state.
  for (std::size_t i = 0U; i < context.jointCount; ++i) {
    if (store->joints[i].active) {
      ++blockers.activeJoints;
    }
  }
  return blockers;
}

/// Resets this object back to its reusable empty state for world.
/// Declared reset order (audit H-18, extended by #198): beforeTeardown
/// first — while the outgoing entities and their script modules are still
/// alive, this is where process_pending_scene_op dispatches on_end_play —
/// then entities, the phase-independent destructive teardown, so component
/// removal releases its physics/camera bookkeeping while those managers
/// still exist, then timers, cameras, game mode, the content epoch, and
/// last the animation controller registry, which must only reset once no
/// component can still hold a controllerSlot into it.
void reset_world(World &world, SceneTeardownHook beforeTeardown) noexcept {
  if (beforeTeardown != nullptr) {
    beforeTeardown();
  }
  world.reset_all_entities();
  if (world.alive_entity_count() != 0U) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "reset_world left surviving entities");
  }

  world.timer_manager().clear();
  world.camera_manager().clear();
  world.game_mode().reset();
  world.mark_content_replaced(world.content_epoch());
  reset_anim_controllers();
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
bool load_scene(World &world, const char *path,
                SceneTeardownHook beforeTeardown) noexcept {
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

  return load_scene(world, fileBuffer.get(), fileSize, beforeTeardown);
}

/// Loads the requested resource for scene. Declared reset order (#198):
/// every validation and staging step below runs on a scratch World and
/// never touches the caller's world, so a failure returns false with the
/// outgoing scene completely untouched and beforeTeardown never called;
/// only once the replacement content is fully staged and invariant-checked
/// does beforeTeardown fire (outgoing entities and modules still alive)
/// immediately before the destructive `world = *committedWorld` commit.
bool load_scene(World &world, const char *buffer, std::size_t size,
                SceneTeardownHook beforeTeardown) noexcept {
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

  ReflectedComponentDescriptors descs{};
  if (!find_reflected_component_descriptors(&descs, kSceneLogChannel)) {
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

  // World gravity: optional root field, default when absent.
  core::JsonValue gravityValue{};
  if (parser.get_object_field(*root, kGravityKey, &gravityValue)) {
    math::Vec3 gravity{};
    if (!read_vec3(parser, gravityValue, &gravity)) {
      return log_scene_error("invalid gravity field");
    }
    set_gravity(*stagedWorld, gravity.x, gravity.y, gravity.z);
  }

  // Legacy "timers" blocks (scenes saved before issue #209) are ignored:
  // the serialized timing carried no callback identity, so a restored
  // timer could never fire on the production load path — it was cleared or
  // dropped inert. Timers are runtime-only state; scripts re-arm them in
  // on_begin_play.
  core::JsonValue timersArray{};
  if (parser.get_object_field(*root, "timers", &timersArray) &&
      (timersArray.type == core::JsonValue::Type::Array) &&
      (parser.array_size(timersArray) > 0U)) {
    core::log_message(core::LogLevel::Info, kSceneLogChannel,
                      "scene carries a legacy timers block; timers are "
                      "runtime-only state and are not restored (issue #209)");
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
      !world_component_counts_match(*stagedWorld, *committedWorld)) {
    core::log_message(core::LogLevel::Error, kSceneLogChannel,
                      "scene commit invariant mismatch after copy");
    return false;
  }

  const std::uint32_t previousEpoch = world.content_epoch();

  // Staging fully succeeded and passed its invariant checks, so the
  // transition is now guaranteed to commit: dispatch on_end_play to the
  // still-alive outgoing world before it is overwritten (#198).
  if (beforeTeardown != nullptr) {
    beforeTeardown();
  }

  world = *committedWorld;
  world.mark_content_replaced(previousEpoch);
  // The replaced world's components are gone, so their cached animation
  // controllers are released; the loaded scene's components re-acquire
  // lazily on the next animation update (controllerSlot is runtime state
  // and never serialized).
  reset_anim_controllers();
  return true;
}

} // namespace engine::runtime
