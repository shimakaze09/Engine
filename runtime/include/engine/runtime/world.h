#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>

#include "engine/core/sparse_set.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::runtime {

#ifndef ENGINE_MAX_ENTITIES
#define ENGINE_MAX_ENTITIES 65536U
#endif

#ifndef ENGINE_MAX_LIGHT_COMPONENTS
#define ENGINE_MAX_LIGHT_COMPONENTS 1024U
#endif

struct Entity final {
  std::uint32_t index = 0U;
  std::uint32_t generation = 0U;

  friend constexpr bool operator==(const Entity &, const Entity &) = default;
};

[[maybe_unused]] inline constexpr Entity kInvalidEntity{};
using PersistentId = std::uint32_t;
inline constexpr PersistentId kInvalidPersistentId = 0U;

struct Transform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  // References parent by stable persistent ID, not transient runtime handles.
  PersistentId parentId = kInvalidPersistentId;
};

struct WorldTransform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Mat4 matrix = math::Mat4();
};

struct RigidBody final {
  math::Vec3 velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 acceleration = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 angularVelocity = math::Vec3(0.0F, 0.0F, 0.0F);
  float inverseMass = 1.0F;
  float inverseInertia = 1.0F;
  std::uint8_t sleepFrameCount = 0U;
  bool sleeping = false;
};

enum class ColliderShape : std::uint8_t { AABB = 0, Sphere = 1 };

struct Collider final {
  math::Vec3 halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  float restitution = 0.3F;
  float staticFriction = 0.5F;
  float dynamicFriction = 0.3F;
  ColliderShape shape = ColliderShape::AABB;
};

struct NameComponent final {
  char name[32] = {};
};

enum class LightType : std::uint8_t { Directional = 0, Point = 1 };

struct LightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 direction = math::Vec3(0.4F, -1.0F, 0.6F);
  float intensity = 1.0F;
  LightType type = LightType::Directional;
};

// Attaches a Lua script file to an entity.
// The script must return a module table with optional on_start(self) and
// on_update(self, dt) functions. Multiple entities may share the same file.
struct ScriptComponent final {
  char scriptPath[128] = {};
};

// Renderer-facing component; keep minimal to avoid bloating draw commands.
struct MeshComponent final {
  std::uint32_t meshAssetId = 0U;
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
};

using TransformVisitor = void (*)(Entity entity, const Transform &transform,
                                  void *userData) noexcept;

enum class WorldPhase : std::uint8_t {
  Input,
  Simulation,
  TransformPropagation,
  RenderSubmission,
  Render,
  // Compatibility aliases for legacy call sites.
  Idle = Input,
  Update = Simulation,
  RenderPrep = RenderSubmission,
};

enum class MovementAuthority : std::uint8_t {
  None,
  Script,
};

class World final {
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
  static constexpr std::size_t kNameLookupCapacity = kMaxNameComponents * 2U;
  static constexpr std::size_t kStateBufferCount = 2U;
  static constexpr std::size_t kPersistentIndexCapacity = kMaxEntities * 2U;
  static constexpr std::size_t kMaxPhysicsJoints = 4096U;
  static constexpr std::size_t kMaxCollisionPairs = 1024U;
  static constexpr std::size_t kCollisionPairHashBuckets = 4096U;
  using PhysicsCollisionDispatchFn = void (*)(const std::uint32_t *pairs,
                                              std::size_t pairCount) noexcept;

  struct PhysicsJointSlot final {
    Entity entityA = kInvalidEntity;
    Entity entityB = kInvalidEntity;
    float distance = 1.0F;
    bool active = false;
  };

  struct PhysicsContext final {
    math::Vec3 gravity = math::Vec3(0.0F, -9.8F, 0.0F);
    std::array<PhysicsJointSlot, kMaxPhysicsJoints> joints{};
    std::size_t jointCount = 0U;

    // Packed collision pairs: [entityIndexA0, entityIndexB0, ...]
    std::array<std::uint32_t, kMaxCollisionPairs * 2U> collisionPairData{};
    std::size_t collisionPairCount = 0U;
    PhysicsCollisionDispatchFn collisionDispatch = nullptr;

    // O(1) collision-pair dedupe via open addressing and generation stamps.
    std::array<std::uint64_t, kCollisionPairHashBuckets> pairHashKeys{};
    std::array<std::uint32_t, kCollisionPairHashBuckets> pairHashStamps{};
    std::uint32_t pairHashGeneration = 1U;

    // O(1) broadphase neighbor dedupe using per-collider generation stamps.
    std::array<std::uint32_t, kMaxColliders> testedStamps{};
    std::uint32_t testedGeneration = 1U;
  };

