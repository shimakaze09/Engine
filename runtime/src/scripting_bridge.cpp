// Implements the runtime side of the scripting bridge: every operation in
// the RuntimeServices table scripting declares, forwarding the caller's
// entity handle unchanged so the World's own generation check decides
// liveness, plus the entity pools and the install into the locators.

#include "engine/runtime/scripting_bridge.h"

#include "engine/audio/audio.h"
#include "engine/core/logging.h"
#include "engine/core/rng.h"
#include "engine/core/vfs.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_query.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/content/asset_streaming.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/animation_system.h"
#include "engine/runtime/entity_pool.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/primitive_collider.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/save_data.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include "component_registry.h"
#include "engine_runtime_streaming.h"
#include "mesh_reference_resolution.h"

#include <cstring>

namespace engine {

namespace {

constexpr std::uint32_t kInvalidScriptAssetHandle = 0xFFFFFFFFU;
constexpr std::uint32_t kScriptAssetHandleSlotMask = 0xFFFFU;
constexpr std::uint32_t kScriptAssetHandleGenerationShift = 16U;

runtime::EngineAssetDatabaseService *g_scriptingAssetDatabaseService = nullptr;

/// Encodes a runtime asset load handle for Lua.
std::uint32_t encode_script_asset_handle(std::uint32_t slot,
                                         std::uint16_t generation) noexcept {
  if (slot >= runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles) {
    return kInvalidScriptAssetHandle;
  }
  return (static_cast<std::uint32_t>(generation)
          << kScriptAssetHandleGenerationShift) |
         slot;
}

/// Decodes a Lua-facing runtime asset load handle.
bool decode_script_asset_handle(std::uint32_t handle, std::uint32_t *outSlot,
                                std::uint16_t *outGeneration) noexcept {
  if ((handle == kInvalidScriptAssetHandle) || (outSlot == nullptr) ||
      (outGeneration == nullptr)) {
    return false;
  }

  const std::uint32_t slot = handle & kScriptAssetHandleSlotMask;
  if (slot >= runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles) {
    return false;
  }

  *outSlot = slot;
  *outGeneration =
      static_cast<std::uint16_t>(handle >> kScriptAssetHandleGenerationShift);
  return true;
}

/// Finds an existing Lua handle slot for an asset id.
std::uint32_t find_script_asset_handle_slot(
    const runtime::EngineAssetDatabaseService *service,
    content::AssetId assetId) noexcept {
  if ((service == nullptr) || (assetId == content::kInvalidAssetId)) {
    return runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles;
  }

  for (std::uint32_t i = 0U;
       i < runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles;
       ++i) {
    const auto &handle = service->scriptLoadHandles[i];
    if (handle.occupied && (handle.assetId == assetId)) {
      return i;
    }
  }

  return runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles;
}

/// Allocates or reuses a Lua handle slot for a runtime asset request.
std::uint32_t
allocate_script_asset_handle_slot(runtime::EngineAssetDatabaseService *service,
                                  content::AssetId assetId) noexcept {
  if ((service == nullptr) || (assetId == content::kInvalidAssetId)) {
    return runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles;
  }

  const std::uint32_t existing =
      find_script_asset_handle_slot(service, assetId);
  if (existing <
      runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles) {
    return existing;
  }

  for (std::uint32_t i = 0U;
       i < runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles;
       ++i) {
    auto &handle = service->scriptLoadHandles[i];
    if (!handle.occupied) {
      handle.occupied = true;
      handle.assetId = assetId;
      handle.streamingHandle = content::kInvalidLoadHandle;
      ++handle.generation;
      if (handle.generation == 0U) {
        handle.generation = 1U;
      }
      return i;
    }
  }

  return runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles;
}

/// Maps Lua asset priority values to renderer streaming priorities.
content::LoadPriority script_asset_priority(std::uint8_t priority) noexcept {
  switch (priority) {
  case 0:
    return content::LoadPriority::Low;
  case 2:
    return content::LoadPriority::High;
  case 3:
    return content::LoadPriority::Immediate;
  case 1:
  default:
    return content::LoadPriority::Normal;
  }
}

void scripting_set_camera_position(float x, float y, float z) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.position = math::Vec3(x, y, z);
  renderer::set_active_camera(camera);
}

void scripting_set_camera_target(float x, float y, float z) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.target = math::Vec3(x, y, z);
  renderer::set_active_camera(camera);
}

void scripting_set_camera_up(float x, float y, float z) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.up = math::Vec3(x, y, z);
  renderer::set_active_camera(camera);
}

void scripting_set_camera_fov(float fovRadians) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.fovRadians = fovRadians;
  renderer::set_active_camera(camera);
}

static_assert(scripting::kMaxWorldEntities == runtime::World::kMaxEntities,
              "scripting's entity capacity must match the World's");
static_assert(scripting::kMaxTimerSlots == runtime::TimerManager::kMaxTimers,
              "scripting's timer slot count must match the timer manager's");
static_assert(scripting::kMaxEntityPoolSize ==
                  runtime::EntityPool::kMaxPoolSize,
              "scripting's pool size must match the entity pool's");
static_assert(static_cast<int>(scripting::GameModeState::WaitingToStart) ==
                      static_cast<int>(runtime::GameMode::State::WaitingToStart) &&
                  static_cast<int>(scripting::GameModeState::InProgress) ==
                      static_cast<int>(runtime::GameMode::State::InProgress) &&
                  static_cast<int>(scripting::GameModeState::Paused) ==
                      static_cast<int>(runtime::GameMode::State::Paused) &&
                  static_cast<int>(scripting::GameModeState::Ended) ==
                      static_cast<int>(runtime::GameMode::State::Ended),
              "scripting's game mode states must mirror the runtime's");

// Camera manager bridge functions
bool scripting_push_camera(runtime::World *world, runtime::Entity entity,
                           float posX, float posY, float posZ, float tgtX,
                           float tgtY, float tgtZ, float priority,
                           float blendSpeed) noexcept {
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }
  runtime::CameraEntry entry{};
  entry.position = math::Vec3(posX, posY, posZ);
  entry.target = math::Vec3(tgtX, tgtY, tgtZ);
  entry.blendSpeed = blendSpeed;
  return world->camera_manager().push_camera(entity, entry, priority);
}

bool scripting_pop_camera(runtime::World *world,
                          runtime::Entity entity) noexcept {
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }
  return world->camera_manager().pop_camera(entity);
}

bool scripting_get_active_camera(runtime::World *world, float *outPosX,
                                 float *outPosY, float *outPosZ, float *outTgtX,
                                 float *outTgtY, float *outTgtZ,
                                 float *outFov) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::CameraEntry *entry = world->camera_manager().active_camera();
  if (entry == nullptr) {
    return false;
  }
  *outPosX = entry->position.x;
  *outPosY = entry->position.y;
  *outPosZ = entry->position.z;
  *outTgtX = entry->target.x;
  *outTgtY = entry->target.y;
  *outTgtZ = entry->target.z;
  *outFov = entry->fovRadians;
  return true;
}

