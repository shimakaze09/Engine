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
using engine::math::CharacterControllerComponent;
using engine::math::Collider;
using engine::math::ColliderShape;
using engine::math::HullSource;
using engine::math::LightComponent;
using engine::math::LightType;
using engine::math::MeshComponent;
using engine::math::NameComponent;
using engine::math::PointLightComponent;
using engine::math::RigidBody;
using engine::math::ScriptComponent;
using engine::math::SpotLightComponent;
using engine::math::SpringArmComponent;
using engine::math::TagSetComponent;
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
/// Entity pools a scene may hold and the entities one pool may seed. A
/// game pools each kind it spawns often (bullets, coins, enemies,
/// effects), so 16 kinds were met; a pool is about 13 KB of fixed storage.
constexpr std::size_t kMaxEntityPools = 64U;
constexpr std::size_t kMaxEntityPoolSize = 1024U;
/// A save slot name's capacity, terminator included, and how many slots a
/// listing returns; the runtime asserts both match its own.
constexpr std::size_t kGameSaveSlotNameCapacity = 32U;
constexpr std::size_t kMaxGameSaveSlots = 256U;
/// What reading a save slot found; the runtime's SaveReadResult, asserted
/// value for value.
enum class GameSaveRead : std::uint8_t {
  Ok,
  Absent,
  Unreadable,
  Corrupt,
  Unsupported,
};
/// One save slot as a listing sees it; the runtime's SaveSlotInfo.
struct GameSaveSlotInfo final {
  char slot[kGameSaveSlotNameCapacity] = {};
  GameSaveRead status = GameSaveRead::Ok;
  std::int64_t savedAt = 0;
  std::uint64_t payloadBytes = 0U;
  bool legacy = false;
};

/// Visitor for the entity iteration operations.
using EntityVisitFn = void (*)(core::Entity entity, void *context) noexcept;
/// Visitor over entities that carry a ScriptComponent, in ascending entity
/// index: the one order every script hook dispatches in.
using ScriptedEntityVisitFn = void (*)(core::Entity entity,
                                       const math::ScriptComponent &script,
                                       void *context) noexcept;
/// Callback a runtime timer fires with its own id.
using TimerCallbackFn = void (*)(std::uint32_t timerId,
                                 void *userData) noexcept;

/// What a character's move found, as scripting sees it.
struct RuntimeCharacterMove final {
  bool grounded = false;
  /// physics::kCharacterCollided* bits: 1 below, 2 sides, 4 above.
  std::uint32_t flags = 0U;
  core::Entity ground{};
  float normalX = 0.0F;
  float normalY = 0.0F;
  float normalZ = 0.0F;
};

/// What a path query found, mirroring navigation::NavPathResult.
enum class RuntimePathResult : std::uint8_t {
  Found,
  /// The start or the end is not near a loaded mesh.
  OffMesh,
  /// Both are on the mesh but no walkable route joins them.
  Unreachable,
  /// The path has more corners than the output holds.
  TooLong,
};

/// One corner of a path.
struct RuntimePathPoint final {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

/// Where a navigation agent is in reaching its destination, mirroring
/// runtime::NavAgentStatus.
enum class RuntimeNavAgentStatus : std::uint8_t {
  Idle,
  Pending,
  Moving,
  Arrived,
  Failed,
};

/// Why an agent's destination failed, mirroring runtime::NavAgentFailure.
enum class RuntimeNavAgentFailure : std::uint8_t {
  None,
  OffMesh,
  Unreachable,
  TooLong,
  CannotMove,
};

/// A navigation agent as scripting reads it.
struct RuntimeNavAgentState final {
  RuntimeNavAgentStatus status = RuntimeNavAgentStatus::Idle;
  RuntimeNavAgentFailure failure = RuntimeNavAgentFailure::None;
  float remainingDistance = 0.0F;
  float speed = 0.0F;
};

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
  // The camera that renders, as the camera manager last evaluated it.
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
  // The gameplay random stream, drawn through the World so a script's
  // randomness is part of the simulation state a run is reproducible
  // from. Three operations rather than exposing the stream itself: the
  // scripting side holds no simulation state of its own, so there is one
  // place a reset has to reach rather than two to keep in step.
  double (*random_double)(runtime::World *world) noexcept = nullptr;
  std::int64_t (*random_range)(runtime::World *world, std::int64_t minimum,
                               std::int64_t maximum) noexcept = nullptr;
  void (*seed_random)(runtime::World *world,
                      std::uint64_t seed) noexcept = nullptr;
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
  bool (*get_transform_op)(runtime::World *world, core::Entity entity,
                           runtime::Transform *outTransform) noexcept = nullptr;
  bool (*get_rigid_body_op)(runtime::World *world, core::Entity entity,
                            runtime::RigidBody *outRigidBody) noexcept =
      nullptr;
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
  bool (*get_tag_set_component_op)(
      runtime::World *world, core::Entity entity,
      math::TagSetComponent *outComponent) noexcept = nullptr;
  bool (*get_character_controller_op)(
      runtime::World *world, core::Entity entity,
      math::CharacterControllerComponent *outComponent) noexcept = nullptr;
  /// World::find_entities_by_tag: up to `capacity` entities carrying `tag`
  /// in ascending entity index; returns how many carry it.
  std::size_t (*find_entities_by_tag)(runtime::World *world, const char *tag,
                                      core::Entity *out,
                                      std::size_t capacity) noexcept = nullptr;
  /// True when the entity's collider carries a resident convex hull.
  bool (*has_convex_hull_payload)(runtime::World *world,
                                  core::Entity entity) noexcept = nullptr;

