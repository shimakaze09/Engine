#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::runtime {

class World;
struct Transform;
struct RigidBody;
struct Collider;
struct MeshComponent;
struct NameComponent;
struct LightComponent;
struct ScriptComponent;
enum class MovementAuthority : std::uint8_t;
enum class WorldPhase : std::uint8_t;

} // namespace engine::runtime

namespace engine::scripting {

struct RuntimeRaycastHit final {
  std::uint32_t entityIndex = 0U;
  float distance = 0.0F;
  float pointX = 0.0F;
  float pointY = 0.0F;
  float pointZ = 0.0F;
  float normalX = 0.0F;
  float normalY = 0.0F;
  float normalZ = 0.0F;
};

struct RuntimeServices final {
  void (*set_camera_position)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_target)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_up)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_fov)(float fovRadians) noexcept = nullptr;

  // Camera manager operations (routed through bridge to avoid circular link)
  bool (*push_camera_op)(runtime::World *world, std::uint32_t entityIndex,
                         float posX, float posY, float posZ, float tgtX,
                         float tgtY, float tgtZ, float priority,
                         float blendSpeed) noexcept = nullptr;
  bool (*pop_camera_op)(runtime::World *world,
                        std::uint32_t entityIndex) noexcept = nullptr;
  bool (*get_active_camera_op)(runtime::World *world, float *outPosX,
                               float *outPosY, float *outPosZ, float *outTgtX,
                               float *outTgtY, float *outTgtZ,
                               float *outFov) noexcept = nullptr;
  bool (*camera_shake_op)(runtime::World *world, float amplitude,
                          float frequency, float duration,
                          float decay) noexcept = nullptr;

  // World query operations needed by Lua bindings
  runtime::WorldPhase (*get_current_phase)(runtime::World *world) noexcept =
      nullptr;
  std::uint32_t (*get_entity_index)(
      runtime::World *world, std::uint32_t entityIndex) noexcept = nullptr;
  std::uint32_t (*get_transform_count)(runtime::World *world) noexcept =
      nullptr;
  std::uint32_t (*create_entity_op)(runtime::World *world) noexcept = nullptr;
  const runtime::Transform *(*get_transform_read_ptr)(
      runtime::World *world, std::uint32_t entityIndex) noexcept = nullptr;
  bool (*get_transform_op)(runtime::World *world, std::uint32_t entityIndex,
                           runtime::Transform *outTransform) noexcept = nullptr;
  bool (*get_rigid_body_op)(runtime::World *world, std::uint32_t entityIndex,
                            runtime::RigidBody *outRigidBody) noexcept =
      nullptr;
  const runtime::MeshComponent *(*get_mesh_component_ptr)(
      runtime::World *world, std::uint32_t entityIndex) noexcept = nullptr;
  bool (*get_mesh_component_op)(
      runtime::World *world, std::uint32_t entityIndex,
      runtime::MeshComponent *outComponent) noexcept = nullptr;
  bool (*get_name_component_op)(
      runtime::World *world, std::uint32_t entityIndex,
      runtime::NameComponent *outComponent) noexcept = nullptr;
  bool (*get_collider_op)(runtime::World *world, std::uint32_t entityIndex,
                          runtime::Collider *outCollider) noexcept = nullptr;

  // World mutation operations (called from deferred mutation queue)
  bool (*destroy_entity_op)(runtime::World *world,
                            std::uint32_t entityIndex) noexcept = nullptr;
  bool (*add_transform_op)(runtime::World *world, std::uint32_t entityIndex,
                           const runtime::Transform &transform) noexcept =
      nullptr;
  bool (*set_movement_authority_op)(
      runtime::World *world, std::uint32_t entityIndex,
      runtime::MovementAuthority authority) noexcept = nullptr;
  bool (*add_rigid_body_op)(runtime::World *world, std::uint32_t entityIndex,
                            const runtime::RigidBody &rigidBody) noexcept =
      nullptr;
  bool (*add_collider_op)(runtime::World *world, std::uint32_t entityIndex,
                          const runtime::Collider &collider) noexcept = nullptr;
  bool (*add_mesh_component_op)(
      runtime::World *world, std::uint32_t entityIndex,
      const runtime::MeshComponent &component) noexcept = nullptr;
  bool (*add_name_component_op)(
      runtime::World *world, std::uint32_t entityIndex,
      const runtime::NameComponent &component) noexcept = nullptr;
  bool (*add_light_component_op)(
      runtime::World *world, std::uint32_t entityIndex,
      const runtime::LightComponent &component) noexcept = nullptr;
  bool (*remove_light_component_op)(
      runtime::World *world, std::uint32_t entityIndex) noexcept = nullptr;
  bool (*add_script_component_op)(
      runtime::World *world, std::uint32_t entityIndex,
      const runtime::ScriptComponent &component) noexcept = nullptr;
  bool (*remove_script_component_op)(
      runtime::World *world, std::uint32_t entityIndex) noexcept = nullptr;

  void (*set_gravity)(runtime::World *world, float x, float y,
                      float z) noexcept = nullptr;
  bool (*get_gravity)(runtime::World *world, float *outX, float *outY,
                      float *outZ) noexcept = nullptr;
  bool (*raycast)(runtime::World *world, float ox, float oy, float oz, float dx,
                  float dy, float dz, float maxDistance,
                  RuntimeRaycastHit *outHit) noexcept = nullptr;
  std::size_t (*raycast_all)(runtime::World *world, float ox, float oy,
                             float oz, float dx, float dy, float dz,
                             float maxDistance, RuntimeRaycastHit *outHits,
                             std::size_t maxHits,
                             std::uint32_t mask) noexcept = nullptr;
  std::size_t (*overlap_sphere)(runtime::World *world, float cx, float cy,
                                float cz, float radius,
                                std::uint32_t *outEntityIndices,
                                std::size_t maxResults,
                                std::uint32_t mask) noexcept = nullptr;
  std::size_t (*overlap_box)(runtime::World *world, float cx, float cy,
                             float cz, float hx, float hy, float hz,
                             std::uint32_t *outEntityIndices,
                             std::size_t maxResults,
                             std::uint32_t mask) noexcept = nullptr;
  bool (*sweep_sphere)(runtime::World *world, float ox, float oy, float oz,
                       float radius, float dx, float dy, float dz,
                       float maxDistance,
                       RuntimeRaycastHit *outHit,
                       std::uint32_t mask) noexcept = nullptr;
  bool (*sweep_box)(runtime::World *world, float cx, float cy, float cz,
                    float hx, float hy, float hz, float dx, float dy,
                    float dz, float maxDistance,
                    RuntimeRaycastHit *outHit,
                    std::uint32_t mask) noexcept = nullptr;
  std::uint32_t (*add_distance_joint)(runtime::World *world,
                                      std::uint32_t entityIndexA,
                                      std::uint32_t entityIndexB,
                                      float distance) noexcept = nullptr;
  std::uint32_t (*add_hinge_joint)(runtime::World *world,
                                   std::uint32_t entityIndexA,
                                   std::uint32_t entityIndexB, float pivotX,
                                   float pivotY, float pivotZ, float axisX,
                                   float axisY, float axisZ) noexcept = nullptr;
  std::uint32_t (*add_ball_socket_joint)(runtime::World *world,
                                         std::uint32_t entityIndexA,
                                         std::uint32_t entityIndexB,
                                         float pivotX, float pivotY,
                                         float pivotZ) noexcept = nullptr;
  std::uint32_t (*add_slider_joint)(runtime::World *world,
                                    std::uint32_t entityIndexA,
                                    std::uint32_t entityIndexB, float axisX,
                                    float axisY, float axisZ) noexcept = nullptr;
  std::uint32_t (*add_spring_joint)(runtime::World *world,
                                    std::uint32_t entityIndexA,
                                    std::uint32_t entityIndexB,
                                    float restLength, float stiffness,
                                    float damping) noexcept = nullptr;
  std::uint32_t (*add_fixed_joint)(runtime::World *world,
                                   std::uint32_t entityIndexA,
                                   std::uint32_t entityIndexB) noexcept = nullptr;
  void (*set_joint_limits)(runtime::World *world, std::uint32_t jointId,
                           float minLimit, float maxLimit) noexcept = nullptr;
  void (*remove_joint)(runtime::World *world,
                       std::uint32_t jointId) noexcept = nullptr;
  void (*wake_body)(runtime::World *world,
                    std::uint32_t entityIndex) noexcept = nullptr;
  bool (*is_sleeping)(runtime::World *world,
                      std::uint32_t entityIndex) noexcept = nullptr;

  std::uint32_t (*load_sound)(const char *path) noexcept = nullptr;
  void (*unload_sound)(std::uint32_t soundId) noexcept = nullptr;
  bool (*play_sound)(std::uint32_t soundId, float volume, float pitch,
                     bool loop) noexcept = nullptr;
  void (*stop_sound)(std::uint32_t soundId) noexcept = nullptr;
  void (*stop_all_sounds)() noexcept = nullptr;
  void (*set_master_volume)(float volume) noexcept = nullptr;

  bool (*save_scene)(const runtime::World *world,
                     const char *path) noexcept = nullptr;
  bool (*save_prefab)(const runtime::World *world, std::uint32_t entityIndex,
                      const char *path) noexcept = nullptr;
  std::uint32_t (*instantiate_prefab)(runtime::World *world,
                                      const char *path) noexcept = nullptr;
};

void bind_runtime_world(runtime::World *world) noexcept;
void bind_runtime_services(const RuntimeServices *services) noexcept;

} // namespace engine::scripting

namespace engine::runtime {

void bind_scripting_runtime(World *world) noexcept;

} // namespace engine::runtime