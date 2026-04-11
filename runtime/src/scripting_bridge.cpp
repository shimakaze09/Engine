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

// World query operations needed by Lua bindings
runtime::WorldPhase
scripting_get_current_phase(runtime::World *world) noexcept {
  if (world == nullptr) {
    return runtime::WorldPhase::Input;
  }
  return world->current_phase();
}

std::uint32_t scripting_get_entity_index(runtime::World *world,
                                         std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return 0U;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return (entity != runtime::kInvalidEntity) ? entity.index : 0U;
}

std::uint32_t scripting_get_transform_count(runtime::World *world) noexcept {
  if (world == nullptr) {
    return 0U;
  }

  const std::size_t count = world->transform_count();
  return static_cast<std::uint32_t>(count);
}

std::uint32_t scripting_create_entity_op(runtime::World *world) noexcept {
  if (world == nullptr) {
    return 0U;
  }
  const runtime::Entity entity = world->create_entity();
  return entity.index;
}

const runtime::Transform *
scripting_get_transform_read_ptr(runtime::World *world,
                                 std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return nullptr;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_transform_read_ptr(entity);
}

bool scripting_get_transform_op(runtime::World *world,
                                std::uint32_t entityIndex,
                                runtime::Transform *outTransform) noexcept {
  if ((world == nullptr) || (outTransform == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_transform(entity, outTransform);
}

bool scripting_get_rigid_body_op(runtime::World *world,
                                 std::uint32_t entityIndex,
                                 runtime::RigidBody *outRigidBody) noexcept {
  if ((world == nullptr) || (outRigidBody == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_rigid_body(entity, outRigidBody);
}

const runtime::MeshComponent *
scripting_get_mesh_component_ptr(runtime::World *world,
                                 std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return nullptr;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_mesh_component_ptr(entity);
}

bool scripting_get_mesh_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    runtime::MeshComponent *outComponent) noexcept {
  if ((world == nullptr) || (outComponent == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_mesh_component(entity, outComponent);
}

bool scripting_get_name_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    runtime::NameComponent *outComponent) noexcept {
  if ((world == nullptr) || (outComponent == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_name_component(entity, outComponent);
}

bool scripting_get_collider_op(runtime::World *world, std::uint32_t entityIndex,
                               runtime::Collider *outCollider) noexcept {
  if ((world == nullptr) || (outCollider == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_collider(entity, outCollider);
}

// World mutation operations (called from deferred mutation queue)
bool scripting_destroy_entity_op(runtime::World *world,
                                 std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->destroy_entity(entity);
}

bool scripting_add_transform_op(runtime::World *world,
                                std::uint32_t entityIndex,
                                const runtime::Transform &transform) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_transform(entity, transform);
}

bool scripting_set_movement_authority_op(
    runtime::World *world, std::uint32_t entityIndex,
    runtime::MovementAuthority authority) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->set_movement_authority(entity, authority);
}

bool scripting_add_rigid_body_op(runtime::World *world,
                                 std::uint32_t entityIndex,
                                 const runtime::RigidBody &rigidBody) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_rigid_body(entity, rigidBody);
}

bool scripting_add_collider_op(runtime::World *world, std::uint32_t entityIndex,
                               const runtime::Collider &collider) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_collider(entity, collider);
}

bool scripting_add_mesh_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::MeshComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_mesh_component(entity, component);
}

bool scripting_add_name_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::NameComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_name_component(entity, component);
}

bool scripting_add_light_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::LightComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_light_component(entity, component);
}

bool scripting_remove_light_component_op(runtime::World *world,
                                         std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->remove_light_component(entity);
}

bool scripting_add_script_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::ScriptComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_script_component(entity, component);
}

bool scripting_remove_script_component_op(runtime::World *world,
                                          std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->remove_script_component(entity);
}

const scripting::RuntimeServices kScriptingRuntimeServices = {
    &scripting_set_camera_position,
    &scripting_set_camera_target,
    &scripting_set_camera_up,
    &scripting_set_camera_fov,
    &scripting_get_current_phase,
    &scripting_get_entity_index,
    &scripting_get_transform_count,
    &scripting_create_entity_op,
    &scripting_get_transform_read_ptr,
    &scripting_get_transform_op,
    &scripting_get_rigid_body_op,
    &scripting_get_mesh_component_ptr,
    &scripting_get_mesh_component_op,
    &scripting_get_name_component_op,
    &scripting_get_collider_op,
    &scripting_destroy_entity_op,
    &scripting_add_transform_op,
    &scripting_set_movement_authority_op,
    &scripting_add_rigid_body_op,
    &scripting_add_collider_op,
    &scripting_add_mesh_component_op,
    &scripting_add_name_component_op,
    &scripting_add_light_component_op,
    &scripting_remove_light_component_op,
    &scripting_add_script_component_op,
    &scripting_remove_script_component_op,
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