  // World mutation operations (also called from the deferred queue).
  bool (*destroy_entity_op)(runtime::World *world,
                            core::Entity entity) noexcept = nullptr;
  bool (*add_transform_op)(runtime::World *world, core::Entity entity,
                           const runtime::Transform &transform) noexcept =
      nullptr;
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
  bool (*add_tag_set_component_op)(
      runtime::World *world, core::Entity entity,
      const math::TagSetComponent &component) noexcept = nullptr;
  bool (*remove_tag_set_component_op)(runtime::World *world,
                                      core::Entity entity) noexcept = nullptr;
  bool (*add_character_controller_op)(
      runtime::World *world, core::Entity entity,
      const math::CharacterControllerComponent &component) noexcept = nullptr;
  bool (*remove_character_controller_op)(
      runtime::World *world, core::Entity entity) noexcept = nullptr;
  /// The scene's navigation path from start to end on the mesh whose bake
  /// volume holds the start: on Found, `out` holds up to `capacity`
  /// corners and *outCount their number. Allocates nothing.
  RuntimePathResult (*find_path_op)(runtime::World *world, float sx, float sy,
                                    float sz, float ex, float ey, float ez,
                                    RuntimePathPoint *out, std::size_t capacity,
                                    std::size_t *outCount) noexcept = nullptr;
  /// Sends the entity's navigation agent to (x, y, z); its path is found
  /// on its next fixed step. False, with a logged reason, when it has no
  /// Nav Agent, is parented, or the point is not finite.
  bool (*set_nav_destination_op)(runtime::World *world, core::Entity entity,
                                 float x, float y, float z) noexcept = nullptr;
  /// Stops the entity's navigation agent where it stands; false when it
  /// has none.
  bool (*stop_nav_agent_op)(runtime::World *world,
                            core::Entity entity) noexcept = nullptr;
  /// Reads the entity's navigation agent; false when it has none.
  bool (*nav_agent_state_op)(runtime::World *world, core::Entity entity,
                             RuntimeNavAgentState *out) noexcept = nullptr;
  /// runtime::move_character: moves the character at once; false, with a
  /// logged reason and nothing changed, when it cannot.
  bool (*move_character_op)(runtime::World *world, core::Entity entity,
                            float dx, float dy, float dz,
                            RuntimeCharacterMove *out) noexcept = nullptr;
  /// The collider every spawn path gives a built-in primitive, hull
  /// provenance and offset included (runtime::primitive_collider).
  runtime::Collider (*primitive_collider)(math::PrimitiveShape shape) noexcept =
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
  // Coming due and firing are separate now. The pipeline advances the
  // World's timers once per fixed step and dispatches once per frame, so
  // when a timer comes due is simulation time rather than frame rate;
  // this advance entry exists for callers outside the fixed step that
  // step a world by hand.
  std::size_t (*timer_advance)(runtime::World *world,
                               float deltaSeconds) noexcept = nullptr;
  // Runs the callbacks of the timers an advance marked as due. No delta:
  // coming due was already decided, and this only dispatches it.
  std::size_t (*timer_dispatch)(runtime::World *world) noexcept = nullptr;

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
  /// The bit of the project's collision layer named `name`, matched
  /// ignoring case; -1 when no layer has that name.
  int (*collision_layer_bit)(const char *name) noexcept = nullptr;
  /// Copies the name of collision layer `bit` into `out` whole; false when
  /// the layer is unnamed, the bit is past 31 or the name does not fit.
  bool (*collision_layer_name)(std::uint32_t bit, char *out,
                               std::size_t capacity) noexcept = nullptr;
  /// Ray queries take the same skipEntity as the sweeps (kInvalidEntity
  /// for none): that entity's colliders and the compound colliders it owns
  /// are excluded.
  bool (*raycast)(runtime::World *world, float ox, float oy, float oz, float dx,
                  float dy, float dz, float maxDistance,
                  RuntimeRaycastHit *outHit, core::Entity skipEntity,
                  std::uint32_t mask) noexcept = nullptr;
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
  /// Capsule between hemisphere centers a and b of the given radius.
  bool (*sweep_capsule)(runtime::World *world, float ax, float ay, float az,
                        float bx, float by, float bz, float radius, float dx,
                        float dy, float dz, float maxDistance,
                        RuntimeRaycastHit *outHit, std::uint32_t mask,
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
  /// Writes `payload` as the save slot `slot` (a valid slot name).
  bool (*save_game_data)(const char *slot, const char *payload,
                         std::size_t length) noexcept = nullptr;
  /// Reads the save slot `slot`; on Ok, *outPayload points at its
  /// NUL-terminated payload, valid until the next load_game_data.
  GameSaveRead (*load_game_data)(const char *slot, const char **outPayload,
                                 std::size_t *outLength) noexcept = nullptr;
  /// Holds a slot after a load could not read it or found its document
  /// corrupt or from a newer build: save_game_data refuses that slot until
  /// discard_game_save.
  void (*hold_game_save)(const char *slot) noexcept = nullptr;
  /// Moves a slot aside (never deleting it) and lifts its hold.
  bool (*discard_game_save)(const char *slot) noexcept = nullptr;
  /// Lists the save slots sorted by name into `out`; returns how many
  /// there are, which may exceed `capacity`.
  std::size_t (*list_game_saves)(GameSaveSlotInfo *out,
                                 std::size_t capacity) noexcept = nullptr;
  /// The largest payload save_game_data writes, the project's setting.
  std::size_t (*game_save_limit)() noexcept = nullptr;

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

  // Records on the asset catalog that the script at `path` reloaded and
  // committed (content::note_asset_reloaded); nothing when the catalog
  // does not hold it. Called only after a reload commits.
  void (*note_script_reloaded)(const char *path) noexcept = nullptr;
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
