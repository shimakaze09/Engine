// Declares the contract between scripting and the runtime: the function
// table the runtime fills in for every operation Lua reaches the World and
// engine services through, and the binding calls that install it. Owned
// by scripting so the module describes its own needs without a runtime
// header; the runtime implements the table. Every operation that names an
// entity takes the full handle (index plus generation), so the World's
// own liveness check refuses a recycled handle instead of the bridge
// re-resolving the index to whichever entity holds it now.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/asset_identity.h"
#include "engine/core/entity.h"
#include "engine/math/component_types.h"
#include "engine/math/world_component_types.h"

namespace engine::runtime {

// The World is an opaque handle on this side of the boundary: every
// operation on it goes through the function table below.
class World;

using engine::core::Entity;
using engine::core::kInvalidEntity;
using engine::core::kInvalidPersistentId;
using engine::core::PersistentId;
using engine::math::CameraComponent;
using engine::math::CameraProjection;
using engine::math::Collider;
using engine::math::ColliderShape;
using engine::math::HullSource;
using engine::math::LightComponent;
using engine::math::LightType;
using engine::math::MeshComponent;
using engine::math::MovementAuthority;
using engine::math::NameComponent;
using engine::math::PointLightComponent;
using engine::math::RigidBody;
using engine::math::ScriptComponent;
using engine::math::SpotLightComponent;
using engine::math::SpringArmComponent;
using engine::math::Transform;

} // namespace engine::runtime

namespace engine::core {
class ServiceLocator;
} // namespace engine::core

