// Implements scripting bridge behavior for the Engine runtime world.

#include "engine/runtime/scripting_bridge.h"

#include "engine/audio/audio.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_query.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

/// Handles scripting set camera position.
void scripting_set_camera_position(float x, float y, float z) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.position = math::Vec3(x, y, z);
  renderer::set_active_camera(camera);
}

/// Handles scripting set camera target.
void scripting_set_camera_target(float x, float y, float z) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.target = math::Vec3(x, y, z);
  renderer::set_active_camera(camera);
}

/// Handles scripting set camera up.
void scripting_set_camera_up(float x, float y, float z) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.up = math::Vec3(x, y, z);
  renderer::set_active_camera(camera);
}

/// Handles scripting set camera fov.
void scripting_set_camera_fov(float fovRadians) noexcept {
  renderer::CameraState camera = renderer::get_active_camera();
  camera.fovRadians = fovRadians;
  renderer::set_active_camera(camera);
}

// Camera manager bridge functions
bool scripting_push_camera(runtime::World *world, std::uint32_t entityIndex,
                           float posX, float posY, float posZ, float tgtX,
                           float tgtY, float tgtZ, float priority,
                           float blendSpeed) noexcept {
  if (world == nullptr) {
    return false;
  }
  runtime::CameraEntry entry{};
  entry.position = math::Vec3(posX, posY, posZ);
  entry.target = math::Vec3(tgtX, tgtY, tgtZ);
  entry.blendSpeed = blendSpeed;
  return world->camera_manager().push_camera(entityIndex, entry, priority);
}

/// Handles scripting pop camera.
bool scripting_pop_camera(runtime::World *world,
                          std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return false;
  }
  return world->camera_manager().pop_camera(entityIndex);
}

/// Handles scripting get active camera.
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

/// Handles scripting camera shake.
bool scripting_camera_shake(runtime::World *world, float amplitude,
                            float frequency, float duration,
                            float decay) noexcept {
  if (world == nullptr) {
    return false;
  }
  return world->camera_manager().add_shake(amplitude, frequency, duration,
                                           decay);
}

/// Handles scripting set gravity.
void scripting_set_gravity(runtime::World *world, float x, float y,
                           float z) noexcept {
  if (world == nullptr) {
    return;
  }

  runtime::set_gravity(*world, x, y, z);
}

/// Handles scripting get gravity.
bool scripting_get_gravity(runtime::World *world, float *outX, float *outY,
                           float *outZ) noexcept {
  if ((world == nullptr) || (outX == nullptr) || (outY == nullptr) ||
      (outZ == nullptr)) {
    return false;
  }

  return runtime::get_gravity(*world, outX, outY, outZ);
}

/// Handles scripting raycast.
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

/// Handles scripting raycast all.
std::size_t scripting_raycast_all(runtime::World *world, float ox, float oy,
                                  float oz, float dx, float dy, float dz,
                                  float maxDistance,
                                  scripting::RuntimeRaycastHit *outHits,
                                  std::size_t maxHits,
                                  std::uint32_t mask) noexcept {
  if ((world == nullptr) || (outHits == nullptr) || (maxHits == 0U)) {
    return 0U;
  }
  constexpr std::size_t kLocalMax = 32U;
  const std::size_t cap = maxHits < kLocalMax ? maxHits : kLocalMax;
  runtime::PhysicsRaycastHit hits[kLocalMax]{};
  const std::size_t count = runtime::raycast_all(*world, math::Vec3(ox, oy, oz),
                                                 math::Vec3(dx, dy, dz),
                                                 maxDistance, hits, cap, mask);
  for (std::size_t i = 0U; i < count; ++i) {
    outHits[i].entityIndex = hits[i].entity.index;
    outHits[i].distance = hits[i].distance;
    outHits[i].pointX = hits[i].point.x;
    outHits[i].pointY = hits[i].point.y;
    outHits[i].pointZ = hits[i].point.z;
    outHits[i].normalX = hits[i].normal.x;
    outHits[i].normalY = hits[i].normal.y;
    outHits[i].normalZ = hits[i].normal.z;
  }
  return count;
}

/// Handles scripting overlap sphere.
std::size_t scripting_overlap_sphere(runtime::World *world, float cx, float cy,
                                     float cz, float radius,
                                     std::uint32_t *outEntityIndices,
                                     std::size_t maxResults,
                                     std::uint32_t mask) noexcept {
  if (world == nullptr) {
    return 0U;
  }
  return runtime::overlap_sphere(*world, math::Vec3(cx, cy, cz), radius,
                                 outEntityIndices, maxResults, mask);
}

