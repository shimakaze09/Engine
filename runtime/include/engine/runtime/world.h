// Declares world types and APIs for the Engine runtime world.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>

#include "engine/core/entity.h"
#include "engine/core/fixed_hash_table.h"
#include "engine/core/sparse_set.h"
#include "engine/math/component_types.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics_world_view.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/game_mode.h"
#include "engine/runtime/timer_manager.h"

namespace engine::runtime {

#ifndef ENGINE_MAX_ENTITIES
#define ENGINE_MAX_ENTITIES 65536U
#endif

#ifndef ENGINE_MAX_LIGHT_COMPONENTS
#define ENGINE_MAX_LIGHT_COMPONENTS 1024U
#endif

// Types owned by core/math modules, re-exported into engine::runtime.
using engine::core::Entity;
using engine::core::kInvalidEntity;
using engine::core::kInvalidPersistentId;
using engine::core::PersistentId;
using engine::math::Collider;
using engine::math::ColliderShape;
using engine::math::MovementAuthority;
using engine::math::RigidBody;
using engine::math::Transform;

/// Propagated world-space transform plus its cached composite matrix.
struct WorldTransform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Mat4 matrix = math::Mat4();
};

/// Fixed-capacity display name (31 chars + terminator).
struct NameComponent final {
  static constexpr std::size_t kMaxNameLength = 31U; // +1 for null terminator
  char name[kMaxNameLength + 1U] = {};
};

/// Enumerates light type values used by the engine.
enum class LightType : std::uint8_t { Directional = 0, Point = 1 };

/// Directional or point light: color, direction, and intensity.
struct LightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 direction = math::Vec3(0.4F, -1.0F, 0.6F);
  float intensity = 1.0F;
  LightType type = LightType::Directional;
};

/// Point light with color, intensity, and attenuation radius.
struct PointLightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  float intensity = 1.0F;
  float radius = 10.0F;
};

/// Spot light: color, direction, cone angles (radians), and radius.
struct SpotLightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 direction = math::Vec3(0.0F, -1.0F, 0.0F);
  float intensity = 1.0F;
  float radius = 10.0F;
  float innerConeAngle = 0.3491F; // ~20 degrees in radians
  float outerConeAngle = 0.5236F; // ~30 degrees in radians
};

/// IBL reflection probe: bake resolutions, influence shape, bake flag.
struct ReflectionProbeComponent final {
  math::Vec3 boxExtents = math::Vec3(5.0F, 5.0F, 5.0F);
  float radius = 10.0F;
  float intensity = 1.0F;
  std::uint32_t prefilteredResolution = 128U;
  std::uint32_t irradianceResolution = 32U;
  std::uint32_t brdfLutResolution = 512U;
  std::uint32_t mipLevels = 5U;
  bool boxProjection = false;
  bool needsBake = true;
};

// Attaches a Lua script file to an entity.
// The script must return a module table with optional on_start(self) and
// on_update(self, dt) functions. Multiple entities may share the same file.
struct ScriptComponent final {
  static constexpr std::size_t kMaxPathLength = 127U; // +1 for null terminator
  char scriptPath[kMaxPathLength + 1U] = {};
};

// Renderer-facing component; keep minimal to avoid bloating draw commands.
// When materialAssetId is set (non-zero) render prep uses the resolved
// material asset's parameters; the inline fields below are the fallback.
struct MeshComponent final {
  std::uint64_t meshAssetId = 0ULL;
  std::uint64_t materialAssetId = 0ULL;
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
};

/// One foliage instance: offset from the patch origin, scale, wind
/// phase, and LOD.
struct FoliageInstance final {
  math::Vec3 offset = math::Vec3(0.0F, 0.0F, 0.0F);
  float scale = 1.0F;
  float phase = 0.0F;
  std::uint32_t lodIndex = 0U;
};

/// Instanced foliage patch: per-LOD meshes, material, wind, instances.
struct FoliagePatchComponent final {
  static constexpr std::size_t kMaxInstances = 64U;
  static constexpr std::size_t kMaxLods = 3U;

  std::uint64_t meshAssetIds[kMaxLods] = {};
  std::uint32_t instanceCount = 0U;
  float density = 1.0F;
  math::Vec3 albedo = math::Vec3(0.25F, 0.65F, 0.25F);
  float roughness = 0.85F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  float windStrength = 0.14F;
  float windFrequency = 1.6F;
  FoliageInstance instances[kMaxInstances] = {};
};

/// Spring arm component: drives a third-person camera boom that shortens on
/// collision and smoothly interpolates length.
struct SpringArmComponent final {
  float armLength = 5.0F;     ///< Desired arm length (world units).
  float currentLength = 5.0F; ///< Interpolated length after collision.
  math::Vec3 offset = math::Vec3(0.0F, 1.0F, 0.0F); ///< Pivot offset.
  float lagSpeed = 8.0F;         ///< Smoothing interpolation rate.
  float collisionRadius = 0.25F; ///< Sphere sweep radius.
  bool collisionEnabled = true;
};

using TransformVisitor = void (*)(Entity entity, const Transform &transform,
                                  void *userData) noexcept;

/// Frame phases the world moves through; mutation is gated on Input.
enum class WorldPhase : std::uint8_t {
  Input,
  BeginPlay,
  Simulation,
  TransformPropagation,
  RenderSubmission,
  Render,
  EndPlay,
};