bool scripting_camera_shake(runtime::World *world, float amplitude,
                            float frequency, float duration,
                            float decay) noexcept {
  if (world == nullptr) {
    return false;
  }
  return world->camera_manager().add_shake(amplitude, frequency, duration,
                                           decay);
}

void scripting_set_gravity(runtime::World *world, float x, float y,
                           float z) noexcept {
  if (world == nullptr) {
    return;
  }

  runtime::set_gravity(*world, x, y, z);
}

bool scripting_get_gravity(runtime::World *world, float *outX, float *outY,
                           float *outZ) noexcept {
  if ((world == nullptr) || (outX == nullptr) || (outY == nullptr) ||
      (outZ == nullptr)) {
    return false;
  }

  return runtime::get_gravity(*world, outX, outY, outZ);
}

/// Mirrors a physics raycast hit into the bridge's flat hit record.
void copy_raycast_hit(const runtime::PhysicsRaycastHit &hit,
                      scripting::RuntimeRaycastHit *outHit) noexcept {
  outHit->entity = hit.entity;
  outHit->distance = hit.distance;
  outHit->pointX = hit.point.x;
  outHit->pointY = hit.point.y;
  outHit->pointZ = hit.point.z;
  outHit->normalX = hit.normal.x;
  outHit->normalY = hit.normal.y;
  outHit->normalZ = hit.normal.z;
}

/// Mirrors a sweep hit into the bridge's flat hit record; the sweep names
/// its entity by index, resolved to the live handle here.
void copy_sweep_hit(const runtime::World &world, const physics::SweepHit &hit,
                    scripting::RuntimeRaycastHit *outHit) noexcept {
  outHit->entity = world.find_entity_by_index(hit.entityIndex);
  outHit->distance = hit.distance;
  outHit->pointX = hit.contactPoint.x;
  outHit->pointY = hit.contactPoint.y;
  outHit->pointZ = hit.contactPoint.z;
  outHit->normalX = hit.normal.x;
  outHit->normalY = hit.normal.y;
  outHit->normalZ = hit.normal.z;
}

bool scripting_raycast(runtime::World *world, float ox, float oy, float oz,
                       float dx, float dy, float dz, float maxDistance,
                       scripting::RuntimeRaycastHit *outHit,
                       runtime::Entity skipEntity) noexcept {
  if ((world == nullptr) || (outHit == nullptr)) {
    return false;
  }
  runtime::PhysicsRaycastHit hit{};
  if (!runtime::raycast(*world, math::Vec3(ox, oy, oz), math::Vec3(dx, dy, dz),
                        maxDistance, &hit, skipEntity)) {
    return false;
  }
  copy_raycast_hit(hit, outHit);
  return true;
}

std::size_t scripting_raycast_all(runtime::World *world, float ox, float oy,
                                  float oz, float dx, float dy, float dz,
                                  float maxDistance,
                                  scripting::RuntimeRaycastHit *outHits,
                                  std::size_t maxHits, std::uint32_t mask,
                                  runtime::Entity skipEntity) noexcept {
  if ((world == nullptr) || (outHits == nullptr) || (maxHits == 0U)) {
    return 0U;
  }
  constexpr std::size_t kLocalMax = 32U;
  const std::size_t cap = maxHits < kLocalMax ? maxHits : kLocalMax;
  runtime::PhysicsRaycastHit hits[kLocalMax]{};
  const std::size_t count = runtime::raycast_all(
      *world, math::Vec3(ox, oy, oz), math::Vec3(dx, dy, dz), maxDistance,
      hits, cap, mask, skipEntity);
  for (std::size_t i = 0U; i < count; ++i) {
    copy_raycast_hit(hits[i], &outHits[i]);
  }
  return count;
}

/// Resolves the index results of an overlap query to live handles.
std::size_t resolve_overlap_entities(const runtime::World &world,
                                     const std::uint32_t *indices,
                                     std::size_t count,
                                     runtime::Entity *outEntities) noexcept {
  for (std::size_t i = 0U; i < count; ++i) {
    outEntities[i] = world.find_entity_by_index(indices[i]);
  }
  return count;
}

constexpr std::size_t kMaxOverlapResults = 64U;

std::size_t scripting_overlap_sphere(runtime::World *world, float cx, float cy,
                                     float cz, float radius,
                                     runtime::Entity *outEntities,
                                     std::size_t maxResults,
                                     std::uint32_t mask) noexcept {
  if ((world == nullptr) || (outEntities == nullptr)) {
    return 0U;
  }
  std::uint32_t indices[kMaxOverlapResults]{};
  const std::size_t cap =
      maxResults < kMaxOverlapResults ? maxResults : kMaxOverlapResults;
  const std::size_t count = runtime::overlap_sphere(
      *world, math::Vec3(cx, cy, cz), radius, indices, cap, mask);
  return resolve_overlap_entities(*world, indices, count, outEntities);
}

std::size_t scripting_overlap_box(runtime::World *world, float cx, float cy,
                                  float cz, float hx, float hy, float hz,
                                  runtime::Entity *outEntities,
                                  std::size_t maxResults,
                                  std::uint32_t mask) noexcept {
  if ((world == nullptr) || (outEntities == nullptr)) {
    return 0U;
  }
  std::uint32_t indices[kMaxOverlapResults]{};
  const std::size_t cap =
      maxResults < kMaxOverlapResults ? maxResults : kMaxOverlapResults;
  const std::size_t count =
      runtime::overlap_box(*world, math::Vec3(cx, cy, cz),
                           math::Vec3(hx, hy, hz), indices, cap, mask);
  return resolve_overlap_entities(*world, indices, count, outEntities);
}

bool scripting_sweep_sphere(runtime::World *world, float ox, float oy, float oz,
                            float radius, float dx, float dy, float dz,
                            float maxDistance,
                            scripting::RuntimeRaycastHit *outHit,
                            std::uint32_t mask,
                            runtime::Entity skipEntity) noexcept {
  if ((world == nullptr) || (outHit == nullptr)) {
    return false;
  }
  physics::SweepHit sh{};
  if (!runtime::sweep_sphere(*world, math::Vec3(ox, oy, oz), radius,
                             math::Vec3(dx, dy, dz), maxDistance, &sh, mask,
                             skipEntity)) {
    return false;
  }
  copy_sweep_hit(*world, sh, outHit);
  return true;
}

bool scripting_sweep_box(runtime::World *world, float cx, float cy, float cz,
                         float hx, float hy, float hz, float dx, float dy,
                         float dz, float maxDistance,
                         scripting::RuntimeRaycastHit *outHit,
                         std::uint32_t mask,
                         runtime::Entity skipEntity) noexcept {
  if ((world == nullptr) || (outHit == nullptr)) {
    return false;
  }
  physics::SweepHit sh{};
  if (!runtime::sweep_box(*world, math::Vec3(cx, cy, cz),
                          math::Vec3(hx, hy, hz), math::Vec3(dx, dy, dz),
                          maxDistance, &sh, mask, skipEntity)) {
    return false;
  }
  copy_sweep_hit(*world, sh, outHit);
  return true;
}