  World() noexcept;

  Entity create_entity() noexcept;
  Entity create_entity_with_persistent_id(PersistentId persistentId) noexcept;
  bool destroy_entity(Entity entity) noexcept;
  bool is_alive(Entity entity) const noexcept;
  Entity find_entity_by_index(std::uint32_t index) const noexcept;
  Entity find_entity_by_persistent_id(PersistentId persistentId) const noexcept;
  PersistentId persistent_id(Entity entity) const noexcept;
  std::size_t alive_entity_count() const noexcept;

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

  bool add_transform(Entity entity, const Transform &transform) noexcept;
  bool remove_transform(Entity entity) noexcept;
  bool get_transform(Entity entity, Transform *outTransform) const noexcept;
  // Read buffer is last committed state; during Idle this is the previous
  // frame.
  const Transform *get_transform_read_ptr(Entity entity) const noexcept;
  class SimulationAccessToken final {
  public:
    constexpr bool valid() const noexcept { return m_valid; }

  private:
    explicit constexpr SimulationAccessToken(bool isValid) noexcept
        : m_valid(isValid) {}
    bool m_valid = false;
    friend class World;
  };
  SimulationAccessToken simulation_access_token() const noexcept;
  Transform *
  get_transform_write_ptr(Entity entity,
                          const SimulationAccessToken &token) noexcept;
  const WorldTransform *
  get_world_transform_read_ptr(Entity entity) const noexcept;
  bool set_movement_authority(Entity entity,
                              MovementAuthority authority) noexcept;
  MovementAuthority movement_authority(Entity entity) const noexcept;

  bool add_rigid_body(Entity entity, const RigidBody &rigidBody) noexcept;
  bool remove_rigid_body(Entity entity) noexcept;
  bool get_rigid_body(Entity entity, RigidBody *outRigidBody) const noexcept;

  bool add_collider(Entity entity, const Collider &collider) noexcept;
  bool remove_collider(Entity entity) noexcept;
  bool get_collider(Entity entity, Collider *outCollider) const noexcept;
  bool get_collider_range(std::size_t startIndex, std::size_t count,
                          const Entity **outEntities,
                          const Collider **outColliders) const noexcept;

  bool add_mesh_component(Entity entity,
                          const MeshComponent &component) noexcept;
  bool remove_mesh_component(Entity entity) noexcept;
  bool get_mesh_component(Entity entity,
                          MeshComponent *outComponent) const noexcept;
  MeshComponent *get_mesh_component_ptr(Entity entity) noexcept;
  const MeshComponent *get_mesh_component_ptr(Entity entity) const noexcept;

  bool add_name_component(Entity entity,
                          const NameComponent &component) noexcept;
  bool remove_name_component(Entity entity) noexcept;
  bool get_name_component(Entity entity,
                          NameComponent *outComponent) const noexcept;
  NameComponent *get_name_component_ptr(Entity entity) noexcept;
  const NameComponent *get_name_component_ptr(Entity entity) const noexcept;
  Entity find_entity_by_name(const char *name) const noexcept;

  bool add_script_component(Entity entity,
                            const ScriptComponent &component) noexcept;
  bool remove_script_component(Entity entity) noexcept;
  bool get_script_component(Entity entity,
                            ScriptComponent *outComponent) const noexcept;
  ScriptComponent *get_script_component_ptr(Entity entity) noexcept;
  const ScriptComponent *get_script_component_ptr(Entity entity) const noexcept;

  bool add_light_component(Entity entity,
                           const LightComponent &component) noexcept;
  bool remove_light_component(Entity entity) noexcept;
  bool get_light_component(Entity entity,
                           LightComponent *outComponent) const noexcept;
  bool has_light_component(Entity entity) const noexcept;
  std::size_t light_count() const noexcept;
  const LightComponent *light_at(std::size_t index) const noexcept;
  Entity light_entity_at(std::size_t index) const noexcept;

  void begin_update_phase() noexcept;
  void begin_update_step() noexcept;
  void commit_update_phase() noexcept;
  void begin_transform_phase() noexcept;
  void begin_render_prep_phase() noexcept;
  void begin_render_phase() noexcept;
  void end_frame_phase() noexcept;
  WorldPhase current_phase() const noexcept;