/// Fixed-capacity ECS world: entity lifetimes, component storage, phase
/// gating, and the physics-facing world view.
class World final : public physics::PhysicsWorldView {
public:
  static constexpr std::size_t kMaxEntities = ENGINE_MAX_ENTITIES;
  static constexpr std::size_t kMaxTransforms = kMaxEntities;
  static constexpr std::size_t kMaxRigidBodies = kMaxEntities;
  static constexpr std::size_t kMaxColliders = kMaxEntities;
  static constexpr std::size_t kMaxMeshComponents = kMaxEntities;
  static constexpr std::size_t kMaxNameComponents = kMaxEntities;
  static constexpr std::size_t kMaxLightComponents =
      ENGINE_MAX_LIGHT_COMPONENTS;
  static constexpr std::size_t kMaxScriptComponents = kMaxEntities;
  static constexpr std::size_t kMaxSpringArmComponents = 64U;
  static constexpr std::size_t kMaxPointLightComponents = 128U;
  static constexpr std::size_t kMaxSpotLightComponents = 64U;
  static constexpr std::size_t kMaxReflectionProbeComponents = 64U;
  static constexpr std::size_t kMaxFoliagePatchComponents = 128U;
  static constexpr std::size_t kNameLookupCapacity = kMaxNameComponents * 2U;
  static constexpr std::size_t kStateBufferCount = 2U;
  static constexpr std::size_t kPersistentIndexCapacity = kMaxEntities * 2U;
  World() noexcept;

  /// Creates a new object, handle, or resource for entity.
  Entity create_entity() noexcept;
  /// Creates a new object, handle, or resource for entity with persistent id.
  Entity create_entity_with_persistent_id(PersistentId persistentId) noexcept;
  /// Destroys or releases the requested object, handle, or resource for entity.
  bool destroy_entity(Entity entity) noexcept;
  /// Returns whether is alive.
  bool is_alive(Entity entity) const noexcept;
  /// Finds the matching object or resource for entity by index.
  Entity find_entity_by_index(std::uint32_t index) const noexcept;
  /// Finds the matching object or resource for entity by persistent id.
  Entity find_entity_by_persistent_id(PersistentId persistentId) const noexcept;
  /// Serialization-stable id of a live entity; kInvalidPersistentId when dead.
  PersistentId persistent_id(Entity entity) const noexcept;
  /// Number of live alive entity components.
  std::size_t alive_entity_count() const noexcept;

  /// Invokes fn(Entity) for every alive entity in index order.
  template <typename Fn> void for_each_alive(Fn &&fn) const noexcept {
    if (m_aliveEntityCount == 0U) {
      return;
    }

    std::size_t visited = 0U;
    const std::uint32_t upperBound = m_nextEntityIndex;
    for (std::uint32_t index = 1U;
         (index < upperBound) && (visited < m_aliveEntityCount); ++index) {
      if (!m_entityAlive[index]) {
        continue;
      }

      fn(Entity{index, m_entityGenerations[index]});
      ++visited;
    }
  }