/// Folds the native kInvalidJointId onto the bridge's single 0 failure
/// sentinel; valid ids always carry a non-zero generation.
std::uint32_t normalize_joint_id(physics::JointId id) noexcept {
  return (id == physics::kInvalidJointId) ? 0U
                                          : static_cast<std::uint32_t>(id);
}

/// True when both joint endpoints are live in `world`.
bool joint_endpoints_alive(const runtime::World *world,
                           runtime::Entity entityA,
                           runtime::Entity entityB) noexcept {
  return (world != nullptr) && world->is_alive(entityA) &&
         world->is_alive(entityB);
}

std::uint32_t scripting_add_distance_joint(runtime::World *world,
                                           runtime::Entity entityA,
                                           runtime::Entity entityB,
                                           float distance) noexcept {
  if (!joint_endpoints_alive(world, entityA, entityB)) {
    return 0U;
  }
  return normalize_joint_id(
      runtime::add_distance_joint(*world, entityA, entityB, distance));
}

bool scripting_remove_joint(runtime::World *world,
                            std::uint32_t jointId) noexcept {
  if (world == nullptr) {
    return false;
  }
  return runtime::remove_joint(*world, static_cast<physics::JointId>(jointId));
}

std::uint32_t scripting_add_hinge_joint(runtime::World *world,
                                        runtime::Entity entityA,
                                        runtime::Entity entityB, float pivotX,
                                        float pivotY, float pivotZ,
                                        float axisX, float axisY,
                                        float axisZ) noexcept {
  if (!joint_endpoints_alive(world, entityA, entityB)) {
    return 0U;
  }
  const math::Vec3 pivot(pivotX, pivotY, pivotZ);
  const math::Vec3 axis(axisX, axisY, axisZ);
  return normalize_joint_id(
      runtime::add_hinge_joint(*world, entityA, entityB, pivot, axis));
}

std::uint32_t scripting_add_ball_socket_joint(runtime::World *world,
                                              runtime::Entity entityA,
                                              runtime::Entity entityB,
                                              float pivotX, float pivotY,
                                              float pivotZ) noexcept {
  if (!joint_endpoints_alive(world, entityA, entityB)) {
    return 0U;
  }
  const math::Vec3 pivot(pivotX, pivotY, pivotZ);
  return normalize_joint_id(
      runtime::add_ball_socket_joint(*world, entityA, entityB, pivot));
}

std::uint32_t scripting_add_slider_joint(runtime::World *world,
                                         runtime::Entity entityA,
                                         runtime::Entity entityB, float axisX,
                                         float axisY, float axisZ) noexcept {
  if (!joint_endpoints_alive(world, entityA, entityB)) {
    return 0U;
  }
  const math::Vec3 axis(axisX, axisY, axisZ);
  return normalize_joint_id(
      runtime::add_slider_joint(*world, entityA, entityB, axis));
}

std::uint32_t scripting_add_spring_joint(runtime::World *world,
                                         runtime::Entity entityA,
                                         runtime::Entity entityB,
                                         float restLength, float stiffness,
                                         float damping) noexcept {
  if (!joint_endpoints_alive(world, entityA, entityB)) {
    return 0U;
  }
  return normalize_joint_id(runtime::add_spring_joint(
      *world, entityA, entityB, restLength, stiffness, damping));
}

std::uint32_t scripting_add_fixed_joint(runtime::World *world,
                                        runtime::Entity entityA,
                                        runtime::Entity entityB) noexcept {
  if (!joint_endpoints_alive(world, entityA, entityB)) {
    return 0U;
  }
  return normalize_joint_id(
      runtime::add_fixed_joint(*world, entityA, entityB));
}

bool scripting_set_joint_limits(runtime::World *world, std::uint32_t jointId,
                                float minLimit, float maxLimit) noexcept {
  if (world == nullptr) {
    return false;
  }
  return runtime::set_joint_limits(
      *world, static_cast<physics::JointId>(jointId), minLimit, maxLimit);
}

void scripting_wake_body(runtime::World *world,
                         runtime::Entity entity) noexcept {
  if ((world == nullptr) || !world->is_alive(entity)) {
    return;
  }
  runtime::wake_body(*world, entity);
}

bool scripting_is_sleeping(runtime::World *world,
                           runtime::Entity entity) noexcept {
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }
  return runtime::is_sleeping(*world, entity);
}

std::uint32_t scripting_load_sound(const char *path) noexcept {
  return audio::load_sound(path).id;
}

void scripting_unload_sound(std::uint32_t soundId) noexcept {
  audio::unload_sound(audio::SoundHandle{soundId});
}

bool scripting_play_sound(std::uint32_t soundId, float volume, float pitch,
                          bool loop) noexcept {
  audio::PlayParams params{};
  params.volume = volume;
  params.pitch = pitch;
  params.loop = loop;
  return audio::play_sound(audio::SoundHandle{soundId}, params);
}

void scripting_stop_sound(std::uint32_t soundId) noexcept {
  audio::stop_sound(audio::SoundHandle{soundId});
}

void scripting_stop_all_sounds() noexcept { audio::stop_all(); }

void scripting_set_master_volume(float volume) noexcept {
  audio::set_master_volume(volume);
}

bool scripting_play_sound_at(std::uint32_t soundId, float x, float y, float z,
                             float volume) noexcept {
  audio::PlayParams params{};
  params.volume = volume;
  return audio::play_sound_at(audio::SoundHandle{soundId},
                              math::Vec3(x, y, z), params,
                              audio::AudioBus::Sfx);
}

void scripting_set_bus_volume(std::uint32_t bus, float volume) noexcept {
  if (bus <= static_cast<std::uint32_t>(audio::AudioBus::Sfx)) {
    audio::set_bus_volume(static_cast<audio::AudioBus>(bus), volume);
  }
}

bool scripting_play_music(const char *path, float volume,
                          bool loop) noexcept {
  return audio::play_music(path, volume, loop);
}

void scripting_stop_music() noexcept { audio::stop_music(); }

bool scripting_save_game_data(const char *json,
                              std::size_t length) noexcept {
  return runtime::save_game_data(json, length);
}

bool scripting_load_game_data(char *out, std::size_t capacity,
                              std::size_t *outLength) noexcept {
  return runtime::load_game_data(out, capacity, outLength);
}

bool scripting_save_scene(const runtime::World *world,
                          const char *path) noexcept {
  return (world != nullptr) && runtime::save_scene(*world, path);
}

bool scripting_save_prefab(const runtime::World *world, runtime::Entity entity,
                           const char *path) noexcept {
  return (world != nullptr) && world->is_alive(entity) &&
         runtime::save_prefab(*world, entity, path);
}

runtime::Entity scripting_instantiate_prefab(runtime::World *world,
                                             const char *path) noexcept {
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }
  return runtime::instantiate_prefab(*world, path);
}