namespace engine::scripting {

/// Entity capacity of the bound World; the runtime asserts it matches.
constexpr std::size_t kMaxWorldEntities = ENGINE_MAX_ENTITIES;
/// Timer slots the World's timer manager holds; the runtime asserts it.
constexpr std::size_t kMaxTimerSlots = 256U;
/// Entity pools a run may hold and the entities one pool may seed.
constexpr std::size_t kMaxEntityPools = 16U;
constexpr std::size_t kMaxEntityPoolSize = 1024U;

/// Game mode state machine position, mirrored from the runtime's GameMode.
enum class GameModeState : std::uint8_t {
  WaitingToStart = 0,
  InProgress,
  Paused,
  Ended,
};

/// Visitor for the entity iteration operations.
using EntityVisitFn = void (*)(core::Entity entity, void *context) noexcept;
/// Visitor over entities that carry a ScriptComponent.
using ScriptedEntityVisitFn = void (*)(core::Entity entity,
                                       const math::ScriptComponent &script,
                                       void *context) noexcept;
/// Callback a runtime timer fires with its own id.
using TimerCallbackFn = void (*)(std::uint32_t timerId,
                                 void *userData) noexcept;

/// Raycast hit mirrored into scripting-friendly fields.
struct RuntimeRaycastHit final {
  core::Entity entity = core::kInvalidEntity;
  float distance = 0.0F;
  float pointX = 0.0F;
  float pointY = 0.0F;
  float pointZ = 0.0F;
  float normalX = 0.0F;
  float normalY = 0.0F;
  float normalZ = 0.0F;
};

/// Function-pointer surface the scripting layer calls into the runtime.
/// Every pointer is null until the runtime binds the table; callers test
/// the ones they use.
struct RuntimeServices final {
  void (*set_camera_position)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_target)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_up)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_fov)(float fovRadians) noexcept = nullptr;

  // Camera manager operations.
  bool (*push_camera_op)(runtime::World *world, core::Entity entity,
                         float posX, float posY, float posZ, float tgtX,
                         float tgtY, float tgtZ, float priority,
                         float blendSpeed) noexcept = nullptr;
  bool (*pop_camera_op)(runtime::World *world,
                        core::Entity entity) noexcept = nullptr;
  bool (*get_active_camera_op)(runtime::World *world, float *outPosX,
                               float *outPosY, float *outPosZ, float *outTgtX,
                               float *outTgtY, float *outTgtZ,
                               float *outFov) noexcept = nullptr;
  bool (*camera_shake_op)(runtime::World *world, float amplitude,
                          float frequency, float duration,
                          float decay) noexcept = nullptr;

  // World identity and lookup. A handle is live only while its generation
  // matches; content_epoch advances whenever the World's whole contents
  // are replaced, so retained handles from an earlier scene are refused.
  bool (*is_input_phase)(runtime::World *world) noexcept = nullptr;
  bool (*is_alive)(runtime::World *world,
                   core::Entity entity) noexcept = nullptr;
  std::uint32_t (*content_epoch)(runtime::World *world) noexcept = nullptr;
  std::size_t (*alive_entity_count)(runtime::World *world) noexcept = nullptr;
  core::Entity (*find_entity_by_index)(runtime::World *world,
                                       std::uint32_t index) noexcept = nullptr;
  core::Entity (*find_entity_by_name)(runtime::World *world,
                                      const char *name) noexcept = nullptr;
  core::Entity (*find_entity_by_persistent_id)(
      runtime::World *world, core::PersistentId persistentId) noexcept =
      nullptr;
  core::PersistentId (*persistent_id)(runtime::World *world,
                                      core::Entity entity) noexcept = nullptr;
  /// Creates a scene object, at `transform` when one is given.
  core::Entity (*create_scene_object_op)(
      runtime::World *world,
      const runtime::Transform *transform) noexcept = nullptr;
  /// Clones every persistent component of `source` onto a fresh scene
  /// object, renaming the copy. Transactional: any component copy failure
  /// destroys the partial clone and returns kInvalidEntity.
  core::Entity (*clone_entity_op)(runtime::World *world,
                                  core::Entity source) noexcept = nullptr;

  // Iteration, in the World's own order.
  void (*for_each_alive)(runtime::World *world, EntityVisitFn visit,
                         void *context) noexcept = nullptr;
  void (*for_each_child)(runtime::World *world, core::Entity parent,
                         EntityVisitFn visit, void *context) noexcept = nullptr;
  void (*for_each_subtree_member)(runtime::World *world, core::Entity root,
                                  EntityVisitFn visit,
                                  void *context) noexcept = nullptr;
  void (*for_each_needs_begin_play)(runtime::World *world, EntityVisitFn visit,
                                    void *context) noexcept = nullptr;
  void (*for_each_pending_destroy)(runtime::World *world, EntityVisitFn visit,
                                   void *context) noexcept = nullptr;
  void (*for_each_scripted_entity)(runtime::World *world,
                                   ScriptedEntityVisitFn visit,
                                   void *context) noexcept = nullptr;
  bool (*has_begun_play)(runtime::World *world,
                         core::Entity entity) noexcept = nullptr;
  void (*mark_begin_play_done)(runtime::World *world,
                               core::Entity entity) noexcept = nullptr;

  // Component reads.
  const runtime::Transform *(*get_transform_read_ptr)(
      runtime::World *world, core::Entity entity) noexcept = nullptr;
  bool (*get_transform_op)(runtime::World *world, core::Entity entity,
                           runtime::Transform *outTransform) noexcept = nullptr;
  bool (*get_rigid_body_op)(runtime::World *world, core::Entity entity,
                            runtime::RigidBody *outRigidBody) noexcept =
      nullptr;
  const runtime::MeshComponent *(*get_mesh_component_ptr)(
      runtime::World *world, core::Entity entity) noexcept = nullptr;
  bool (*get_mesh_component_op)(
      runtime::World *world, core::Entity entity,
      runtime::MeshComponent *outComponent) noexcept = nullptr;
  bool (*get_name_component_op)(
      runtime::World *world, core::Entity entity,
      runtime::NameComponent *outComponent) noexcept = nullptr;
  bool (*get_collider_op)(runtime::World *world, core::Entity entity,
                          runtime::Collider *outCollider) noexcept = nullptr;
  bool (*get_light_component_op)(
      runtime::World *world, core::Entity entity,
      runtime::LightComponent *outComponent) noexcept = nullptr;
  bool (*has_light_component)(runtime::World *world,
                              core::Entity entity) noexcept = nullptr;
  bool (*get_point_light_component_op)(
      runtime::World *world, core::Entity entity,
      runtime::PointLightComponent *outComponent) noexcept = nullptr;
  bool (*get_spot_light_component_op)(
      runtime::World *world, core::Entity entity,
      runtime::SpotLightComponent *outComponent) noexcept = nullptr;
  bool (*get_script_component_op)(
      runtime::World *world, core::Entity entity,
      runtime::ScriptComponent *outComponent) noexcept = nullptr;
  bool (*get_spring_arm_op)(runtime::World *world, core::Entity entity,
                            math::SpringArmComponent *outComponent) noexcept =
      nullptr;
  bool (*get_camera_component_op)(
      runtime::World *world, core::Entity entity,
      math::CameraComponent *outComponent) noexcept = nullptr;
  /// True when the entity's collider carries a resident convex hull.
  bool (*has_convex_hull_payload)(runtime::World *world,
                                  core::Entity entity) noexcept = nullptr;

  // World mutation operations (also called from the deferred queue).
  bool (*destroy_entity_op)(runtime::World *world,
                            core::Entity entity) noexcept = nullptr;
  bool (*add_transform_op)(runtime::World *world, core::Entity entity,
                           const runtime::Transform &transform) noexcept =
      nullptr;
  bool (*set_movement_authority_op)(
      runtime::World *world, core::Entity entity,
      runtime::MovementAuthority authority) noexcept = nullptr;
  bool (*add_rigid_body_op)(runtime::World *world, core::Entity entity,
                            const runtime::RigidBody &rigidBody) noexcept =
      nullptr;
  bool (*add_collider_op)(runtime::World *world, core::Entity entity,
                          const runtime::Collider &collider) noexcept = nullptr;
  bool (*add_mesh_component_op)(
      runtime::World *world, core::Entity entity,
      const runtime::MeshComponent &component) noexcept = nullptr;
  bool (*add_name_component_op)(
      runtime::World *world, core::Entity entity,
      const runtime::NameComponent &component) noexcept = nullptr;
  bool (*add_light_component_op)(
      runtime::World *world, core::Entity entity,
      const runtime::LightComponent &component) noexcept = nullptr;
  bool (*remove_light_component_op)(runtime::World *world,
                                    core::Entity entity) noexcept = nullptr;
  bool (*add_point_light_component_op)(
      runtime::World *world, core::Entity entity,
      const runtime::PointLightComponent &component) noexcept = nullptr;
  bool (*remove_point_light_component_op)(
      runtime::World *world, core::Entity entity) noexcept = nullptr;
  bool (*add_spot_light_component_op)(
      runtime::World *world, core::Entity entity,
      const runtime::SpotLightComponent &component) noexcept = nullptr;
  bool (*remove_spot_light_component_op)(
      runtime::World *world, core::Entity entity) noexcept = nullptr;
  bool (*add_script_component_op)(
      runtime::World *world, core::Entity entity,
      const runtime::ScriptComponent &component) noexcept = nullptr;
  bool (*remove_script_component_op)(runtime::World *world,
                                     core::Entity entity) noexcept = nullptr;
  bool (*add_spring_arm_op)(
      runtime::World *world, core::Entity entity,
      const math::SpringArmComponent &component) noexcept = nullptr;
  bool (*add_camera_component_op)(
      runtime::World *world, core::Entity entity,
      const math::CameraComponent &component) noexcept = nullptr;
  bool (*remove_camera_component_op)(runtime::World *world,
                                     core::Entity entity) noexcept = nullptr;
  /// Applies the canonical convex hull `source` names to `collider`; false
  /// (collider untouched) when the source names no primitive hull.
  bool (*apply_primitive_hull)(math::HullSource source,
                               runtime::Collider *collider) noexcept = nullptr;

  // Game mode, owned by the World.
  const char *(*game_mode_name)(runtime::World *world) noexcept = nullptr;
  bool (*set_game_mode_name)(runtime::World *world,
                             const char *name) noexcept = nullptr;
  bool (*game_mode_start)(runtime::World *world) noexcept = nullptr;
  bool (*game_mode_pause)(runtime::World *world) noexcept = nullptr;
  bool (*game_mode_end)(runtime::World *world) noexcept = nullptr;
  GameModeState (*game_mode_state)(runtime::World *world) noexcept = nullptr;
  bool (*game_mode_set_rule)(runtime::World *world, const char *key,
                             const char *value) noexcept = nullptr;
  const char *(*game_mode_get_rule)(runtime::World *world,
                                    const char *key) noexcept = nullptr;
  std::uint32_t (*game_mode_max_players)(runtime::World *world) noexcept =
      nullptr;
  void (*set_game_mode_max_players)(runtime::World *world,
                                    std::uint32_t maxPlayers) noexcept =
      nullptr;

  // Timers, owned by the World. Ids are opaque and 0 is invalid; a slot is
  // the generation-matching storage index of an id, kMaxTimerSlots when
  // the id no longer names a live timer.
  std::uint32_t (*timer_set)(runtime::World *world, float seconds, bool repeat,
                             TimerCallbackFn callback,
                             void *userData) noexcept = nullptr;
  void (*timer_cancel)(runtime::World *world,
                       std::uint32_t timerId) noexcept = nullptr;
  std::size_t (*timer_slot_for_id)(runtime::World *world,
                                   std::uint32_t timerId) noexcept = nullptr;
  bool (*timer_slot_state)(runtime::World *world, std::size_t slot,
                           bool *outRepeat, bool *outActive) noexcept = nullptr;
  void (*timer_clear)(runtime::World *world) noexcept = nullptr;
  std::size_t (*timer_tick)(runtime::World *world,
                            float deltaSeconds) noexcept = nullptr;

  // Entity pools: kMaxEntityPools slots the runtime owns, each seeded
  // against the World's current contents and expiring with them.
  bool (*entity_pool_init)(runtime::World *world, std::size_t slot,
                           std::size_t count) noexcept = nullptr;
  core::Entity (*entity_pool_acquire)(runtime::World *world,
                                      std::size_t slot) noexcept = nullptr;
  bool (*entity_pool_release)(runtime::World *world, std::size_t slot,
                              core::Entity entity) noexcept = nullptr;
  void (*entity_pool_reset_all)() noexcept = nullptr;

  void (*set_gravity)(runtime::World *world, float x, float y,
                      float z) noexcept = nullptr;
  bool (*get_gravity)(runtime::World *world, float *outX, float *outY,
                      float *outZ) noexcept = nullptr;
  /// Ray queries take the same skipEntity as the sweeps (kInvalidEntity
  /// for none): that entity's colliders and the compound colliders it owns
  /// are excluded.
  bool (*raycast)(runtime::World *world, float ox, float oy, float oz, float dx,
                  float dy, float dz, float maxDistance,
                  RuntimeRaycastHit *outHit,
                  core::Entity skipEntity) noexcept = nullptr;
  std::size_t (*raycast_all)(runtime::World *world, float ox, float oy,
                             float oz, float dx, float dy, float dz,
                             float maxDistance, RuntimeRaycastHit *outHits,
                             std::size_t maxHits, std::uint32_t mask,
                             core::Entity skipEntity) noexcept = nullptr;
  std::size_t (*overlap_sphere)(runtime::World *world, float cx, float cy,
                                float cz, float radius,
                                core::Entity *outEntities,
                                std::size_t maxResults,
                                std::uint32_t mask) noexcept = nullptr;
  std::size_t (*overlap_box)(runtime::World *world, float cx, float cy,
                             float cz, float hx, float hy, float hz,
                             core::Entity *outEntities, std::size_t maxResults,
                             std::uint32_t mask) noexcept = nullptr;
  bool (*sweep_sphere)(runtime::World *world, float ox, float oy, float oz,
                       float radius, float dx, float dy, float dz,
                       float maxDistance, RuntimeRaycastHit *outHit,
                       std::uint32_t mask,
                       core::Entity skipEntity) noexcept = nullptr;
  bool (*sweep_box)(runtime::World *world, float cx, float cy, float cz,
                    float hx, float hy, float hz, float dx, float dy, float dz,
                    float maxDistance, RuntimeRaycastHit *outHit,
                    std::uint32_t mask,
                    core::Entity skipEntity) noexcept = nullptr;
  /// Joint constructors return the joint id or 0 for every failure —
  /// invalid entities, invalid parameters, self-joints, and a full joint
  /// table all share the one sentinel.
  std::uint32_t (*add_distance_joint)(runtime::World *world,
                                      core::Entity entityA,
                                      core::Entity entityB,
                                      float distance) noexcept = nullptr;
  std::uint32_t (*add_hinge_joint)(runtime::World *world, core::Entity entityA,
                                   core::Entity entityB, float pivotX,
                                   float pivotY, float pivotZ, float axisX,
                                   float axisY, float axisZ) noexcept = nullptr;
  std::uint32_t (*add_ball_socket_joint)(runtime::World *world,
                                         core::Entity entityA,
                                         core::Entity entityB, float pivotX,
                                         float pivotY,
                                         float pivotZ) noexcept = nullptr;
  std::uint32_t (*add_slider_joint)(runtime::World *world,
                                    core::Entity entityA, core::Entity entityB,
                                    float axisX, float axisY,
                                    float axisZ) noexcept = nullptr;
  std::uint32_t (*add_spring_joint)(runtime::World *world,
                                    core::Entity entityA, core::Entity entityB,
                                    float restLength, float stiffness,
                                    float damping) noexcept = nullptr;
  std::uint32_t (*add_fixed_joint)(runtime::World *world, core::Entity entityA,
                                   core::Entity entityB) noexcept = nullptr;
  // false on a stale/invalid joint id, wrong joint type,
  // out-of-range limits, or outside the Input phase, so a script can tell a
  // dropped write from an applied one.
  bool (*set_joint_limits)(runtime::World *world, std::uint32_t jointId,
                           float minLimit, float maxLimit) noexcept = nullptr;
  bool (*remove_joint)(runtime::World *world,
                       std::uint32_t jointId) noexcept = nullptr;
  void (*wake_body)(runtime::World *world,
                    core::Entity entity) noexcept = nullptr;
  bool (*is_sleeping)(runtime::World *world,
                      core::Entity entity) noexcept = nullptr;

  std::uint32_t (*load_sound)(const char *path) noexcept = nullptr;
  void (*unload_sound)(std::uint32_t soundId) noexcept = nullptr;
  bool (*play_sound)(std::uint32_t soundId, float volume, float pitch,
                     bool loop) noexcept = nullptr;
  void (*stop_sound)(std::uint32_t soundId) noexcept = nullptr;
  void (*stop_all_sounds)() noexcept = nullptr;
  void (*set_master_volume)(float volume) noexcept = nullptr;
  bool (*play_sound_at)(std::uint32_t soundId, float x, float y, float z,
                        float volume) noexcept = nullptr;
  void (*set_bus_volume)(std::uint32_t bus, float volume) noexcept = nullptr;
  bool (*play_music)(const char *path, float volume,
                     bool loop) noexcept = nullptr;
  void (*stop_music)() noexcept = nullptr;
  bool (*save_game_data)(const char *json,
                         std::size_t length) noexcept = nullptr;
  bool (*load_game_data)(char *out, std::size_t capacity,
                         std::size_t *outLength) noexcept = nullptr;

  bool (*save_scene)(const runtime::World *world,
                     const char *path) noexcept = nullptr;
  bool (*save_prefab)(const runtime::World *world, core::Entity entity,
                      const char *path) noexcept = nullptr;
  core::Entity (*instantiate_prefab)(runtime::World *world,
                                     const char *path) noexcept = nullptr;

  // Async asset streaming. Returns an opaque handle index (0xFFFFFFFF =
  // invalid). priority: 0=Low, 1=Normal, 2=High, 3=Immediate.
  std::uint32_t (*load_asset_async)(const char *path,
                                    std::uint8_t priority) noexcept = nullptr;
  bool (*is_asset_ready)(std::uint32_t handleIndex) noexcept = nullptr;

  // The persistent identity the catalog holds for an asset id, or a nil
  // reference when the asset carries none. A script that points a
  // component at an asset stores this beside the id: the id says where
  // the bytes are this session, the reference is what a saved scene
  // names, so a component assigned from Lua survives a save and reload.
  core::AssetRef (*asset_ref_for_id)(std::uint64_t assetId) noexcept =
      nullptr;
};

/// Binds the runtime world into an explicit service locator. Binding
/// replaces any current World entry (last writer wins); passing nullptr
/// unbinds, removing the entry only while it still holds the world this
/// bridge registered so a newer provider is never clobbered.
void bind_runtime_world(runtime::World *world,
                        core::ServiceLocator &locator) noexcept;
/// Binds runtime services into an explicit service locator; same
/// last-writer-wins bind and ownership-checked unbind as bind_runtime_world.
void bind_runtime_services(const RuntimeServices *services,
                           core::ServiceLocator &locator) noexcept;

} // namespace engine::scripting