/// Handles scripting overlap box.
std::size_t scripting_overlap_box(runtime::World *world, float cx, float cy,
                                  float cz, float hx, float hy, float hz,
                                  std::uint32_t *outEntityIndices,
                                  std::size_t maxResults,
                                  std::uint32_t mask) noexcept {
  if (world == nullptr) {
    return 0U;
  }
  return runtime::overlap_box(*world, math::Vec3(cx, cy, cz),
                              math::Vec3(hx, hy, hz), outEntityIndices,
                              maxResults, mask);
}

/// Handles scripting sweep sphere.
bool scripting_sweep_sphere(runtime::World *world, float ox, float oy, float oz,
                            float radius, float dx, float dy, float dz,
                            float maxDistance,
                            scripting::RuntimeRaycastHit *outHit,
                            std::uint32_t mask) noexcept {
  if ((world == nullptr) || (outHit == nullptr)) {
    return false;
  }
  physics::SweepHit sh{};
  if (!runtime::sweep_sphere(*world, math::Vec3(ox, oy, oz), radius,
                             math::Vec3(dx, dy, dz), maxDistance, &sh, mask)) {
    return false;
  }
  outHit->entityIndex = sh.entityIndex;
  outHit->distance = sh.distance;
  outHit->pointX = sh.contactPoint.x;
  outHit->pointY = sh.contactPoint.y;
  outHit->pointZ = sh.contactPoint.z;
  outHit->normalX = sh.normal.x;
  outHit->normalY = sh.normal.y;
  outHit->normalZ = sh.normal.z;
  return true;
}

/// Handles scripting sweep box.
bool scripting_sweep_box(runtime::World *world, float cx, float cy, float cz,
                         float hx, float hy, float hz, float dx, float dy,
                         float dz, float maxDistance,
                         scripting::RuntimeRaycastHit *outHit,
                         std::uint32_t mask) noexcept {
  if ((world == nullptr) || (outHit == nullptr)) {
    return false;
  }
  physics::SweepHit sh{};
  if (!runtime::sweep_box(*world, math::Vec3(cx, cy, cz),
                          math::Vec3(hx, hy, hz), math::Vec3(dx, dy, dz),
                          maxDistance, &sh, mask)) {
    return false;
  }
  outHit->entityIndex = sh.entityIndex;
  outHit->distance = sh.distance;
  outHit->pointX = sh.contactPoint.x;
  outHit->pointY = sh.contactPoint.y;
  outHit->pointZ = sh.contactPoint.z;
  outHit->normalX = sh.normal.x;
  outHit->normalY = sh.normal.y;
  outHit->normalZ = sh.normal.z;
  return true;
}

/// Handles scripting add distance joint.
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

/// Handles scripting remove joint.
void scripting_remove_joint(runtime::World *world,
                            std::uint32_t jointId) noexcept {
  if (world == nullptr) {
    return;
  }
  runtime::remove_joint(*world, static_cast<physics::JointId>(jointId));
}

/// Handles scripting add hinge joint.
std::uint32_t scripting_add_hinge_joint(runtime::World *world,
                                        std::uint32_t entityIndexA,
                                        std::uint32_t entityIndexB,
                                        float pivotX, float pivotY,
                                        float pivotZ, float axisX, float axisY,
                                        float axisZ) noexcept {
  if ((world == nullptr) || (entityIndexA == 0U) || (entityIndexB == 0U)) {
    return 0U;
  }
  const runtime::Entity entityA = world->find_entity_by_index(entityIndexA);
  const runtime::Entity entityB = world->find_entity_by_index(entityIndexB);
  const math::Vec3 pivot(pivotX, pivotY, pivotZ);
  const math::Vec3 axis(axisX, axisY, axisZ);
  return static_cast<std::uint32_t>(
      runtime::add_hinge_joint(*world, entityA, entityB, pivot, axis));
}

/// Handles scripting add ball socket joint.
std::uint32_t scripting_add_ball_socket_joint(runtime::World *world,
                                              std::uint32_t entityIndexA,
                                              std::uint32_t entityIndexB,
                                              float pivotX, float pivotY,
                                              float pivotZ) noexcept {
  if ((world == nullptr) || (entityIndexA == 0U) || (entityIndexB == 0U)) {
    return 0U;
  }
  const runtime::Entity entityA = world->find_entity_by_index(entityIndexA);
  const runtime::Entity entityB = world->find_entity_by_index(entityIndexB);
  const math::Vec3 pivot(pivotX, pivotY, pivotZ);
  return static_cast<std::uint32_t>(
      runtime::add_ball_socket_joint(*world, entityA, entityB, pivot));
}