/// The catalog's persistent identity for an asset id; nil when the id is
/// unknown or the asset carries no identity. Never mints one: an asset
/// without a sidecar is reported by the mount walk, and a made-up
/// identity in a saved scene would name a different asset next run.
core::AssetRef scripting_asset_ref_for_id(std::uint64_t assetId) noexcept {
  if ((assetId == content::kInvalidAssetId) ||
      (g_scriptingAssetDatabaseService == nullptr) ||
      (g_scriptingAssetDatabaseService->catalog == nullptr)) {
    return core::AssetRef{};
  }
  const content::AssetMetadata *metadata = content::find_asset_metadata(
      g_scriptingAssetDatabaseService->catalog, assetId);
  return (metadata != nullptr) ? metadata->ref : core::AssetRef{};
}

/// Queues a mesh asset load through runtime-owned asset services.
std::uint32_t scripting_load_asset_async(const char *path,
                                         std::uint8_t priority) noexcept {
  if ((path == nullptr) || (path[0] == '\0') ||
      (g_scriptingAssetDatabaseService == nullptr) ||
      (g_scriptingAssetDatabaseService->database == nullptr) ||
      (g_scriptingAssetDatabaseService->catalog == nullptr)) {
    return kInvalidScriptAssetHandle;
  }

  // The virtual path is the asset's identity; the bytes come from wherever
  // the mount puts it. The streaming worker and the request queue open the
  // path they are handed, so it must already be the OS path.
  const content::AssetId assetId = content::make_asset_id_from_path(path);
  if (assetId == content::kInvalidAssetId) {
    return kInvalidScriptAssetHandle;
  }
  char osPath[512] = {};
  if (!core::vfs_resolve_os_path(path, osPath, sizeof(osPath))) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "load_asset_async: virtual path did not resolve to an "
                      "OS path (is its mount registered?)");
    return kInvalidScriptAssetHandle;
  }
  // A mesh a script names by path is catalogued under that path, the
  // same record the editor's picker and a reopened scene read. It carries
  // no authored identity: asking for a file by name is not importing it,
  // so the record is reachable by id for this session and by nothing
  // afterwards.
  static_cast<void>(
      note_mesh_asset_path(g_scriptingAssetDatabaseService->catalog, assetId,
                           path, core::AssetRef{}));

  retire_terminal_script_loads(g_scriptingAssetDatabaseService);

  const bool alreadyReady =
      renderer::mesh_asset_state(g_scriptingAssetDatabaseService->database,
                                 assetId) == content::AssetState::Ready;

  const std::uint32_t slot = allocate_script_asset_handle_slot(
      g_scriptingAssetDatabaseService, assetId);
  if (slot >= runtime::EngineAssetDatabaseService::kMaxScriptAssetLoadHandles) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "load_asset_async: no free runtime asset handles");
    return kInvalidScriptAssetHandle;
  }

  auto &scriptHandle = g_scriptingAssetDatabaseService->scriptLoadHandles[slot];
  scriptHandle.streamingHandle = content::kInvalidLoadHandle;

  if (g_scriptingAssetDatabaseService->streamingQueue != nullptr) {
    if (!renderer::request_mesh_asset_streaming_load(
            g_scriptingAssetDatabaseService->database, assetId, osPath)) {
      scriptHandle.occupied = false;
      scriptHandle.assetId = content::kInvalidAssetId;
      return kInvalidScriptAssetHandle;
    }

    if (!alreadyReady) {
      const content::LoadHandle streamingHandle = content::load_asset_async(
          g_scriptingAssetDatabaseService->streamingQueue, assetId, osPath,
          script_asset_priority(priority));
      if (!streamingHandle.valid()) {
        static_cast<void>(renderer::set_mesh_asset_state(
            g_scriptingAssetDatabaseService->database, assetId,
            content::AssetState::Failed, renderer::kInvalidMeshHandle));
        scriptHandle.occupied = false;
        scriptHandle.assetId = content::kInvalidAssetId;
        return kInvalidScriptAssetHandle;
      }
      scriptHandle.streamingHandle = streamingHandle;
    }

    return encode_script_asset_handle(slot, scriptHandle.generation);
  }

  if ((g_scriptingAssetDatabaseService->manager == nullptr) ||
      !renderer::queue_mesh_load(g_scriptingAssetDatabaseService->manager,
                                 g_scriptingAssetDatabaseService->database,
                                 assetId, osPath)) {
    auto &handle = g_scriptingAssetDatabaseService->scriptLoadHandles[slot];
    handle.occupied = false;
    handle.assetId = content::kInvalidAssetId;
    return kInvalidScriptAssetHandle;
  }

  return encode_script_asset_handle(slot, scriptHandle.generation);
}

/// Returns whether a Lua-facing runtime asset load handle is ready.
bool scripting_is_asset_ready(std::uint32_t handleIndex) noexcept {
  if ((g_scriptingAssetDatabaseService == nullptr) ||
      (g_scriptingAssetDatabaseService->database == nullptr)) {
    return false;
  }

  std::uint32_t slot = 0U;
  std::uint16_t generation = 0U;
  if (!decode_script_asset_handle(handleIndex, &slot, &generation)) {
    return false;
  }

  const auto &handle = g_scriptingAssetDatabaseService->scriptLoadHandles[slot];
  if (!handle.occupied || (handle.generation != generation) ||
      (handle.assetId == content::kInvalidAssetId)) {
    return false;
  }

  const bool databaseReady =
      renderer::mesh_asset_state(g_scriptingAssetDatabaseService->database,
                                 handle.assetId) == content::AssetState::Ready;
  if ((g_scriptingAssetDatabaseService->streamingQueue != nullptr) &&
      handle.streamingHandle.valid()) {
    return databaseReady && content::is_load_ready(
                                g_scriptingAssetDatabaseService->streamingQueue,
                                handle.streamingHandle);
  }

  return databaseReady;
}

// World identity, lookup and iteration.
bool scripting_is_input_phase(runtime::World *world) noexcept {
  return (world != nullptr) &&
         (world->current_phase() == runtime::WorldPhase::Input);
}

bool scripting_is_alive(runtime::World *world,
                        runtime::Entity entity) noexcept {
  return (world != nullptr) && world->is_alive(entity);
}

std::uint32_t scripting_content_epoch(runtime::World *world) noexcept {
  return (world != nullptr) ? world->content_epoch() : 0U;
}

double scripting_random_double(runtime::World *world) noexcept {
  if (world == nullptr) {
    return 0.0;
  }
  return core::rng_next_double(&world->random());
}

std::int64_t scripting_random_range(runtime::World *world,
                                    std::int64_t minimum,
                                    std::int64_t maximum) noexcept {
  if (world == nullptr) {
    return minimum;
  }
  return core::rng_range(&world->random(), minimum, maximum);
}

void scripting_seed_random(runtime::World *world,
                           std::uint64_t seed) noexcept {
  if (world != nullptr) {
    world->seed_random(seed);
  }
}