  /// Adds or replaces the entity's transform. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_transform(Entity entity, const Transform &transform) noexcept;
  /// Removes the entity's transform. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_transform(Entity entity) noexcept;
  /// Copies the entity's transform into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_transform(Entity entity,
                     Transform *outTransform) const noexcept override;
  // Read buffer is last committed state; during Idle this is the previous
  // frame.
  const Transform *get_transform_read_ptr(Entity entity) const noexcept;
  /// Token proving the Simulation phase is active; gates transform writes.
  SimulationAccessToken simulation_access_token() const noexcept override;
  /// Pointer to the entity's transform write, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  Transform *
  get_transform_write_ptr(Entity entity,
                          const SimulationAccessToken &token) noexcept override;
  /// Pointer to the entity's world transform read, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const WorldTransform *
  get_world_transform_read_ptr(Entity entity) const noexcept;
  /// Sets the requested value for movement authority.
  bool set_movement_authority(Entity entity,
                              MovementAuthority authority) noexcept;
  /// Who currently drives this entity's transform (physics vs script).
  MovementAuthority movement_authority(Entity entity) const noexcept override;

  /// Adds or replaces the entity's rigid body. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_rigid_body(Entity entity, const RigidBody &rigidBody) noexcept;
  /// Removes the entity's rigid body. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_rigid_body(Entity entity) noexcept;
  /// Copies the entity's rigid body into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_rigid_body(Entity entity,
                      RigidBody *outRigidBody) const noexcept override;

  /// Adds or replaces the entity's collider. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_collider(Entity entity, const Collider &collider) noexcept;
  /// Removes the entity's collider. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_collider(Entity entity) noexcept;
  /// Copies the entity's collider into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_collider(Entity entity, Collider *outCollider) const noexcept;
  /// Dense collider span [startIndex, startIndex+count); false out of range.
  bool
  get_collider_range(std::size_t startIndex, std::size_t count,
                     const Entity **outEntities,
                     const Collider **outColliders) const noexcept override;

  /// Adds or replaces the entity's mesh component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_mesh_component(Entity entity,
                          const MeshComponent &component) noexcept;
  /// Removes the entity's mesh component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_mesh_component(Entity entity) noexcept;
  /// Copies the entity's mesh component into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_mesh_component(Entity entity,
                          MeshComponent *outComponent) const noexcept;
  /// Pointer to the entity's mesh component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  MeshComponent *get_mesh_component_ptr(Entity entity) noexcept;
  /// Pointer to the entity's mesh component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const MeshComponent *get_mesh_component_ptr(Entity entity) const noexcept;

  /// Adds or replaces the entity's foliage patch. Requires the Input phase
  /// and a live entity; logs and returns false otherwise or when full.
  bool add_foliage_patch_component(
      Entity entity, const FoliagePatchComponent &component) noexcept;
  /// Removes the entity's foliage patch component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_foliage_patch_component(Entity entity) noexcept;
  /// Copies the entity's foliage patch into the out parameter; logs and
  /// returns false for stale/dead entities or when absent.
  bool get_foliage_patch_component(
      Entity entity, FoliagePatchComponent *outComponent) const noexcept;
  /// Returns whether has foliage patch component.
  bool has_foliage_patch_component(Entity entity) const noexcept;
  /// Number of live foliage patch components.
  std::size_t foliage_patch_count() const noexcept;
  /// Dense-storage foliage patch at `index` (0..count-1); nullptr out of range.
  const FoliagePatchComponent *
  foliage_patch_at(std::size_t index) const noexcept;
  /// Entity owning the dense foliage patch slot at `index`; kInvalidEntity
  /// when out of range.
  Entity foliage_patch_entity_at(std::size_t index) const noexcept;
  /// Pointer to the entity's foliage patch component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  FoliagePatchComponent *get_foliage_patch_component_ptr(
      Entity entity) noexcept;
  /// Pointer to the entity's foliage patch component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const FoliagePatchComponent *get_foliage_patch_component_ptr(
      Entity entity) const noexcept;

  /// Adds or replaces the entity's name component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_name_component(Entity entity,
                          const NameComponent &component) noexcept;
  /// Removes the entity's name component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_name_component(Entity entity) noexcept;
  /// Copies the entity's name component into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_name_component(Entity entity,
                          NameComponent *outComponent) const noexcept;
  /// Pointer to the entity's name component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  NameComponent *get_name_component_ptr(Entity entity) noexcept;
  /// Pointer to the entity's name component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const NameComponent *get_name_component_ptr(Entity entity) const noexcept;
  /// Finds the matching object or resource for entity by name.
  Entity find_entity_by_name(const char *name) const noexcept;

  /// Adds or replaces the entity's script component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_script_component(Entity entity,
                            const ScriptComponent &component) noexcept;
  /// Removes the entity's script component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_script_component(Entity entity) noexcept;
  /// Copies the entity's script component into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_script_component(Entity entity,
                            ScriptComponent *outComponent) const noexcept;
  /// Pointer to the entity's script component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  ScriptComponent *get_script_component_ptr(Entity entity) noexcept;
  /// Pointer to the entity's script component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const ScriptComponent *get_script_component_ptr(Entity entity) const noexcept;

  /// Adds or replaces the entity's light component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_light_component(Entity entity,
                           const LightComponent &component) noexcept;
  /// Removes the entity's light component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_light_component(Entity entity) noexcept;
  /// Copies the entity's light component into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_light_component(Entity entity,
                           LightComponent *outComponent) const noexcept;
  /// Returns whether has light component.
  bool has_light_component(Entity entity) const noexcept;
  /// Number of live light components.
  std::size_t light_count() const noexcept;
  /// Dense-storage light at `index` (0..count-1); nullptr out of range.
  const LightComponent *light_at(std::size_t index) const noexcept;
  /// Entity owning the dense light slot at `index`; kInvalidEntity
  /// when out of range.
  Entity light_entity_at(std::size_t index) const noexcept;

  /// Adds or replaces the entity's point light component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_point_light_component(Entity entity,
                                 const PointLightComponent &component) noexcept;
  /// Removes the entity's point light component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_point_light_component(Entity entity) noexcept;
  /// Copies the entity's point light component into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool
  get_point_light_component(Entity entity,
                            PointLightComponent *outComponent) const noexcept;
  /// Returns whether has point light component.
  bool has_point_light_component(Entity entity) const noexcept;
  /// Number of live point light components.
  std::size_t point_light_count() const noexcept;
  /// Dense-storage point light at `index` (0..count-1); nullptr out of range.
  const PointLightComponent *point_light_at(std::size_t index) const noexcept;
  /// Entity owning the dense point light slot at `index`; kInvalidEntity
  /// when out of range.
  Entity point_light_entity_at(std::size_t index) const noexcept;

  /// Adds or replaces the entity's spot light component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_spot_light_component(Entity entity,
                                const SpotLightComponent &component) noexcept;
  /// Removes the entity's spot light component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_spot_light_component(Entity entity) noexcept;
  /// Copies the entity's spot light component into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool
  get_spot_light_component(Entity entity,
                           SpotLightComponent *outComponent) const noexcept;
  /// Returns whether has spot light component.
  bool has_spot_light_component(Entity entity) const noexcept;
  /// Number of live spot light components.
  std::size_t spot_light_count() const noexcept;
  /// Dense-storage spot light at `index` (0..count-1); nullptr out of range.
  const SpotLightComponent *spot_light_at(std::size_t index) const noexcept;
  /// Entity owning the dense spot light slot at `index`; kInvalidEntity
  /// when out of range.
  Entity spot_light_entity_at(std::size_t index) const noexcept;

  /// Adds or replaces the entity's reflection probe. Requires the Input
  /// phase and a live entity; logs and returns false otherwise or when full.
  bool add_reflection_probe_component(
      Entity entity, const ReflectionProbeComponent &component) noexcept;
  /// Removes the entity's reflection probe component. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_reflection_probe_component(Entity entity) noexcept;
  /// Copies the entity's reflection probe into the out parameter; logs and
  /// returns false for stale/dead entities or when absent.
  bool get_reflection_probe_component(
      Entity entity, ReflectionProbeComponent *outComponent) const noexcept;
  /// Returns whether has reflection probe component.
  bool has_reflection_probe_component(Entity entity) const noexcept;
  /// Number of live reflection probe components.
  std::size_t reflection_probe_count() const noexcept;
  /// Dense-storage reflection probe at `index` (0..count-1); nullptr out of range.
  const ReflectionProbeComponent *
  reflection_probe_at(std::size_t index) const noexcept;
  /// Entity owning the dense reflection probe slot at `index`; kInvalidEntity
  /// when out of range.
  Entity reflection_probe_entity_at(std::size_t index) const noexcept;
  /// Pointer to the entity's reflection probe component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  ReflectionProbeComponent *
  get_reflection_probe_component_ptr(Entity entity) noexcept;
  /// Pointer to the entity's reflection probe component, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const ReflectionProbeComponent *
  get_reflection_probe_component_ptr(Entity entity) const noexcept;

  /// Adds or replaces the entity's spring arm. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when storage is full.
  bool add_spring_arm(Entity entity,
                      const SpringArmComponent &component) noexcept;
  /// Removes the entity's spring arm. Requires the Input phase and a live
  /// entity; logs and returns false otherwise or when the component is absent.
  bool remove_spring_arm(Entity entity) noexcept;
  /// Copies the entity's spring arm into the out parameter; logs and returns
  /// false for stale or dead entities or when the component is absent.
  bool get_spring_arm(Entity entity,
                      SpringArmComponent *outComponent) const noexcept;
  /// Returns whether has spring arm.
  bool has_spring_arm(Entity entity) const noexcept;
  /// Pointer to the entity's spring arm, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  SpringArmComponent *get_spring_arm_ptr(Entity entity) noexcept;
  /// Pointer to the entity's spring arm, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const SpringArmComponent *get_spring_arm_ptr(Entity entity) const noexcept;

  /// Begins the requested operation or profiling range for update phase.
  void begin_update_phase() noexcept;
  /// Begins the requested operation or profiling range for update step.
  void begin_update_step() noexcept;
  /// Publishes the written transform state as the new read state (swap).
  void commit_update_phase() noexcept;
  /// Begins the requested operation or profiling range for transform phase.
  void begin_transform_phase() noexcept;
  /// Begins the requested operation or profiling range for render prep phase.
  void begin_render_prep_phase() noexcept;
  /// Begins the requested operation or profiling range for render phase.
  void begin_render_phase() noexcept;
  /// Ends the requested operation or profiling range for frame phase.
  void end_frame_phase() noexcept;
  /// Current WorldPhase; component mutation is only legal in Input.
  WorldPhase current_phase() const noexcept;

  // Game mode (owned by World — reset on scene load). -------------------
  GameMode &game_mode() noexcept { return m_gameMode; }
  const GameMode &game_mode() const noexcept { return m_gameMode; }

  // Per-World timer manager (reset on scene load). -------------------------
  TimerManager &timer_manager() noexcept { return m_timerManager; }
  const TimerManager &timer_manager() const noexcept { return m_timerManager; }

  // Per-World camera manager (priority stack + shake). ---------------------
  CameraManager &camera_manager() noexcept { return m_cameraManager; }
  const CameraManager &camera_manager() const noexcept {
    return m_cameraManager;
  }

  // Lifecycle phase helpers ------------------------------------------------
  // BeginPlay: transition Input → BeginPlay. Iterate new entities via
  // for_each_needs_begin_play, then call end_begin_play_phase.
  void begin_begin_play_phase() noexcept;
  /// Ends the requested operation or profiling range for begin play phase.
  void end_begin_play_phase() noexcept;

  // EndPlay: transition Render → EndPlay. Iterate pending-destroy entities
  // via for_each_pending_destroy, then call end_end_play_phase which flushes.
  void begin_end_play_phase() noexcept;
  /// Ends the requested operation or profiling range for end play phase.
  void end_end_play_phase() noexcept;

  // Mark entity as having received its begin_play callback.
  void mark_begin_play_done(Entity entity) noexcept;

  // Number of alive entities that have not yet received begin_play; lets the
  // frame loop skip the BeginPlay phase entirely on quiet frames.
  std::size_t begin_play_pending_count() const noexcept {
    return m_beginPlayPendingCount;
  }

  // Iterate alive entities that have NOT yet received begin_play.
  template <typename Fn> void for_each_needs_begin_play(Fn &&fn) noexcept {
    if ((m_aliveEntityCount == 0U) || (m_beginPlayPendingCount == 0U)) {
      return;
    }
    std::size_t visited = 0U;
    const std::uint32_t upperBound = m_nextEntityIndex;
    for (std::uint32_t index = 1U;
         (index < upperBound) && (visited < m_aliveEntityCount); ++index) {
      if (!m_entityAlive[index]) {
        continue;
      }
      ++visited;
      if (!m_entityBeginPlayFired[index]) {
        fn(Entity{index, m_entityGenerations[index]});
      }
    }
  }

  // Iterate entities pending deferred destruction (read-only snapshot).
  template <typename Fn> void for_each_pending_destroy(Fn &&fn) const noexcept {
    for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
      const Entity entity = m_pendingDestroyEntities[i];
      if (is_valid_entity(entity)) {
        fn(entity);
      }
    }
  }

  /// Invokes the visitor for every local transform in the read state.
  void for_each_transform(TransformVisitor visitor,
                          void *userData) const noexcept;
  /// Advances this system for the current frame or tick for transforms.
  bool update_transforms(float deltaSeconds) noexcept;
  // Thread-safety contract: may be called in parallel during Update with
  // non-overlapping index ranges; the write state index is fixed per update.
  bool update_transforms_range(std::size_t startIndex, std::size_t count,
                               float deltaSeconds) noexcept;
  /// Exposes read/write transform spans for one parallel update chunk; the
  /// write buffer index is fixed for the whole update.
  bool
  get_transform_update_range(std::size_t startIndex, std::size_t count,
                             const Entity **outEntities,
                             const Transform **outReadTransforms,
                             Transform **outWriteTransforms) noexcept override;
  /// Reads transform range data.
  bool read_transform_range(std::size_t startIndex, std::size_t count,
                            const Entity **outEntities,
                            const Transform **outTransforms) const noexcept;
  /// Reads world transform range data.
  bool read_world_transform_range(
      std::size_t startIndex, std::size_t count, const Entity **outEntities,
      const WorldTransform **outTransforms) const noexcept;
  /// Number of live transform components.
  std::size_t transform_count() const noexcept override;
  /// Number of live world transform components.
  std::size_t world_transform_count() const noexcept;
  /// Number of live rigid body components.
  std::size_t rigid_body_count() const noexcept;
  /// Number of live collider components.
  std::size_t collider_count() const noexcept override;

  /// Physics payload storage (gravity, joints, hull/heightfield data).
  physics::PhysicsContext &physics_context() noexcept override;
  const physics::PhysicsContext &physics_context() const noexcept override;

  /// Pointer to the entity's rigid body, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  RigidBody *get_rigid_body_ptr(Entity entity) noexcept override;
  /// Pointer to the entity's rigid body, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const RigidBody *get_rigid_body_ptr(Entity entity) const noexcept override;
  /// Pointer to the entity's collider, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  const Collider *get_collider_ptr(Entity entity) const noexcept override;
  /// Pointer to the entity's collider, or nullptr when the handle is
  /// stale or the component is absent (no logging).
  Collider *get_collider_ptr(Entity entity) noexcept;

  template <typename... Components, typename Fn>
  /// Invokes fn(Entity, const Components&...) for entities that have every
  /// listed component; iterates the smallest set and probes the rest.
  void for_each(Fn &&fn) const noexcept {
    static_assert(sizeof...(Components) >= 1U,
                  "World::for_each requires at least one component type.");
    static_assert((is_supported_component<Components>() && ...),
                  "World::for_each component type is not supported.");

    using ComponentTuple = std::tuple<std::remove_cv_t<Components>...>;
    constexpr std::size_t N = sizeof...(Components);

    if constexpr (N == 1U) {
      using C0 = std::tuple_element_t<0U, ComponentTuple>;
      for_each_primary<C0>(
          [&fn](Entity entity, const C0 &c0) noexcept { fn(entity, c0); });
    } else {
      // Iterate the component set with the fewest entries, then probe the rest.
      for_each_variadic<ComponentTuple>(fn, std::make_index_sequence<N>{});
    }
  }

