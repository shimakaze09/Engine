#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

#include "engine/core/sparse_set.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/renderer/command_buffer.h"

namespace engine::runtime {

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
  float inverseMass = 1.0F;
};

struct Collider final {
  math::Vec3 halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
};

struct NameComponent final {
  char name[32] = {};
};

// Renderer-facing component; keep minimal to avoid bloating draw commands.
struct MeshComponent final {
  std::uint32_t meshAssetId = 0U;
  renderer::Material material{};
};

using TransformVisitor = void (*)(Entity entity,
                                  const Transform &transform,
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
  static constexpr std::size_t kMaxEntities = 16384U;
  static constexpr std::size_t kMaxTransforms = 16384U;
  static constexpr std::size_t kMaxRigidBodies = 16384U;
  static constexpr std::size_t kMaxColliders = 16384U;
  static constexpr std::size_t kMaxMeshComponents = 16384U;
  static constexpr std::size_t kMaxNameComponents = 16384U;
  static constexpr std::size_t kStateBufferCount = 2U;
  static constexpr std::size_t kPersistentIndexCapacity = kMaxEntities * 2U;

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
         (index < upperBound) && (visited < m_aliveEntityCount);
         ++index) {
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
  Transform *get_transform_write_ptr(Entity entity) noexcept;
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
  bool get_collider_range(std::size_t startIndex,
                          std::size_t count,
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

  void begin_update_phase() noexcept;
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
  bool update_transforms_range(std::size_t startIndex,
                               std::size_t count,
                               float deltaSeconds) noexcept;
  bool get_transform_update_range(std::size_t startIndex,
                                  std::size_t count,
                                  const Entity **outEntities,
                                  const Transform **outReadTransforms,
                                  Transform **outWriteTransforms) noexcept;
  bool read_transform_range(std::size_t startIndex,
                            std::size_t count,
                            const Entity **outEntities,
                            const Transform **outTransforms) const noexcept;
  bool read_world_transform_range(
      std::size_t startIndex,
      std::size_t count,
      const Entity **outEntities,
      const WorldTransform **outTransforms) const noexcept;
  std::size_t transform_count() const noexcept;
  std::size_t world_transform_count() const noexcept;
  std::size_t rigid_body_count() const noexcept;
  std::size_t collider_count() const noexcept;

  RigidBody *get_rigid_body_ptr(Entity entity) noexcept;
  const RigidBody *get_rigid_body_ptr(Entity entity) const noexcept;
  const Collider *get_collider_ptr(Entity entity) const noexcept;

  template <typename... Components, typename Fn>
  void for_each(Fn &&fn) const noexcept {
    static_assert((sizeof...(Components) >= 1U)
                      && (sizeof...(Components) <= 2U),
                  "World::for_each supports one or two component types.");
    static_assert((is_supported_component<Components>() && ...),
                  "World::for_each component type is not supported.");

    using ComponentTuple = std::tuple<Components...>;
    if constexpr (sizeof...(Components) == 1U) {
      using C0 = std::remove_cv_t<std::tuple_element_t<0U, ComponentTuple>>;
      for_each_primary<C0>(
          [&fn](Entity entity, const C0 &c0) noexcept { fn(entity, c0); });
    } else {
      using C0 = std::remove_cv_t<std::tuple_element_t<0U, ComponentTuple>>;
      using C1 = std::remove_cv_t<std::tuple_element_t<1U, ComponentTuple>>;

      if (component_count<C0>() <= component_count<C1>()) {
        for_each_primary<C0>([&fn, this](Entity entity, const C0 &c0) noexcept {
          const C1 *c1 = try_get_component<C1>(entity);
          if (c1 != nullptr) {
            fn(entity, c0, *c1);
          }
        });
      } else {
        for_each_primary<C1>([&fn, this](Entity entity, const C1 &c1) noexcept {
          const C0 *c0 = try_get_component<C0>(entity);
          if (c0 != nullptr) {
            fn(entity, *c0, c1);
          }
        });
      }
    }
  }

private:
  using TransformSet = core::SparseSet<Entity,
                                       Transform,
                                       kMaxEntities,
                                       kMaxTransforms,
                                       kStateBufferCount>;
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

  template <typename Component> static consteval bool is_supported_component() {
    using C = std::remove_cv_t<Component>;
    return std::is_same_v<C, Transform> || std::is_same_v<C, RigidBody>
           || std::is_same_v<C, WorldTransform> || std::is_same_v<C, Collider>
           || std::is_same_v<C, MeshComponent>
           || std::is_same_v<C, NameComponent>;
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
  static bool transform_equals(const Transform &lhs,
                               const Transform &rhs) noexcept;
  void reset_transform_cache(std::uint32_t entityIndex) noexcept;
  bool propagate_world_transforms() noexcept;
  std::size_t query_state_index() const noexcept;

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
    }

    return nullptr;
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

  TransformSet m_transforms{};
  WorldTransformSet m_worldTransforms{};
  std::array<std::uint32_t, kMaxEntities + 1U> m_transformParentIndex{};
  std::array<std::uint32_t, kMaxEntities + 1U> m_transformFirstChild{};
  std::array<std::uint32_t, kMaxEntities + 1U> m_transformLastChild{};
  std::array<std::uint32_t, kMaxEntities + 1U> m_transformNextSibling{};
  std::array<std::uint32_t, kMaxEntities> m_transformActiveIndices{};
  std::size_t m_transformActiveCount = 0U;
  std::array<std::uint32_t, kMaxEntities> m_transformRoots{};
  std::array<std::uint32_t, kMaxEntities> m_transformQueueIndices{};
  std::array<bool, kMaxEntities> m_transformQueueInheritedDirty{};
  std::array<std::uint8_t, kMaxEntities + 1U> m_transformTraversalState{};
  std::array<bool, kMaxEntities + 1U> m_transformPresent{};
  std::array<bool, kMaxEntities + 1U> m_transformLocalDirty{};
  std::array<bool, kMaxEntities + 1U> m_transformCacheValid{};
  std::array<PersistentId, kMaxEntities + 1U> m_cachedParentIds{};
  std::array<std::uint32_t, kMaxEntities + 1U> m_cachedParentIndices{};
  std::array<Transform, kMaxEntities + 1U> m_cachedLocalTransforms{};
  RigidBodySet m_rigidBodies{};
  ColliderSet m_colliders{};
  MeshComponentSet m_meshComponents{};
  NameComponentSet m_nameComponents{};

  std::size_t m_readStateIndex = 0U;
  std::size_t m_writeStateIndex = 1U;
  bool m_updateSwapPending = false;
};

} // namespace engine::runtime