std::size_t scripting_alive_entity_count(runtime::World *world) noexcept {
  return (world != nullptr) ? world->alive_entity_count() : 0U;
}

runtime::Entity scripting_find_entity_by_index(runtime::World *world,
                                               std::uint32_t index) noexcept {
  return (world != nullptr) ? world->find_entity_by_index(index)
                            : runtime::kInvalidEntity;
}

runtime::Entity scripting_find_entity_by_name(runtime::World *world,
                                              const char *name) noexcept {
  return ((world != nullptr) && (name != nullptr))
             ? world->find_entity_by_name(name)
             : runtime::kInvalidEntity;
}

runtime::Entity scripting_find_entity_by_persistent_id(
    runtime::World *world, runtime::PersistentId persistentId) noexcept {
  return (world != nullptr) ? world->find_entity_by_persistent_id(persistentId)
                            : runtime::kInvalidEntity;
}

runtime::PersistentId scripting_persistent_id(runtime::World *world,
                                              runtime::Entity entity) noexcept {
  return (world != nullptr) ? world->persistent_id(entity)
                            : runtime::kInvalidPersistentId;
}

runtime::Entity
scripting_create_scene_object_op(runtime::World *world,
                                 const runtime::Transform *transform) noexcept {
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }
  return (transform != nullptr) ? world->create_scene_object(*transform)
                                : world->create_scene_object();
}

void scripting_for_each_alive(runtime::World *world,
                              scripting::EntityVisitFn visit,
                              void *context) noexcept {
  if ((world == nullptr) || (visit == nullptr)) {
    return;
  }
  world->for_each_alive(
      [visit, context](runtime::Entity entity) noexcept { visit(entity, context); });
}

void scripting_for_each_child(runtime::World *world, runtime::Entity parent,
                              scripting::EntityVisitFn visit,
                              void *context) noexcept {
  if ((world == nullptr) || (visit == nullptr)) {
    return;
  }
  world->for_each_child(parent, [visit, context](runtime::Entity child) noexcept {
    visit(child, context);
  });
}

void scripting_for_each_subtree_member(runtime::World *world,
                                       runtime::Entity root,
                                       scripting::EntityVisitFn visit,
                                       void *context) noexcept {
  if ((world == nullptr) || (visit == nullptr)) {
    return;
  }
  world->for_each_subtree_member(
      root, [visit, context](runtime::Entity member) noexcept {
        visit(member, context);
      });
}

void scripting_for_each_needs_begin_play(runtime::World *world,
                                         scripting::EntityVisitFn visit,
                                         void *context) noexcept {
  if ((world == nullptr) || (visit == nullptr)) {
    return;
  }
  world->for_each_needs_begin_play(
      [visit, context](runtime::Entity entity) noexcept { visit(entity, context); });
}

void scripting_for_each_pending_destroy(runtime::World *world,
                                        scripting::EntityVisitFn visit,
                                        void *context) noexcept {
  if ((world == nullptr) || (visit == nullptr)) {
    return;
  }
  world->for_each_pending_destroy(
      [visit, context](runtime::Entity entity) noexcept { visit(entity, context); });
}

void scripting_for_each_scripted_entity(runtime::World *world,
                                        scripting::ScriptedEntityVisitFn visit,
                                        void *context) noexcept {
  if ((world == nullptr) || (visit == nullptr)) {
    return;
  }
  world->for_each<runtime::ScriptComponent>(
      [visit, context](runtime::Entity entity,
                       const runtime::ScriptComponent &script) noexcept {
        visit(entity, script, context);
      });
}

bool scripting_has_begun_play(runtime::World *world,
                              runtime::Entity entity) noexcept {
  return (world != nullptr) && world->has_begun_play(entity);
}

void scripting_mark_begin_play_done(runtime::World *world,
                                    runtime::Entity entity) noexcept {
  if (world != nullptr) {
    world->mark_begin_play_done(entity);
  }
}

/// Writes "<source> (clone)" into destination, truncating the prefix rather
/// than the suffix so the clone marker always survives.
void copy_clone_name(char *destination, std::size_t destinationSize,
                     const char *source) noexcept {
  constexpr const char *kCloneSuffix = " (clone)";
  constexpr std::size_t kCloneSuffixLength = 8U;

  if ((destination == nullptr) || (destinationSize == 0U)) {
    return;
  }

  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  const std::size_t maxPrefixLength =
      (destinationSize > (kCloneSuffixLength + 1U))
          ? (destinationSize - kCloneSuffixLength - 1U)
          : 0U;
  const std::size_t sourceLength = std::strlen(source);
  const std::size_t prefixLength =
      (sourceLength < maxPrefixLength) ? sourceLength : maxPrefixLength;
  if (prefixLength > 0U) {
    std::memcpy(destination, source, prefixLength);
  }
  if ((prefixLength + kCloneSuffixLength) < destinationSize) {
    std::memcpy(destination + prefixLength, kCloneSuffix, kCloneSuffixLength);
    destination[prefixLength + kCloneSuffixLength] = '\0';
  }
}

/// Copies one persistent component when the source carries it; false only
/// when a present component fails to install on the clone.
template <typename Component>
bool clone_component(
    runtime::World &world, runtime::Entity source, runtime::Entity clone,
    bool (runtime::World::*getComponent)(runtime::Entity, Component *)
        const noexcept,
    bool (runtime::World::*addComponent)(runtime::Entity,
                                         const Component &) noexcept) noexcept {
  Component component{};
  if ((world.*getComponent)(source, &component) &&
      !(world.*addComponent)(clone, component)) {
    return false;
  }
  return true;
}

/// Clones every persistent component type through the authoritative
/// registry, so a component added to the World is cloned by construction
/// instead of being forgotten by a hand-maintained list. The clone is
/// transactional: the first failed component copy destroys the partial
/// clone so no half-built entity is ever handed back.
runtime::Entity scripting_clone_entity_op(runtime::World *world,
                                          runtime::Entity source) noexcept {
  if ((world == nullptr) || !world->is_alive(source)) {
    return runtime::kInvalidEntity;
  }

  const runtime::Entity clone = world->create_scene_object();
  if (clone == runtime::kInvalidEntity) {
    return runtime::kInvalidEntity;
  }

  bool success = true;
  const auto copy = [&](auto getComponent, auto addComponent) noexcept {
    return clone_component(*world, source, clone, getComponent, addComponent);
  };
#define ENGINE_PCR_CLONE_COMPONENT(Type, Key, GetFn, AddFn, RemoveFn)          \
  success = success && copy(&runtime::World::GetFn, &runtime::World::AddFn);
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_CLONE_COMPONENT)
#undef ENGINE_PCR_CLONE_COMPONENT

  if (success) {
    runtime::NameComponent name{};
    if (world->get_name_component(source, &name)) {
      runtime::NameComponent cloneName{};
      copy_clone_name(cloneName.name, sizeof(cloneName.name), name.name);
      success = world->add_name_component(clone, cloneName);
    }
  }

  if (!success) {
    static_cast<void>(world->destroy_entity(clone));
    core::log_message(core::LogLevel::Warning, "runtime",
                      "clone_entity rolled back: component copy failed");
    return runtime::kInvalidEntity;
  }
  return clone;
}