  void for_each_transform(TransformVisitor visitor,
                          void *userData) const noexcept;
  bool update_transforms(float deltaSeconds) noexcept;
  // Thread-safety contract: may be called in parallel during Update with
  // non-overlapping index ranges; the write state index is fixed per update.
  bool update_transforms_range(std::size_t startIndex, std::size_t count,
                               float deltaSeconds) noexcept;
  bool get_transform_update_range(std::size_t startIndex, std::size_t count,
                                  const Entity **outEntities,
                                  const Transform **outReadTransforms,
                                  Transform **outWriteTransforms) noexcept;
  bool read_transform_range(std::size_t startIndex, std::size_t count,
                            const Entity **outEntities,
                            const Transform **outTransforms) const noexcept;
  bool read_world_transform_range(
      std::size_t startIndex, std::size_t count, const Entity **outEntities,
      const WorldTransform **outTransforms) const noexcept;
  std::size_t transform_count() const noexcept;
  std::size_t world_transform_count() const noexcept;
  std::size_t rigid_body_count() const noexcept;
  std::size_t collider_count() const noexcept;

  PhysicsContext &physics_context() noexcept;
  const PhysicsContext &physics_context() const noexcept;

  RigidBody *get_rigid_body_ptr(Entity entity) noexcept;
  const RigidBody *get_rigid_body_ptr(Entity entity) const noexcept;
  const Collider *get_collider_ptr(Entity entity) const noexcept;
  Collider *get_collider_ptr(Entity entity) noexcept;

  template <typename... Components, typename Fn>
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

  template <typename Component> static consteval bool is_supported_component() {
    using C = std::remove_cv_t<Component>;
    return std::is_same_v<C, Transform> || std::is_same_v<C, RigidBody> ||
           std::is_same_v<C, WorldTransform> || std::is_same_v<C, Collider> ||
           std::is_same_v<C, MeshComponent> ||
           std::is_same_v<C, NameComponent> ||
           std::is_same_v<C, LightComponent> ||
           std::is_same_v<C, ScriptComponent>;
  }

  bool is_mutation_phase() const noexcept;
  bool is_valid_entity(Entity entity) const noexcept;
  bool destroy_entity_immediate(Entity entity) noexcept;
  bool queue_deferred_destroy(Entity entity) noexcept;
  void flush_deferred_destroys() noexcept;
  bool insert_persistent_index(PersistentId persistentId,
                               std::uint32_t entityIndex) noexcept;
  std::uint32_t find_persistent_index(PersistentId persistentId) const noexcept;
  void erase_persistent_index(PersistentId persistentId) noexcept;
  void reset_transform_cache(std::uint32_t entityIndex) noexcept;
  bool propagate_world_transforms() noexcept;
  std::size_t query_state_index() const noexcept;
  std::uint32_t hash_name_string(const char *name) const noexcept;
  bool name_lookup_insert(std::uint32_t nameHash, const char *name,
                          std::uint32_t entityIndex) noexcept;
  void rebuild_name_lookup() noexcept;

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
    } else {
      return 0U;
    }
  }

  template <typename Component>
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
    } else {
      return nullptr;
    }
  }

  // ---- Variadic for_each helpers ----

  // Find the index (within a tuple) of the component type with lowest count.
  template <typename Tuple, std::size_t... Is>
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
  static void invoke_for_each(
      Fn &&fn, Entity entity,
      const std::array<const void *, std::tuple_size_v<Tuple>> &ptrs,
      std::index_sequence<Is...>) noexcept {
    fn(entity,
       *static_cast<const std::tuple_element_t<Is, Tuple> *>(ptrs[Is])...);
  }

  // Entry point: picks smallest component at runtime and dispatches.
  template <typename Tuple, typename Fn, std::size_t... Is>
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
    }
  }

  WorldPhase m_phase = WorldPhase::Idle;
  std::uint32_t m_nextEntityIndex = 1U;
  PersistentId m_nextPersistentId = 1U;
  std::array<std::uint32_t, kMaxEntities + 1U> m_entityGenerations{};
  std::array<PersistentId, kMaxEntities + 1U> m_entityPersistentIds{};
  std::array<MovementAuthority, kMaxEntities + 1U> m_movementAuthorities{};
  std::array<PersistentId, kPersistentIndexCapacity> m_persistentIndexKeys{};
  std::array<std::uint32_t, kPersistentIndexCapacity> m_persistentIndexValues{};
  std::array<std::uint8_t, kPersistentIndexCapacity> m_persistentIndexState{};
  std::array<bool, kMaxEntities + 1U> m_entityAlive{};
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
  LightComponentSet m_lightComponents{};
  ScriptComponentSet m_scriptComponents{};
  PhysicsContext m_physicsContext{};

  std::size_t m_readStateIndex = 0U;
  std::size_t m_writeStateIndex = 1U;
  bool m_updateSwapPending = false;
};

} // namespace engine::runtime