/// Handles scripting add slider joint.
std::uint32_t scripting_add_slider_joint(runtime::World *world,
                                         std::uint32_t entityIndexA,
                                         std::uint32_t entityIndexB,
                                         float axisX, float axisY,
                                         float axisZ) noexcept {
  if ((world == nullptr) || (entityIndexA == 0U) || (entityIndexB == 0U)) {
    return 0U;
  }
  const runtime::Entity entityA = world->find_entity_by_index(entityIndexA);
  const runtime::Entity entityB = world->find_entity_by_index(entityIndexB);
  const math::Vec3 axis(axisX, axisY, axisZ);
  return static_cast<std::uint32_t>(
      runtime::add_slider_joint(*world, entityA, entityB, axis));
}

/// Handles scripting add spring joint.
std::uint32_t scripting_add_spring_joint(runtime::World *world,
                                         std::uint32_t entityIndexA,
                                         std::uint32_t entityIndexB,
                                         float restLength, float stiffness,
                                         float damping) noexcept {
  if ((world == nullptr) || (entityIndexA == 0U) || (entityIndexB == 0U)) {
    return 0U;
  }
  const runtime::Entity entityA = world->find_entity_by_index(entityIndexA);
  const runtime::Entity entityB = world->find_entity_by_index(entityIndexB);
  return static_cast<std::uint32_t>(runtime::add_spring_joint(
      *world, entityA, entityB, restLength, stiffness, damping));
}

/// Handles scripting add fixed joint.
std::uint32_t scripting_add_fixed_joint(runtime::World *world,
                                        std::uint32_t entityIndexA,
                                        std::uint32_t entityIndexB) noexcept {
  if ((world == nullptr) || (entityIndexA == 0U) || (entityIndexB == 0U)) {
    return 0U;
  }
  const runtime::Entity entityA = world->find_entity_by_index(entityIndexA);
  const runtime::Entity entityB = world->find_entity_by_index(entityIndexB);
  return static_cast<std::uint32_t>(
      runtime::add_fixed_joint(*world, entityA, entityB));
}

/// Handles scripting set joint limits.
void scripting_set_joint_limits(runtime::World *world, std::uint32_t jointId,
                                float minLimit, float maxLimit) noexcept {
  if (world == nullptr) {
    return;
  }
  runtime::set_joint_limits(*world, static_cast<physics::JointId>(jointId),
                            minLimit, maxLimit);
}

/// Handles scripting wake body.
void scripting_wake_body(runtime::World *world,
                         std::uint32_t entityIndex) noexcept {
  if ((world == nullptr) || (entityIndex == 0U)) {
    return;
  }

  runtime::wake_body(*world, world->find_entity_by_index(entityIndex));
}

/// Handles scripting is sleeping.
bool scripting_is_sleeping(runtime::World *world,
                           std::uint32_t entityIndex) noexcept {
  if ((world == nullptr) || (entityIndex == 0U)) {
    return false;
  }

  return runtime::is_sleeping(*world, world->find_entity_by_index(entityIndex));
}

/// Handles scripting load sound.
std::uint32_t scripting_load_sound(const char *path) noexcept {
  return audio::load_sound(path).id;
}

/// Handles scripting unload sound.
void scripting_unload_sound(std::uint32_t soundId) noexcept {
  audio::unload_sound(audio::SoundHandle{soundId});
}

/// Handles scripting play sound.
bool scripting_play_sound(std::uint32_t soundId, float volume, float pitch,
                          bool loop) noexcept {
  audio::PlayParams params{};
  params.volume = volume;
  params.pitch = pitch;
  params.loop = loop;
  return audio::play_sound(audio::SoundHandle{soundId}, params);
}

/// Handles scripting stop sound.
void scripting_stop_sound(std::uint32_t soundId) noexcept {
  audio::stop_sound(audio::SoundHandle{soundId});
}

/// Handles scripting stop all sounds.
void scripting_stop_all_sounds() noexcept { audio::stop_all(); }

/// Handles scripting set master volume.
void scripting_set_master_volume(float volume) noexcept {
  audio::set_master_volume(volume);
}

/// Handles scripting save scene.
bool scripting_save_scene(const runtime::World *world,
                          const char *path) noexcept {
  return (world != nullptr) && runtime::save_scene(*world, path);
}

/// Handles scripting save prefab.
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

/// Handles scripting instantiate prefab.
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

/// Handles scripting get entity index.
std::uint32_t scripting_get_entity_index(runtime::World *world,
                                         std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return 0U;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return (entity != runtime::kInvalidEntity) ? entity.index : 0U;
}

/// Handles scripting get transform count.
std::uint32_t scripting_get_transform_count(runtime::World *world) noexcept {
  if (world == nullptr) {
    return 0U;
  }

  const std::size_t count = world->transform_count();
  return static_cast<std::uint32_t>(count);
}