// Component reads. Each forwards the handle unchanged; the World's own
// liveness check refuses a stale one.
const runtime::Transform *
scripting_get_transform_read_ptr(runtime::World *world,
                                 runtime::Entity entity) noexcept {
  return (world != nullptr) ? world->get_transform_read_ptr(entity) : nullptr;
}

bool scripting_get_transform_op(runtime::World *world, runtime::Entity entity,
                                runtime::Transform *outTransform) noexcept {
  return (world != nullptr) && (outTransform != nullptr) &&
         world->get_transform(entity, outTransform);
}

bool scripting_get_rigid_body_op(runtime::World *world, runtime::Entity entity,
                                 runtime::RigidBody *outRigidBody) noexcept {
  return (world != nullptr) && (outRigidBody != nullptr) &&
         world->get_rigid_body(entity, outRigidBody);
}

const runtime::MeshComponent *
scripting_get_mesh_component_ptr(runtime::World *world,
                                 runtime::Entity entity) noexcept {
  return (world != nullptr) ? world->get_mesh_component_ptr(entity) : nullptr;
}

bool scripting_get_mesh_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::MeshComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_mesh_component(entity, outComponent);
}

bool scripting_get_name_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::NameComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_name_component(entity, outComponent);
}

bool scripting_get_collider_op(runtime::World *world, runtime::Entity entity,
                               runtime::Collider *outCollider) noexcept {
  return (world != nullptr) && (outCollider != nullptr) &&
         world->get_collider(entity, outCollider);
}

bool scripting_get_light_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::LightComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_light_component(entity, outComponent);
}

bool scripting_has_light_component(runtime::World *world,
                                   runtime::Entity entity) noexcept {
  return (world != nullptr) && world->has_light_component(entity);
}

bool scripting_get_point_light_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::PointLightComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_point_light_component(entity, outComponent);
}

bool scripting_get_spot_light_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::SpotLightComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_spot_light_component(entity, outComponent);
}

bool scripting_get_script_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::ScriptComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_script_component(entity, outComponent);
}

bool scripting_get_spring_arm_op(
    runtime::World *world, runtime::Entity entity,
    runtime::SpringArmComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_spring_arm(entity, outComponent);
}

bool scripting_get_camera_component_op(
    runtime::World *world, runtime::Entity entity,
    runtime::CameraComponent *outComponent) noexcept {
  return (world != nullptr) && (outComponent != nullptr) &&
         world->get_camera_component(entity, outComponent);
}

bool scripting_has_convex_hull_payload(runtime::World *world,
                                       runtime::Entity entity) noexcept {
  return (world != nullptr) && world->has_convex_hull_payload(entity);
}

// World mutation operations (also called from the deferred mutation queue).
bool scripting_destroy_entity_op(runtime::World *world,
                                 runtime::Entity entity) noexcept {
  return (world != nullptr) && world->destroy_entity(entity);
}

bool scripting_add_transform_op(runtime::World *world, runtime::Entity entity,
                                const runtime::Transform &transform) noexcept {
  return (world != nullptr) && world->add_transform(entity, transform);
}

bool scripting_set_movement_authority_op(
    runtime::World *world, runtime::Entity entity,
    runtime::MovementAuthority authority) noexcept {
  return (world != nullptr) && world->set_movement_authority(entity, authority);
}

bool scripting_add_rigid_body_op(runtime::World *world, runtime::Entity entity,
                                 const runtime::RigidBody &rigidBody) noexcept {
  return (world != nullptr) && world->add_rigid_body(entity, rigidBody);
}

bool scripting_add_collider_op(runtime::World *world, runtime::Entity entity,
                               const runtime::Collider &collider) noexcept {
  return (world != nullptr) && world->add_collider(entity, collider);
}

bool scripting_add_mesh_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::MeshComponent &component) noexcept {
  return (world != nullptr) && world->add_mesh_component(entity, component);
}

bool scripting_add_name_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::NameComponent &component) noexcept {
  return (world != nullptr) && world->add_name_component(entity, component);
}

bool scripting_add_light_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::LightComponent &component) noexcept {
  return (world != nullptr) && world->add_light_component(entity, component);
}

bool scripting_remove_light_component_op(runtime::World *world,
                                         runtime::Entity entity) noexcept {
  return (world != nullptr) && world->remove_light_component(entity);
}

bool scripting_add_point_light_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::PointLightComponent &component) noexcept {
  return (world != nullptr) &&
         world->add_point_light_component(entity, component);
}

bool scripting_remove_point_light_component_op(
    runtime::World *world, runtime::Entity entity) noexcept {
  return (world != nullptr) && world->remove_point_light_component(entity);
}

bool scripting_add_spot_light_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::SpotLightComponent &component) noexcept {
  return (world != nullptr) &&
         world->add_spot_light_component(entity, component);
}

bool scripting_remove_spot_light_component_op(
    runtime::World *world, runtime::Entity entity) noexcept {
  return (world != nullptr) && world->remove_spot_light_component(entity);
}

bool scripting_add_script_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::ScriptComponent &component) noexcept {
  return (world != nullptr) && world->add_script_component(entity, component);
}

bool scripting_remove_script_component_op(runtime::World *world,
                                          runtime::Entity entity) noexcept {
  return (world != nullptr) && world->remove_script_component(entity);
}

bool scripting_add_spring_arm_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::SpringArmComponent &component) noexcept {
  return (world != nullptr) && world->add_spring_arm(entity, component);
}

bool scripting_add_camera_component_op(
    runtime::World *world, runtime::Entity entity,
    const runtime::CameraComponent &component) noexcept {
  return (world != nullptr) && world->add_camera_component(entity, component);
}

bool scripting_remove_camera_component_op(runtime::World *world,
                                          runtime::Entity entity) noexcept {
  return (world != nullptr) && world->remove_camera_component(entity);
}

bool scripting_apply_primitive_hull(math::HullSource source,
                                    runtime::Collider *collider) noexcept {
  return (collider != nullptr) && runtime::apply_primitive_hull(source, collider);
}

// Game mode, owned by the World.
const char *scripting_game_mode_name(runtime::World *world) noexcept {
  return (world != nullptr) ? world->game_mode().name : "";
}

bool scripting_set_game_mode_name(runtime::World *world,
                                  const char *name) noexcept {
  if ((world == nullptr) || (name == nullptr)) {
    return false;
  }
  std::snprintf(world->game_mode().name, runtime::GameMode::kMaxNameLength,
                "%s", name);
  return true;
}

bool scripting_game_mode_start(runtime::World *world) noexcept {
  return (world != nullptr) && world->game_mode().start();
}