private:
  using TransformSet = core::SparseSet<Entity, Transform, kMaxEntities,
                                       kMaxTransforms, kStateBufferCount>;
  using WorldTransformSet =
      core::SparseSet<Entity, WorldTransform, kMaxEntities, kMaxTransforms>;
  using RigidBodySet =
      core::SparseSet<Entity, RigidBody, kMaxEntities, kMaxRigidBodies>;
  using ColliderSet =
      core::SparseSet<Entity, Collider, kMaxEntities, kMaxColliders>;
  using MeshComponentSet =
      core::SparseSet<Entity, MeshComponent, kMaxEntities, kMaxMeshComponents>;
  using NameComponentSet =
      core::SparseSet<Entity, NameComponent, kMaxEntities, kMaxNameComponents>;
  using LightComponentSet = core::SparseSet<Entity, LightComponent,
                                            kMaxEntities, kMaxLightComponents>;
  using ScriptComponentSet =
      core::SparseSet<Entity, ScriptComponent, kMaxEntities,
                      kMaxScriptComponents>;
  using SpringArmSet = core::SparseSet<Entity, SpringArmComponent, kMaxEntities,
                                       kMaxSpringArmComponents>;
  using PointLightSet = core::SparseSet<Entity, PointLightComponent,
                                        kMaxEntities, kMaxPointLightComponents>;
  using SpotLightSet = core::SparseSet<Entity, SpotLightComponent, kMaxEntities,
                                       kMaxSpotLightComponents>;
  using ReflectionProbeSet =
      core::SparseSet<Entity, ReflectionProbeComponent, kMaxEntities,
                      kMaxReflectionProbeComponents>;
  using FoliagePatchSet =
      core::SparseSet<Entity, FoliagePatchComponent, kMaxEntities,
                      kMaxFoliagePatchComponents>;

  /// Returns whether is supported component.
  template <typename Component> static consteval bool is_supported_component() {
    using C = std::remove_cv_t<Component>;
    return std::is_same_v<C, Transform> || std::is_same_v<C, RigidBody> ||
           std::is_same_v<C, WorldTransform> || std::is_same_v<C, Collider> ||
           std::is_same_v<C, MeshComponent> ||
           std::is_same_v<C, NameComponent> ||
           std::is_same_v<C, LightComponent> ||
           std::is_same_v<C, ScriptComponent> ||
           std::is_same_v<C, SpringArmComponent> ||
           std::is_same_v<C, PointLightComponent> ||
           std::is_same_v<C, SpotLightComponent> ||
           std::is_same_v<C, ReflectionProbeComponent> ||
           std::is_same_v<C, FoliagePatchComponent>;
  }

  /// Returns whether is mutation phase.
  bool is_mutation_phase() const noexcept;
  /// Returns whether is valid entity.
  bool is_valid_entity(Entity entity) const noexcept;
  /// Destroys or releases the requested object, handle, or resource for entity immediate.
  bool destroy_entity_immediate(Entity entity) noexcept;
  /// Queues the entity for destruction at the EndPlay flush.
  bool queue_deferred_destroy(Entity entity) noexcept;
  /// Flushes queued work to the backing runtime system for deferred destroys.
  void flush_deferred_destroys() noexcept;
  /// Maps a persistent id to its entity index; false when the table is full.
  bool insert_persistent_index(PersistentId persistentId,
                               std::uint32_t entityIndex) noexcept;
  /// Returns the entity index for a persistent id, or 0 when unmapped.
  std::uint32_t find_persistent_index(PersistentId persistentId) const noexcept;
  /// Unmaps a persistent id; rebuilds the table when tombstones pile up.
  void erase_persistent_index(PersistentId persistentId) noexcept;
  /// Rebuilds the persistent-id table from the alive entity arrays.
  void rebuild_persistent_index() noexcept;
  /// Resets this object back to its reusable empty state for transform cache.
  void reset_transform_cache(std::uint32_t entityIndex) noexcept;
  /// Rebuilds parent links and recomputes dirty world transforms.
  bool propagate_world_transforms() noexcept;
  /// Transform state buffer index reads should use in the current phase.
  std::size_t query_state_index() const noexcept;
  // Shared guard/log/dispatch bodies behind the per-component add/remove/get
  // wrappers. Defined in world.cpp; every instantiation lives there.
  /// Phase + liveness guard used by component mutators with extra logic.
  bool check_component_mutation(Entity entity, const char *label) noexcept;
  /// Phase + liveness guarded SparseSet insert; logs failures under `label`.
  template <typename Set, typename Component>
  bool add_component_checked(Set &set, Entity entity,
                             const Component &component,
                             const char *label) noexcept;
  /// Phase + liveness guarded SparseSet remove; logs failures under `label`.
  template <typename Set>
  bool remove_component_checked(Set &set, Entity entity,
                                const char *label) noexcept;
  /// Liveness-guarded SparseSet copy-out; logs failures under `label`.
  template <typename Set, typename Component>
  bool get_component_checked(const Set &set, Entity entity, Component *out,
                             const char *label) const noexcept;
  /// Liveness-guarded SparseSet pointer lookup (silent on miss).
  template <typename Set>
  auto *get_component_ptr_checked(Set &set, Entity entity) noexcept {
    if (!is_valid_entity(entity)) {
      return static_cast<decltype(set.get_ptr(entity))>(nullptr);
    }
    return set.get_ptr(entity);
  }
  /// Liveness-guarded const SparseSet pointer lookup (silent on miss).
  template <typename Set>
  auto *get_component_ptr_checked(const Set &set, Entity entity) const noexcept {
    if (!is_valid_entity(entity)) {
      return static_cast<decltype(set.get_ptr(entity))>(nullptr);
    }
    return set.get_ptr(entity);
  }

  /// Inserts one name-lookup entry (reuses tombstoned slots). Entities that
  /// share a name each keep their own entry.
  bool name_lookup_insert(std::uint32_t nameHash,
                          std::uint32_t entityIndex) noexcept;
  /// Tombstones the lookup entry for one entity's name, if present.
  void name_lookup_erase(std::uint32_t nameHash,
                         std::uint32_t entityIndex) noexcept;
  /// Rebuilds the lookup from live name components (clears tombstones).
  void rebuild_name_lookup() noexcept;

  /// Live component count for the given component type.
  template <typename Component> std::size_t component_count() const noexcept {
    using C = std::remove_cv_t<Component>;
    if constexpr (std::is_same_v<C, Transform>) {
      return m_transforms.count();
    } else if constexpr (std::is_same_v<C, WorldTransform>) {
      return m_worldTransforms.count();
    } else if constexpr (std::is_same_v<C, RigidBody>) {
      return m_rigidBodies.count();
    } else if constexpr (std::is_same_v<C, Collider>) {
      return m_colliders.count();
    } else if constexpr (std::is_same_v<C, MeshComponent>) {
      return m_meshComponents.count();
    } else if constexpr (std::is_same_v<C, NameComponent>) {
      return m_nameComponents.count();
    } else if constexpr (std::is_same_v<C, LightComponent>) {
      return m_lightComponents.count();
    } else if constexpr (std::is_same_v<C, ScriptComponent>) {
      return m_scriptComponents.count();
    } else if constexpr (std::is_same_v<C, SpringArmComponent>) {
      return m_springArms.count();
    } else if constexpr (std::is_same_v<C, PointLightComponent>) {
      return m_pointLights.count();
    } else if constexpr (std::is_same_v<C, SpotLightComponent>) {
      return m_spotLights.count();
    } else if constexpr (std::is_same_v<C, ReflectionProbeComponent>) {
      return m_reflectionProbes.count();
    } else if constexpr (std::is_same_v<C, FoliagePatchComponent>) {
      return m_foliagePatches.count();
    } else {
      return 0U;
    }
  }

  template <typename Component>
  /// Pointer to the entity's component in query state, or nullptr.
  const std::remove_cv_t<Component> *
  try_get_component(Entity entity) const noexcept {
    using C = std::remove_cv_t<Component>;
    if (!is_valid_entity(entity)) {
      return nullptr;
    }

    if constexpr (std::is_same_v<C, Transform>) {
      return m_transforms.get_ptr(entity, query_state_index());
    } else if constexpr (std::is_same_v<C, WorldTransform>) {
      return m_worldTransforms.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, RigidBody>) {
      return m_rigidBodies.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, Collider>) {
      return m_colliders.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, MeshComponent>) {
      return m_meshComponents.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, NameComponent>) {
      return m_nameComponents.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, LightComponent>) {
      return m_lightComponents.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, ScriptComponent>) {
      return m_scriptComponents.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, SpringArmComponent>) {
      return m_springArms.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, PointLightComponent>) {
      return m_pointLights.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, SpotLightComponent>) {
      return m_spotLights.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, ReflectionProbeComponent>) {
      return m_reflectionProbes.get_ptr(entity);
    } else if constexpr (std::is_same_v<C, FoliagePatchComponent>) {
      return m_foliagePatches.get_ptr(entity);
    } else {
      return nullptr;
    }
  }

  // ---- Variadic for_each helpers ----

  // Find the index (within a tuple) of the component type with lowest count.
  template <typename Tuple, std::size_t... Is>
  /// Index (within the tuple) of the component type with fewest entries.
  std::size_t
  smallest_component_index(std::index_sequence<Is...>) const noexcept {
    std::size_t minCount = (std::numeric_limits<std::size_t>::max)();
    std::size_t minIndex = 0U;
    const auto check = [&](std::size_t idx, std::size_t count) noexcept {
      if (count < minCount) {
        minCount = count;
        minIndex = idx;
      }
    };
    (check(Is, component_count<std::tuple_element_t<Is, Tuple>>()), ...);
    return minIndex;
  }

  // Dispatch: iterate the component set at PrimaryIdx, probe the rest.
  template <typename Tuple, std::size_t PrimaryIdx, typename Fn,
            std::size_t... AllIs>
  /// Iterates the primary component set and probes the rest per entity.
  void for_each_with_primary(Fn &&fn,
                             std::index_sequence<AllIs...>) const noexcept {
    using PrimaryC = std::tuple_element_t<PrimaryIdx, Tuple>;
    constexpr std::size_t N = std::tuple_size_v<Tuple>;
    for_each_primary<PrimaryC>(
        [&fn, this](Entity entity, const PrimaryC &primary) noexcept {
          std::array<const void *, N> ptrs{};
          ptrs[PrimaryIdx] = &primary;
          if (try_get_rest_excluding<Tuple, PrimaryIdx>(
                  entity, ptrs, std::make_index_sequence<N>{})) {
            invoke_for_each<Tuple>(fn, entity, ptrs,
                                   std::index_sequence<AllIs...>{});
          }
        });
  }

  template <typename Tuple, std::size_t PrimaryIdx, std::size_t... AllIs>
  /// Fills ptrs for the non-primary components; false when any is absent.
  bool try_get_rest_excluding(
      Entity entity, std::array<const void *, std::tuple_size_v<Tuple>> &ptrs,
      std::index_sequence<AllIs...>) const noexcept {
    bool allPresent = true;
    const auto probe = [&](auto IndexConstant) noexcept {
      constexpr std::size_t I = decltype(IndexConstant)::value;
      if constexpr (I != PrimaryIdx) {
        if (allPresent) {
          ptrs[I] = try_get_component<std::tuple_element_t<I, Tuple>>(entity);
          allPresent = (ptrs[I] != nullptr);
        }
      }
    };
    (probe(std::integral_constant<std::size_t, AllIs>{}), ...);
    return allPresent;
  }

  template <typename Tuple, typename Fn, std::size_t... Is>
  /// Calls fn with the typed component refs recovered from ptrs.
  static void invoke_for_each(
      Fn &&fn, Entity entity,
      const std::array<const void *, std::tuple_size_v<Tuple>> &ptrs,
      std::index_sequence<Is...>) noexcept {
    fn(entity,
       *static_cast<const std::tuple_element_t<Is, Tuple> *>(ptrs[Is])...);
  }

  // Entry point: picks smallest component at runtime and dispatches.
  template <typename Tuple, typename Fn, std::size_t... Is>
  /// Picks the smallest component set at runtime and dispatches on it.
  void for_each_variadic(Fn &&fn, std::index_sequence<Is...>) const noexcept {
    const std::size_t primaryIdx =
        smallest_component_index<Tuple>(std::index_sequence<Is...>{});
    const auto dispatch = [&](auto IndexConstant) noexcept {
      constexpr std::size_t I = decltype(IndexConstant)::value;
      if (I == primaryIdx) {
        for_each_with_primary<Tuple, I>(fn, std::index_sequence<Is...>{});
      }
    };
    (dispatch(std::integral_constant<std::size_t, Is>{}), ...);
  }

  template <typename Component, typename Fn>
  /// Invokes fn(Entity, const C&) over one component type's dense storage.
  void for_each_primary(Fn &&fn) const noexcept {
    using C = std::remove_cv_t<Component>;

    if constexpr (std::is_same_v<C, Transform>) {
      const std::size_t stateIndex = query_state_index();
      for (std::size_t i = 0U; i < m_transforms.count(); ++i) {
        fn(m_transforms.entity_at(i), m_transforms.component_at(i, stateIndex));
      }
    } else if constexpr (std::is_same_v<C, WorldTransform>) {
      for (std::size_t i = 0U; i < m_worldTransforms.count(); ++i) {
        fn(m_worldTransforms.entity_at(i), m_worldTransforms.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, RigidBody>) {
      for (std::size_t i = 0U; i < m_rigidBodies.count(); ++i) {
        fn(m_rigidBodies.entity_at(i), m_rigidBodies.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, Collider>) {
      for (std::size_t i = 0U; i < m_colliders.count(); ++i) {
        fn(m_colliders.entity_at(i), m_colliders.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, MeshComponent>) {
      for (std::size_t i = 0U; i < m_meshComponents.count(); ++i) {
        fn(m_meshComponents.entity_at(i), m_meshComponents.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, NameComponent>) {
      for (std::size_t i = 0U; i < m_nameComponents.count(); ++i) {
        fn(m_nameComponents.entity_at(i), m_nameComponents.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, LightComponent>) {
      for (std::size_t i = 0U; i < m_lightComponents.count(); ++i) {
        fn(m_lightComponents.entity_at(i), m_lightComponents.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, ScriptComponent>) {
      for (std::size_t i = 0U; i < m_scriptComponents.count(); ++i) {
        fn(m_scriptComponents.entity_at(i), m_scriptComponents.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, SpringArmComponent>) {
      for (std::size_t i = 0U; i < m_springArms.count(); ++i) {
        fn(m_springArms.entity_at(i), m_springArms.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, PointLightComponent>) {
      for (std::size_t i = 0U; i < m_pointLights.count(); ++i) {
        fn(m_pointLights.entity_at(i), m_pointLights.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, SpotLightComponent>) {
      for (std::size_t i = 0U; i < m_spotLights.count(); ++i) {
        fn(m_spotLights.entity_at(i), m_spotLights.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, ReflectionProbeComponent>) {
      for (std::size_t i = 0U; i < m_reflectionProbes.count(); ++i) {
        fn(m_reflectionProbes.entity_at(i),
           m_reflectionProbes.component_at(i));
      }
    } else if constexpr (std::is_same_v<C, FoliagePatchComponent>) {
      for (std::size_t i = 0U; i < m_foliagePatches.count(); ++i) {
        fn(m_foliagePatches.entity_at(i), m_foliagePatches.component_at(i));
      }
    }
  }

  WorldPhase m_phase = WorldPhase::Input;
  std::uint32_t m_nextEntityIndex = 1U;
  PersistentId m_nextPersistentId = 1U;
  std::array<std::uint32_t, kMaxEntities + 1U> m_entityGenerations{};
  std::array<PersistentId, kMaxEntities + 1U> m_entityPersistentIds{};
  std::array<MovementAuthority, kMaxEntities + 1U> m_movementAuthorities{};
  core::FixedHashTable<PersistentId, std::uint32_t, kPersistentIndexCapacity>
      m_persistentIndex{};
  std::array<bool, kMaxEntities + 1U> m_entityAlive{};
  std::array<bool, kMaxEntities + 1U> m_entityBeginPlayFired{};
  // Alive entities whose begin_play has not fired yet (kept in sync by
  // create/destroy/mark_begin_play_done).
  std::size_t m_beginPlayPendingCount = 0U;
  std::array<std::uint32_t, kMaxEntities> m_freeEntityIndices{};
  std::size_t m_freeEntityCount = 0U;
  std::size_t m_aliveEntityCount = 0U;
  std::array<Entity, kMaxEntities> m_pendingDestroyEntities{};
  std::size_t m_pendingDestroyCount = 0U;

  // Compact per-entity node for transform propagation.
  // Packs tree links, per-frame flags, and cached local values into one struct
  // so each traversal touch stays within one or two cache lines.
  // Indexed by entity index (sparse; index 0 is the sentinel / no-parent).
  struct TransformNode final {
    // Cached local transform — compared each frame for dirty detection.
    // The I/O path uses the full Transform stored in m_transforms (SparseSet).
    math::Vec3 position{};
    math::Quat rotation{}; // alignas(16)
    math::Vec3 scale{1.0F, 1.0F, 1.0F};
    // Resolved runtime tree links, rebuilt every propagation pass.
    std::uint32_t parentIndex = 0U;
    std::uint32_t firstChild = 0U;
    std::uint32_t lastChild = 0U;
    std::uint32_t nextSibling = 0U;
    // Previous-frame parent info for cache-change detection.
    PersistentId cachedParentId = kInvalidPersistentId;
    std::uint32_t cachedParentIndex = 0U;
    // Per-pass flags.
    bool present = false;
    bool localDirty = false;
    bool cacheValid = false;
    std::uint8_t traversalState = 0U;
  };

  TransformSet m_transforms{};
  WorldTransformSet m_worldTransforms{};
  std::array<TransformNode, kMaxEntities + 1U> m_transformNodes{};
  std::array<std::uint32_t, kMaxEntities> m_transformActiveIndices{};
  std::size_t m_transformActiveCount = 0U;
  std::array<std::uint32_t, kMaxEntities> m_transformRoots{};
  std::array<std::uint32_t, kMaxEntities> m_transformQueueIndices{};
  std::array<bool, kMaxEntities> m_transformQueueInheritedDirty{};
  RigidBodySet m_rigidBodies{};
  ColliderSet m_colliders{};
  MeshComponentSet m_meshComponents{};
  NameComponentSet m_nameComponents{};
  std::array<std::uint32_t, kNameLookupCapacity> m_nameLookupHashes{};
  std::array<std::uint32_t, kNameLookupCapacity> m_nameLookupEntityIndices{};
  std::array<std::uint8_t, kNameLookupCapacity> m_nameLookupState{};
  // Tombstones accumulated by erases; a rebuild resets them so probe chains
  // stay short after heavy name churn.
  std::size_t m_nameLookupTombstones = 0U;
  LightComponentSet m_lightComponents{};
  ScriptComponentSet m_scriptComponents{};
  SpringArmSet m_springArms{};
  PointLightSet m_pointLights{};
  SpotLightSet m_spotLights{};
  ReflectionProbeSet m_reflectionProbes{};
  FoliagePatchSet m_foliagePatches{};
  physics::PhysicsContext m_physicsContext{};
  GameMode m_gameMode{};
  TimerManager m_timerManager{};
  CameraManager m_cameraManager{};

  std::size_t m_readStateIndex = 0U;
  std::size_t m_writeStateIndex = 1U;
  bool m_updateSwapPending = false;
};

} // namespace engine::runtime