/// Handles scripting create entity op.
std::uint32_t scripting_create_entity_op(runtime::World *world) noexcept {
  if (world == nullptr) {
    return 0U;
  }
  const runtime::Entity entity = world->create_entity();
  return entity.index;
}

/// Handles scripting get transform read ptr.
const runtime::Transform *
scripting_get_transform_read_ptr(runtime::World *world,
                                 std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return nullptr;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_transform_read_ptr(entity);
}

/// Handles scripting get transform op.
bool scripting_get_transform_op(runtime::World *world,
                                std::uint32_t entityIndex,
                                runtime::Transform *outTransform) noexcept {
  if ((world == nullptr) || (outTransform == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_transform(entity, outTransform);
}

/// Handles scripting get rigid body op.
bool scripting_get_rigid_body_op(runtime::World *world,
                                 std::uint32_t entityIndex,
                                 runtime::RigidBody *outRigidBody) noexcept {
  if ((world == nullptr) || (outRigidBody == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_rigid_body(entity, outRigidBody);
}

/// Handles scripting get mesh component ptr.
const runtime::MeshComponent *
scripting_get_mesh_component_ptr(runtime::World *world,
                                 std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return nullptr;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_mesh_component_ptr(entity);
}

/// Handles scripting get mesh component op.
bool scripting_get_mesh_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    runtime::MeshComponent *outComponent) noexcept {
  if ((world == nullptr) || (outComponent == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_mesh_component(entity, outComponent);
}

/// Handles scripting get name component op.
bool scripting_get_name_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    runtime::NameComponent *outComponent) noexcept {
  if ((world == nullptr) || (outComponent == nullptr)) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->get_name_component(entity, outComponent);
}

/// Handles scripting get collider op.
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

/// Handles scripting add transform op.
bool scripting_add_transform_op(runtime::World *world,
                                std::uint32_t entityIndex,
                                const runtime::Transform &transform) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_transform(entity, transform);
}

/// Handles scripting set movement authority op.
bool scripting_set_movement_authority_op(
    runtime::World *world, std::uint32_t entityIndex,
    runtime::MovementAuthority authority) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->set_movement_authority(entity, authority);
}

/// Handles scripting add rigid body op.
bool scripting_add_rigid_body_op(runtime::World *world,
                                 std::uint32_t entityIndex,
                                 const runtime::RigidBody &rigidBody) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_rigid_body(entity, rigidBody);
}

/// Handles scripting add collider op.
bool scripting_add_collider_op(runtime::World *world, std::uint32_t entityIndex,
                               const runtime::Collider &collider) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_collider(entity, collider);
}

/// Handles scripting add mesh component op.
bool scripting_add_mesh_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::MeshComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_mesh_component(entity, component);
}

/// Handles scripting add name component op.
bool scripting_add_name_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::NameComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_name_component(entity, component);
}

/// Handles scripting add light component op.
bool scripting_add_light_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::LightComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_light_component(entity, component);
}

/// Handles scripting remove light component op.
bool scripting_remove_light_component_op(runtime::World *world,
                                         std::uint32_t entityIndex) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->remove_light_component(entity);
}

/// Handles scripting add script component op.
bool scripting_add_script_component_op(
    runtime::World *world, std::uint32_t entityIndex,
    const runtime::ScriptComponent &component) noexcept {
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity = world->find_entity_by_index(entityIndex);
  return world->add_script_component(entity, component);
}

/// Handles scripting remove script component op.
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
    &scripting_push_camera,
    &scripting_pop_camera,
    &scripting_get_active_camera,
    &scripting_camera_shake,
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
    &scripting_raycast_all,
    &scripting_overlap_sphere,
    &scripting_overlap_box,
    &scripting_sweep_sphere,
    &scripting_sweep_box,
    &scripting_add_distance_joint,
    &scripting_add_hinge_joint,
    &scripting_add_ball_socket_joint,
    &scripting_add_slider_joint,
    &scripting_add_spring_joint,
    &scripting_add_fixed_joint,
    &scripting_set_joint_limits,
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

/// Binds scripting runtime pointers into an explicit service locator.
void bind_scripting_runtime(World *world, core::ServiceLocator &locator) noexcept {
  scripting::bind_runtime_world(world, locator);
  scripting::bind_runtime_services(&kScriptingRuntimeServices, locator);
}

/// Clears scripting runtime bindings from an explicit service locator.
void unbind_scripting_runtime(core::ServiceLocator &locator) noexcept {
  scripting::bind_runtime_world(nullptr, locator);
  scripting::bind_runtime_services(nullptr, locator);
}

} // namespace runtime

} // namespace engine