bool scripting_game_mode_pause(runtime::World *world) noexcept {
  return (world != nullptr) && world->game_mode().pause();
}

bool scripting_game_mode_end(runtime::World *world) noexcept {
  return (world != nullptr) && world->game_mode().end();
}

scripting::GameModeState
scripting_game_mode_state(runtime::World *world) noexcept {
  if (world == nullptr) {
    return scripting::GameModeState::WaitingToStart;
  }
  return static_cast<scripting::GameModeState>(world->game_mode().state);
}

bool scripting_game_mode_set_rule(runtime::World *world, const char *key,
                                  const char *value) noexcept {
  return (world != nullptr) && world->game_mode().set_rule(key, value);
}

const char *scripting_game_mode_get_rule(runtime::World *world,
                                         const char *key) noexcept {
  return (world != nullptr) ? world->game_mode().get_rule(key) : nullptr;
}

std::uint32_t scripting_game_mode_max_players(runtime::World *world) noexcept {
  return (world != nullptr) ? world->game_mode().maxPlayers : 0U;
}

void scripting_set_game_mode_max_players(runtime::World *world,
                                         std::uint32_t maxPlayers) noexcept {
  if (world != nullptr) {
    world->game_mode().maxPlayers = maxPlayers;
  }
}

// Timers, owned by the World.
std::uint32_t scripting_timer_set(runtime::World *world, float seconds,
                                  bool repeat,
                                  scripting::TimerCallbackFn callback,
                                  void *userData) noexcept {
  if ((world == nullptr) || (callback == nullptr)) {
    return runtime::kInvalidTimerId;
  }
  runtime::TimerManager &timers = world->timer_manager();
  return repeat ? timers.set_interval(seconds, callback, userData)
                : timers.set_timeout(seconds, callback, userData);
}

void scripting_timer_cancel(runtime::World *world,
                            std::uint32_t timerId) noexcept {
  if (world != nullptr) {
    world->timer_manager().cancel(timerId);
  }
}

std::size_t scripting_timer_slot_for_id(runtime::World *world,
                                        std::uint32_t timerId) noexcept {
  return (world != nullptr) ? world->timer_manager().slot_for_id(timerId)
                            : runtime::TimerManager::kInvalidTimerSlot;
}

bool scripting_timer_slot_state(runtime::World *world, std::size_t slot,
                                bool *outRepeat, bool *outActive) noexcept {
  if ((world == nullptr) || (slot >= runtime::TimerManager::kMaxTimers) ||
      (outRepeat == nullptr) || (outActive == nullptr)) {
    return false;
  }
  const runtime::TimerManager::Entry &entry =
      world->timer_manager().entry_at(slot);
  *outRepeat = entry.repeat;
  *outActive = entry.active;
  return true;
}

void scripting_timer_clear(runtime::World *world) noexcept {
  if (world != nullptr) {
    world->timer_manager().clear();
  }
}

std::size_t scripting_timer_advance(runtime::World *world,
                                    float deltaSeconds) noexcept {
  return (world != nullptr) ? world->timer_manager().advance(deltaSeconds) : 0U;
}

std::size_t scripting_timer_dispatch(runtime::World *world) noexcept {
  return (world != nullptr) ? world->timer_manager().dispatch() : 0U;
}

// Entity pools the Lua pool bindings address by slot. Each pool records
// the World it was seeded from and expires with that World's contents.
runtime::EntityPool g_scriptEntityPools[scripting::kMaxEntityPools]{};

bool scripting_entity_pool_init(runtime::World *world, std::size_t slot,
                                std::size_t count) noexcept {
  if ((world == nullptr) || (slot >= scripting::kMaxEntityPools)) {
    return false;
  }
  return g_scriptEntityPools[slot].init(world, count);
}

runtime::Entity scripting_entity_pool_acquire(runtime::World *world,
                                              std::size_t slot) noexcept {
  if ((world == nullptr) || (slot >= scripting::kMaxEntityPools)) {
    return runtime::kInvalidEntity;
  }
  return g_scriptEntityPools[slot].acquire();
}

bool scripting_entity_pool_release(runtime::World *world, std::size_t slot,
                                   runtime::Entity entity) noexcept {
  if ((world == nullptr) || (slot >= scripting::kMaxEntityPools)) {
    return false;
  }
  return g_scriptEntityPools[slot].release(entity);
}

void scripting_entity_pool_reset_all() noexcept {
  for (runtime::EntityPool &pool : g_scriptEntityPools) {
    pool = runtime::EntityPool{};
  }
}

