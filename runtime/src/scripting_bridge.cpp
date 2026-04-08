#include "engine/runtime/scripting_bridge.h"

#include "engine/audio/audio.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

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

bool scripting_raycast(runtime::World *world, float ox, float oy, float oz,
                       float dx, float dy, float dz, float maxDistance,
                       scripting::RuntimeRaycastHit *outHit) noexcept {
  if ((world == nullptr) || (outHit == nullptr)) {
    return false;
  }

  runtime::PhysicsRaycastHit hit{};
  if (!runtime::raycast(*world, math::Vec3(ox, oy, oz), math::Vec3(dx, dy, dz),
                        maxDistance, &hit)) {
    return false;
  }

  outHit->entityIndex = hit.entity.index;
  outHit->distance = hit.distance;
  outHit->pointX = hit.point.x;
  outHit->pointY = hit.point.y;
  outHit->pointZ = hit.point.z;
  outHit->normalX = hit.normal.x;
  outHit->normalY = hit.normal.y;
  outHit->normalZ = hit.normal.z;
  return true;
}

std::uint32_t scripting_add_distance_joint(runtime::World *world,
                                           std::uint32_t entityIndexA,
                                           std::uint32_t entityIndexB,
                                           float distance) noexcept {
  if ((world == nullptr) || (entityIndexA == 0U) || (entityIndexB == 0U)) {
    return 0U;
  }

  const runtime::Entity entityA = world->find_entity_by_index(entityIndexA);
  const runtime::Entity entityB = world->find_entity_by_index(entityIndexB);
  return static_cast<std::uint32_t>(
      runtime::add_distance_joint(*world, entityA, entityB, distance));
}

void scripting_remove_joint(runtime::World *world,
                            std::uint32_t jointId) noexcept {
  if (world == nullptr) {
    return;
  }
  runtime::remove_joint(*world, static_cast<physics::JointId>(jointId));
}

void scripting_wake_body(runtime::World *world,
                         std::uint32_t entityIndex) noexcept {
  if ((world == nullptr) || (entityIndex == 0U)) {
    return;
  }

  runtime::wake_body(*world, world->find_entity_by_index(entityIndex));
}

bool scripting_is_sleeping(runtime::World *world,
                           std::uint32_t entityIndex) noexcept {
  if ((world == nullptr) || (entityIndex == 0U)) {
    return false;
  }

  return runtime::is_sleeping(*world, world->find_entity_by_index(entityIndex));
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

bool scripting_save_scene(const runtime::World *world,
                          const char *path) noexcept {
  return (world != nullptr) && runtime::save_scene(*world, path);
}

bool scripting_save_prefab(const runtime::World *world,
                           std::uint32_t entityIndex,
                           const char *path) noexcept {
  if ((world == nullptr) || (entityIndex == 0U)) {
    return false;
  }

  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return (entity != runtime::kInvalidEntity) &&
         runtime::save_prefab(*world, entity, path);
}

std::uint32_t scripting_instantiate_prefab(runtime::World *world,
                                           const char *path) noexcept {
  if (world == nullptr) {
    return 0U;
  }

  const runtime::Entity entity = runtime::instantiate_prefab(*world, path);
  return entity.index;
}

const scripting::RuntimeServices kScriptingRuntimeServices = {
    &scripting_set_camera_position,
    &scripting_set_camera_target,
    &scripting_set_camera_up,
    &scripting_set_camera_fov,
    &scripting_set_gravity,
    &scripting_get_gravity,
    &scripting_raycast,
    &scripting_add_distance_joint,
    &scripting_remove_joint,
    &scripting_wake_body,
    &scripting_is_sleeping,
    &scripting_load_sound,
    &scripting_unload_sound,
    &scripting_play_sound,
    &scripting_stop_sound,
    &scripting_stop_all_sounds,
    &scripting_set_master_volume,
    &scripting_save_scene,
    &scripting_save_prefab,
    &scripting_instantiate_prefab,
};

} // namespace

namespace runtime {

void bind_scripting_runtime(World *world) noexcept {
  scripting::bind_runtime_world(world);
  scripting::bind_runtime_services(&kScriptingRuntimeServices);
}

} // namespace runtime

} // namespace engine