/// Assembles the table by member name so a reordered or added operation
/// can never be bound to the wrong slot.
scripting::RuntimeServices make_scripting_runtime_services() noexcept {
  scripting::RuntimeServices s{};
  s.set_camera_position = &scripting_set_camera_position;
  s.set_camera_target = &scripting_set_camera_target;
  s.set_camera_up = &scripting_set_camera_up;
  s.set_camera_fov = &scripting_set_camera_fov;
  s.push_camera_op = &scripting_push_camera;
  s.pop_camera_op = &scripting_pop_camera;
  s.get_active_camera_op = &scripting_get_active_camera;
  s.camera_shake_op = &scripting_camera_shake;
  s.is_input_phase = &scripting_is_input_phase;
  s.is_alive = &scripting_is_alive;
  s.content_epoch = &scripting_content_epoch;
  s.alive_entity_count = &scripting_alive_entity_count;
  s.random_double = &scripting_random_double;
  s.random_range = &scripting_random_range;
  s.seed_random = &scripting_seed_random;
  s.find_entity_by_index = &scripting_find_entity_by_index;
  s.find_entity_by_name = &scripting_find_entity_by_name;
  s.find_entity_by_persistent_id = &scripting_find_entity_by_persistent_id;
  s.persistent_id = &scripting_persistent_id;
  s.create_scene_object_op = &scripting_create_scene_object_op;
  s.clone_entity_op = &scripting_clone_entity_op;
  s.for_each_alive = &scripting_for_each_alive;
  s.for_each_child = &scripting_for_each_child;
  s.for_each_subtree_member = &scripting_for_each_subtree_member;
  s.for_each_needs_begin_play = &scripting_for_each_needs_begin_play;
  s.for_each_pending_destroy = &scripting_for_each_pending_destroy;
  s.for_each_scripted_entity = &scripting_for_each_scripted_entity;
  s.has_begun_play = &scripting_has_begun_play;
  s.mark_begin_play_done = &scripting_mark_begin_play_done;
  s.get_transform_read_ptr = &scripting_get_transform_read_ptr;
  s.get_transform_op = &scripting_get_transform_op;
  s.get_rigid_body_op = &scripting_get_rigid_body_op;
  s.get_mesh_component_ptr = &scripting_get_mesh_component_ptr;
  s.get_mesh_component_op = &scripting_get_mesh_component_op;
  s.get_name_component_op = &scripting_get_name_component_op;
  s.get_collider_op = &scripting_get_collider_op;
  s.get_light_component_op = &scripting_get_light_component_op;
  s.has_light_component = &scripting_has_light_component;
  s.get_point_light_component_op = &scripting_get_point_light_component_op;
  s.get_spot_light_component_op = &scripting_get_spot_light_component_op;
  s.get_script_component_op = &scripting_get_script_component_op;
  s.get_spring_arm_op = &scripting_get_spring_arm_op;
  s.get_camera_component_op = &scripting_get_camera_component_op;
  s.has_convex_hull_payload = &scripting_has_convex_hull_payload;
  s.destroy_entity_op = &scripting_destroy_entity_op;
  s.add_transform_op = &scripting_add_transform_op;
  s.set_movement_authority_op = &scripting_set_movement_authority_op;
  s.add_rigid_body_op = &scripting_add_rigid_body_op;
  s.add_collider_op = &scripting_add_collider_op;
  s.add_mesh_component_op = &scripting_add_mesh_component_op;
  s.asset_ref_for_id = &scripting_asset_ref_for_id;
  s.add_name_component_op = &scripting_add_name_component_op;
  s.add_light_component_op = &scripting_add_light_component_op;
  s.remove_light_component_op = &scripting_remove_light_component_op;
  s.add_point_light_component_op = &scripting_add_point_light_component_op;
  s.remove_point_light_component_op =
      &scripting_remove_point_light_component_op;
  s.add_spot_light_component_op = &scripting_add_spot_light_component_op;
  s.remove_spot_light_component_op = &scripting_remove_spot_light_component_op;
  s.add_script_component_op = &scripting_add_script_component_op;
  s.remove_script_component_op = &scripting_remove_script_component_op;
  s.add_spring_arm_op = &scripting_add_spring_arm_op;
  s.add_camera_component_op = &scripting_add_camera_component_op;
  s.remove_camera_component_op = &scripting_remove_camera_component_op;
  s.apply_primitive_hull = &scripting_apply_primitive_hull;
  s.game_mode_name = &scripting_game_mode_name;
  s.set_game_mode_name = &scripting_set_game_mode_name;
  s.game_mode_start = &scripting_game_mode_start;
  s.game_mode_pause = &scripting_game_mode_pause;
  s.game_mode_end = &scripting_game_mode_end;
  s.game_mode_state = &scripting_game_mode_state;
  s.game_mode_set_rule = &scripting_game_mode_set_rule;
  s.game_mode_get_rule = &scripting_game_mode_get_rule;
  s.game_mode_max_players = &scripting_game_mode_max_players;
  s.set_game_mode_max_players = &scripting_set_game_mode_max_players;
  s.timer_set = &scripting_timer_set;
  s.timer_cancel = &scripting_timer_cancel;
  s.timer_slot_for_id = &scripting_timer_slot_for_id;
  s.timer_slot_state = &scripting_timer_slot_state;
  s.timer_clear = &scripting_timer_clear;
  s.timer_advance = &scripting_timer_advance;
  s.timer_dispatch = &scripting_timer_dispatch;
  s.entity_pool_init = &scripting_entity_pool_init;
  s.entity_pool_acquire = &scripting_entity_pool_acquire;
  s.entity_pool_release = &scripting_entity_pool_release;
  s.entity_pool_reset_all = &scripting_entity_pool_reset_all;
  s.set_gravity = &scripting_set_gravity;
  s.get_gravity = &scripting_get_gravity;
  s.raycast = &scripting_raycast;
  s.raycast_all = &scripting_raycast_all;
  s.overlap_sphere = &scripting_overlap_sphere;
  s.overlap_box = &scripting_overlap_box;
  s.sweep_sphere = &scripting_sweep_sphere;
  s.sweep_box = &scripting_sweep_box;
  s.add_distance_joint = &scripting_add_distance_joint;
  s.add_hinge_joint = &scripting_add_hinge_joint;
  s.add_ball_socket_joint = &scripting_add_ball_socket_joint;
  s.add_slider_joint = &scripting_add_slider_joint;
  s.add_spring_joint = &scripting_add_spring_joint;
  s.add_fixed_joint = &scripting_add_fixed_joint;
  s.set_joint_limits = &scripting_set_joint_limits;
  s.remove_joint = &scripting_remove_joint;
  s.wake_body = &scripting_wake_body;
  s.is_sleeping = &scripting_is_sleeping;
  s.load_sound = &scripting_load_sound;
  s.unload_sound = &scripting_unload_sound;
  s.play_sound = &scripting_play_sound;
  s.stop_sound = &scripting_stop_sound;
  s.stop_all_sounds = &scripting_stop_all_sounds;
  s.set_master_volume = &scripting_set_master_volume;
  s.play_sound_at = &scripting_play_sound_at;
  s.set_bus_volume = &scripting_set_bus_volume;
  s.play_music = &scripting_play_music;
  s.stop_music = &scripting_stop_music;
  s.save_game_data = &scripting_save_game_data;
  s.load_game_data = &scripting_load_game_data;
  s.save_scene = &scripting_save_scene;
  s.save_prefab = &scripting_save_prefab;
  s.instantiate_prefab = &scripting_instantiate_prefab;
  s.load_asset_async = &scripting_load_asset_async;
  s.is_asset_ready = &scripting_is_asset_ready;
  return s;
}

const scripting::RuntimeServices kScriptingRuntimeServices =
    make_scripting_runtime_services();

} // namespace

namespace runtime {

/// Binds scripting runtime pointers into an explicit service locator.
void bind_scripting_runtime(World *world,
                            core::ServiceLocator &locator) noexcept {
  scripting::bind_runtime_world(world, locator);
  g_scriptingAssetDatabaseService =
      locator.get_service<runtime::EngineAssetDatabaseService>();
  scripting::bind_runtime_services(&kScriptingRuntimeServices, locator);

  scripting::AnimationScriptBridge animationBridge{};
  animationBridge.queueParam = [](core::Entity entity, const char *name,
                                  float value) noexcept {
    return queue_anim_param(entity, name, value);
  };
  animationBridge.firedEventCount = []() noexcept {
    return fired_anim_event_count();
  };
  animationBridge.firedEventAt = [](std::size_t index,
                                    core::Entity *outEntity,
                                    const char **outName) noexcept {
    const FiredAnimEvent *event = fired_anim_event_at(index);
    if ((event == nullptr) || (outEntity == nullptr) ||
        (outName == nullptr)) {
      return false;
    }
    *outEntity = event->entity;
    *outName = event->name;
    return true;
  };
  scripting::set_animation_script_bridge(animationBridge);
}

/// Clears scripting runtime bindings from an explicit service locator.
void unbind_scripting_runtime(core::ServiceLocator &locator) noexcept {
  g_scriptingAssetDatabaseService = nullptr;
  scripting::bind_runtime_world(nullptr, locator);
  scripting::bind_runtime_services(nullptr, locator);
  scripting::set_animation_script_bridge(scripting::AnimationScriptBridge{});
}

} // namespace runtime

} // namespace engine
