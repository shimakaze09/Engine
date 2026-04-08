#include "engine/runtime/world.h"

#include "engine/core/logging.h"
#include "engine/math/transform.h"
#include "engine/runtime/reflect_types.h"

#include <array>
#include <cassert>

namespace engine::runtime {

namespace {

WorldTransform world_transform_from_local(const Transform &local) noexcept {
  WorldTransform world{};
  world.position = local.position;
  world.rotation = local.rotation;
  world.scale = local.scale;
  world.matrix = math::compose_trs(world.position, world.rotation, world.scale);
  return world;
}

} // namespace

World::World() noexcept {
  ensure_runtime_reflection_registered();
  m_entityGenerations.fill(0U);
  m_entityPersistentIds.fill(kInvalidPersistentId);
  m_movementAuthorities.fill(MovementAuthority::None);
  m_persistentIndexKeys.fill(kInvalidPersistentId);
  m_persistentIndexValues.fill(0U);
  m_persistentIndexState.fill(0U);
  m_entityAlive.fill(false);
  m_aliveEntityCount = 0U;
  m_transforms.clear();
  m_worldTransforms.clear();
  m_transformNodes.fill(TransformNode{});
  m_transformActiveIndices.fill(0U);
  m_transformActiveCount = 0U;
  m_transformRoots.fill(0U);
  m_transformQueueIndices.fill(0U);
  m_transformQueueInheritedDirty.fill(false);
  m_rigidBodies.clear();
  m_colliders.clear();
  m_meshComponents.clear();
  m_nameComponents.clear();
  m_scriptComponents.clear();
}

Entity World::create_entity() noexcept {
  return create_entity_with_persistent_id(kInvalidPersistentId);
}

Entity
World::create_entity_with_persistent_id(PersistentId persistentId) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "create_entity requires Input phase");
    return kInvalidEntity;
  }

  if ((persistentId != kInvalidPersistentId) &&
      (find_persistent_index(persistentId) != 0U)) {
    return kInvalidEntity;
  }

  std::uint32_t index = 0U;
  if (m_freeEntityCount > 0U) {
    --m_freeEntityCount;
    index = m_freeEntityIndices[m_freeEntityCount];
  } else {
    if (m_nextEntityIndex > static_cast<std::uint32_t>(kMaxEntities)) {
      return kInvalidEntity;
    }

    index = m_nextEntityIndex;
    ++m_nextEntityIndex;
  }

  if (m_entityGenerations[index] == 0U) {
    m_entityGenerations[index] = 1U;
  }

  if (persistentId == kInvalidPersistentId) {
    const std::uint32_t startCandidate = m_nextPersistentId;
    do {
      if (m_nextPersistentId == kInvalidPersistentId) {
        ++m_nextPersistentId;
      }

      if (find_persistent_index(m_nextPersistentId) == 0U) {
        persistentId = m_nextPersistentId;
        ++m_nextPersistentId;
        if (m_nextPersistentId == kInvalidPersistentId) {
          ++m_nextPersistentId;
        }
        break;
      }

      ++m_nextPersistentId;
      if (m_nextPersistentId == kInvalidPersistentId) {
        ++m_nextPersistentId;
      }
    } while (m_nextPersistentId != startCandidate);

    if (persistentId == kInvalidPersistentId) {
      return kInvalidEntity;
    }
  }

  m_entityAlive[index] = true;
  m_entityPersistentIds[index] = persistentId;
  m_movementAuthorities[index] = MovementAuthority::None;
  if (!insert_persistent_index(persistentId, index)) {
    m_entityAlive[index] = false;
    m_entityPersistentIds[index] = kInvalidPersistentId;
    m_movementAuthorities[index] = MovementAuthority::None;
    if (m_freeEntityCount < m_freeEntityIndices.size()) {
      m_freeEntityIndices[m_freeEntityCount] = index;
      ++m_freeEntityCount;
    }
    return kInvalidEntity;
  }
  ++m_aliveEntityCount;
  return Entity{index, m_entityGenerations[index]};
}

bool World::destroy_entity_immediate(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  static_cast<void>(m_transforms.remove(entity));
  static_cast<void>(m_worldTransforms.remove(entity));
  static_cast<void>(m_rigidBodies.remove(entity));
  static_cast<void>(m_colliders.remove(entity));
  static_cast<void>(m_meshComponents.remove(entity));
  static_cast<void>(m_nameComponents.remove(entity));
  static_cast<void>(m_lightComponents.remove(entity));

  const std::uint32_t index = entity.index;
  erase_persistent_index(m_entityPersistentIds[index]);
  m_entityAlive[index] = false;
  m_entityPersistentIds[index] = kInvalidPersistentId;
  m_movementAuthorities[index] = MovementAuthority::None;
  reset_transform_cache(index);
  if (m_aliveEntityCount > 0U) {
    --m_aliveEntityCount;
  }

  ++m_entityGenerations[index];
  if (m_entityGenerations[index] == 0U) {
    m_entityGenerations[index] = 1U;
  }

  if (m_freeEntityCount < m_freeEntityIndices.size()) {
    m_freeEntityIndices[m_freeEntityCount] = index;
    ++m_freeEntityCount;
  }

  return true;
}

bool World::queue_deferred_destroy(Entity entity) noexcept {
  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    if (m_pendingDestroyEntities[i] == entity) {
      return true;
    }
  }

  if (m_pendingDestroyCount >= m_pendingDestroyEntities.size()) {
    return false;
  }

  m_pendingDestroyEntities[m_pendingDestroyCount] = entity;
  ++m_pendingDestroyCount;
  return true;
}

void World::flush_deferred_destroys() noexcept {
  if (m_pendingDestroyCount == 0U) {
    return;
  }

  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    const Entity entity = m_pendingDestroyEntities[i];
    if (is_valid_entity(entity)) {
      static_cast<void>(destroy_entity_immediate(entity));
    }
  }

  m_pendingDestroyCount = 0U;
}

bool World::destroy_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "destroy_entity requires a live entity");
    return false;
  }

  if (m_phase == WorldPhase::Simulation) {
    return queue_deferred_destroy(entity);
  }

  if (m_phase != WorldPhase::Input) {
    return false;
  }

  return destroy_entity_immediate(entity);
}

bool World::is_alive(Entity entity) const noexcept {
  return is_valid_entity(entity);
}

Entity World::find_entity_by_index(std::uint32_t index) const noexcept {
  if ((index == 0U) || (index > static_cast<std::uint32_t>(kMaxEntities))) {
    return kInvalidEntity;
  }

  if (!m_entityAlive[index]) {
    return kInvalidEntity;
  }

  return Entity{index, m_entityGenerations[index]};
}

Entity
World::find_entity_by_persistent_id(PersistentId persistentId) const noexcept {
  if (persistentId == kInvalidPersistentId) {
    return kInvalidEntity;
  }

  const std::uint32_t index = find_persistent_index(persistentId);
  if ((index == 0U) || (index > static_cast<std::uint32_t>(kMaxEntities))) {
    return kInvalidEntity;
  }

  if (!m_entityAlive[index] || (m_entityPersistentIds[index] != persistentId)) {
    return kInvalidEntity;
  }

  return Entity{index, m_entityGenerations[index]};
}

PersistentId World::persistent_id(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return kInvalidPersistentId;
  }

  return m_entityPersistentIds[entity.index];
}

std::size_t World::alive_entity_count() const noexcept {
  return m_aliveEntityCount;
}

bool World::add_transform(Entity entity, const Transform &transform) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_transform requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_transform requires a live entity");
    return false;
  }

  if (!m_transforms.add(entity, transform)) {
    return false;
  }

  const WorldTransform world = world_transform_from_local(transform);
  m_transformNodes[entity.index].cacheValid = false;
  return m_worldTransforms.add(entity, world);
}

bool World::remove_transform(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_transform requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_transform requires a live entity");
    return false;
  }

  const bool removed = m_transforms.remove(entity);
  static_cast<void>(m_worldTransforms.remove(entity));
  reset_transform_cache(entity.index);
  return removed;
}

bool World::get_transform(Entity entity,
                          Transform *outTransform) const noexcept {
  if (outTransform == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_transform on stale or dead entity");
    return false;
  }

  return m_transforms.get(entity, outTransform, m_readStateIndex);
}

const Transform *World::get_transform_read_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_transforms.get_ptr(entity, m_readStateIndex);
}

Transform *World::get_transform_write_ptr(Entity entity) noexcept {
  if (!is_valid_entity(entity) || (m_phase != WorldPhase::Simulation)) {
    return nullptr;
  }

  return m_transforms.get_ptr(entity, m_writeStateIndex);
}

const WorldTransform *
World::get_world_transform_read_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_worldTransforms.get_ptr(entity);
}

bool World::set_movement_authority(Entity entity,
                                   MovementAuthority authority) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "set_movement_authority requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "set_movement_authority requires a live entity");
    return false;
  }

  m_movementAuthorities[entity.index] = authority;
  return true;
}

MovementAuthority World::movement_authority(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return MovementAuthority::None;
  }

  return m_movementAuthorities[entity.index];
}

bool World::add_rigid_body(Entity entity, const RigidBody &rigidBody) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_rigid_body requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_rigid_body requires a live entity");
    return false;
  }

  return m_rigidBodies.add(entity, rigidBody);
}

bool World::remove_rigid_body(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_rigid_body requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_rigid_body requires a live entity");
    return false;
  }

  return m_rigidBodies.remove(entity);
}

bool World::get_rigid_body(Entity entity,
                           RigidBody *outRigidBody) const noexcept {
  if (outRigidBody == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_rigid_body on stale or dead entity");
    return false;
  }

  return m_rigidBodies.get(entity, outRigidBody);
}

bool World::add_collider(Entity entity, const Collider &collider) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_collider requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_collider requires a live entity");
    return false;
  }

  return m_colliders.add(entity, collider);
}

bool World::remove_collider(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_collider requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_collider requires a live entity");
    return false;
  }

  return m_colliders.remove(entity);
}

bool World::get_collider(Entity entity, Collider *outCollider) const noexcept {
  if (outCollider == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_collider on stale or dead entity");
    return false;
  }

  return m_colliders.get(entity, outCollider);
}

bool World::add_mesh_component(Entity entity,
                               const MeshComponent &component) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_mesh_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_mesh_component requires a live entity");
    return false;
  }

  return m_meshComponents.add(entity, component);
}

bool World::remove_mesh_component(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_mesh_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_mesh_component requires a live entity");
    return false;
  }

  return m_meshComponents.remove(entity);
}

bool World::get_mesh_component(Entity entity,
                               MeshComponent *outComponent) const noexcept {
  if (outComponent == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_mesh_component on stale or dead entity");
    return false;
  }

  return m_meshComponents.get(entity, outComponent);
}

MeshComponent *World::get_mesh_component_ptr(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_meshComponents.get_ptr(entity);
}

const MeshComponent *
World::get_mesh_component_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_meshComponents.get_ptr(entity);
}

bool World::add_name_component(Entity entity,
                               const NameComponent &component) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_name_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_name_component requires a live entity");
    return false;
  }

  return m_nameComponents.add(entity, component);
}

bool World::remove_name_component(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_name_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_name_component requires a live entity");
    return false;
  }

  return m_nameComponents.remove(entity);
}

bool World::get_name_component(Entity entity,
                               NameComponent *outComponent) const noexcept {
  if (outComponent == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_name_component on stale or dead entity");
    return false;
  }

  return m_nameComponents.get(entity, outComponent);
}

NameComponent *World::get_name_component_ptr(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_nameComponents.get_ptr(entity);
}

const NameComponent *
World::get_name_component_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_nameComponents.get_ptr(entity);
}

bool World::add_light_component(Entity entity,
                                const LightComponent &component) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_light_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_light_component requires a live entity");
    return false;
  }

  return m_lightComponents.add(entity, component);
}

bool World::remove_light_component(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_light_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_light_component requires a live entity");
    return false;
  }

  return m_lightComponents.remove(entity);
}

bool World::get_light_component(Entity entity,
                                LightComponent *outComponent) const noexcept {
  if (outComponent == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_light_component on stale or dead entity");
    return false;
  }

  return m_lightComponents.get(entity, outComponent);
}

bool World::has_light_component(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  return m_lightComponents.contains(entity);
}

std::size_t World::light_count() const noexcept {
  return m_lightComponents.count();
}

const LightComponent *World::light_at(std::size_t index) const noexcept {
  if (index >= m_lightComponents.count()) {
    return nullptr;
  }
  return &m_lightComponents.component_at(index);
}

Entity World::light_entity_at(std::size_t index) const noexcept {
  if (index >= m_lightComponents.count()) {
    return kInvalidEntity;
  }
  return m_lightComponents.entity_at(index);
}

bool World::add_script_component(Entity entity,
                                 const ScriptComponent &component) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_script_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_script_component requires a live entity");
    return false;
  }

  return m_scriptComponents.add(entity, component);
}

bool World::remove_script_component(Entity entity) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_script_component requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "remove_script_component requires a live entity");
    return false;
  }

  return m_scriptComponents.remove(entity);
}

bool World::get_script_component(Entity entity,
                                 ScriptComponent *outComponent) const noexcept {
  if (outComponent == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_script_component on stale or dead entity");
    return false;
  }

  return m_scriptComponents.get(entity, outComponent);
}

ScriptComponent *World::get_script_component_ptr(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_scriptComponents.get_ptr(entity);
}

const ScriptComponent *
World::get_script_component_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_scriptComponents.get_ptr(entity);
}

bool World::get_collider_range(std::size_t startIndex, std::size_t count,
                               const Entity **outEntities,
                               const Collider **outColliders) const noexcept {
  if ((outEntities == nullptr) || (outColliders == nullptr)) {
    return false;
  }

  const std::size_t colliderCount = m_colliders.count();
  if (startIndex > colliderCount) {
    return false;
  }

  const std::size_t remaining = colliderCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_colliders.entity_data();
  const Collider *colliders = m_colliders.component_data();
  if ((entities == nullptr) || (colliders == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outColliders = colliders + startIndex;
  return true;
}

void World::begin_update_phase() noexcept {
  if (m_phase != WorldPhase::Input) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_phase requires Input phase");
    return;
  }

  m_writeStateIndex = (m_readStateIndex + 1U) % kStateBufferCount;
  m_updateSwapPending = true;
  m_phase = WorldPhase::Simulation;
}

void World::begin_update_step() noexcept {
  if (m_phase != WorldPhase::Simulation) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_step requires Simulation phase");
    return;
  }
  if (m_updateSwapPending) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_step called with pending update");
    return;
  }
  m_updateSwapPending = true;
}

void World::commit_update_phase() noexcept {
  if ((m_phase != WorldPhase::Simulation) || !m_updateSwapPending) {
    core::log_message(core::LogLevel::Error, "world",
                      "commit_update_phase requires active Simulation phase");
    return;
  }

  flush_deferred_destroys();

  m_readStateIndex = m_writeStateIndex;
  m_writeStateIndex = (m_readStateIndex + 1U) % kStateBufferCount;
  m_updateSwapPending = false;
}

void World::begin_transform_phase() noexcept {
  if ((m_phase != WorldPhase::Simulation) && (m_phase != WorldPhase::Input)) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_transform_phase requires Input or Simulation");
    return;
  }

  if ((m_phase == WorldPhase::Simulation) && m_updateSwapPending) {
    flush_deferred_destroys();

    m_readStateIndex = m_writeStateIndex;
    m_writeStateIndex = (m_readStateIndex + 1U) % kStateBufferCount;
    m_updateSwapPending = false;
  }

  m_phase = WorldPhase::TransformPropagation;
  if (!propagate_world_transforms()) {
    core::log_message(
        core::LogLevel::Warning, "runtime",
        "transform cycle detected; using deterministic root fallback");
  }
}

void World::begin_render_prep_phase() noexcept {
  if ((m_phase == WorldPhase::Simulation) || (m_phase == WorldPhase::Input)) {
    begin_transform_phase();
  }

  if (m_phase != WorldPhase::TransformPropagation) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_render_prep_phase requires transform propagation");
    return;
  }

  m_phase = WorldPhase::RenderSubmission;
}

void World::begin_render_phase() noexcept {
  if (m_phase != WorldPhase::RenderSubmission) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_render_phase requires RenderSubmission phase");
    return;
  }

  m_phase = WorldPhase::Render;
}

void World::end_frame_phase() noexcept {
  if ((m_phase != WorldPhase::Render) &&
      (m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::TransformPropagation) &&
      (m_phase != WorldPhase::Simulation) && (m_phase != WorldPhase::Input)) {
    core::log_message(core::LogLevel::Error, "world",
                      "end_frame_phase called from invalid phase");
    return;
  }

  m_phase = WorldPhase::Input;
}

WorldPhase World::current_phase() const noexcept { return m_phase; }

void World::for_each_transform(TransformVisitor visitor,
                               void *userData) const noexcept {
  if (visitor == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < m_transforms.count(); ++i) {
    visitor(m_transforms.entity_at(i),
            m_transforms.component_at(i, m_readStateIndex), userData);
  }
}

bool World::update_transforms(float deltaSeconds) noexcept {
  return update_transforms_range(0U, m_transforms.count(), deltaSeconds);
}

bool World::update_transforms_range(std::size_t startIndex, std::size_t count,
                                    float deltaSeconds) noexcept {
  static_cast<void>(deltaSeconds);
  if (m_phase != WorldPhase::Simulation) {
    return false;
  }

  const std::size_t transformCount = m_transforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  if (count == 0U) {
    return true;
  }

  const std::size_t remaining = transformCount - startIndex;
  const std::size_t clampedCount = (count > remaining) ? remaining : count;

  const Entity *entities = nullptr;
  const Transform *readState = nullptr;
  Transform *writeState = nullptr;
  if (!get_transform_update_range(startIndex, clampedCount, &entities,
                                  &readState, &writeState)) {
    return false;
  }

  for (std::size_t i = 0U; i < clampedCount; ++i) {
    writeState[i] = readState[i];
  }

  return true;
}

bool World::get_transform_update_range(
    std::size_t startIndex, std::size_t count, const Entity **outEntities,
    const Transform **outReadTransforms,
    Transform **outWriteTransforms) noexcept {
  if ((outEntities == nullptr) || (outReadTransforms == nullptr) ||
      (outWriteTransforms == nullptr)) {
    return false;
  }

  if (m_phase != WorldPhase::Simulation) {
    return false;
  }

  const std::size_t transformCount = m_transforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  const std::size_t remaining = transformCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_transforms.entity_data();
  const Transform *readState = m_transforms.component_data(m_readStateIndex);
  Transform *writeState = m_transforms.component_data(m_writeStateIndex);
  if ((entities == nullptr) || (readState == nullptr) ||
      (writeState == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outReadTransforms = readState + startIndex;
  *outWriteTransforms = writeState + startIndex;
  return true;
}

bool World::read_transform_range(
    std::size_t startIndex, std::size_t count, const Entity **outEntities,
    const Transform **outTransforms) const noexcept {
  if ((outEntities == nullptr) || (outTransforms == nullptr)) {
    return false;
  }

  if ((m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::Render)) {
    return false;
  }

  const std::size_t transformCount = m_transforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  const std::size_t remaining = transformCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_transforms.entity_data();
  const Transform *transforms = m_transforms.component_data(m_readStateIndex);
  if ((entities == nullptr) || (transforms == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outTransforms = transforms + startIndex;
  return true;
}

bool World::read_world_transform_range(
    std::size_t startIndex, std::size_t count, const Entity **outEntities,
    const WorldTransform **outTransforms) const noexcept {
  if ((outEntities == nullptr) || (outTransforms == nullptr)) {
    return false;
  }

  if ((m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::Render)) {
    return false;
  }

  const std::size_t transformCount = m_worldTransforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  const std::size_t remaining = transformCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_worldTransforms.entity_data();
  const WorldTransform *transforms = m_worldTransforms.component_data();
  if ((entities == nullptr) || (transforms == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outTransforms = transforms + startIndex;
  return true;
}

std::size_t World::transform_count() const noexcept {
  return m_transforms.count();
}

std::size_t World::world_transform_count() const noexcept {
  return m_worldTransforms.count();
}

std::size_t World::rigid_body_count() const noexcept {
  return m_rigidBodies.count();
}

std::size_t World::collider_count() const noexcept {
  return m_colliders.count();
}

World::PhysicsContext &World::physics_context() noexcept {
  return m_physicsContext;
}

const World::PhysicsContext &World::physics_context() const noexcept {
  return m_physicsContext;
}

RigidBody *World::get_rigid_body_ptr(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_rigidBodies.get_ptr(entity);
}

const RigidBody *World::get_rigid_body_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_rigidBodies.get_ptr(entity);
}

const Collider *World::get_collider_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_colliders.get_ptr(entity);
}

Collider *World::get_collider_ptr(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_colliders.get_ptr(entity);
}

std::size_t World::query_state_index() const noexcept {
  return m_readStateIndex;
}

bool World::is_mutation_phase() const noexcept {
  return (m_phase == WorldPhase::Input);
}

bool World::is_valid_entity(Entity entity) const noexcept {
  if ((entity.index == 0U) ||
      (entity.index > static_cast<std::uint32_t>(kMaxEntities))) {
    return false;
  }

  if (!m_entityAlive[entity.index]) {
    return false;
  }

  return m_entityGenerations[entity.index] == entity.generation;
}

bool World::insert_persistent_index(PersistentId persistentId,
                                    std::uint32_t entityIndex) noexcept {
  if ((persistentId == kInvalidPersistentId) || (entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxEntities))) {
    return false;
  }

  const std::size_t capacity = m_persistentIndexState.size();
  if (capacity == 0U) {
    return false;
  }

  const std::size_t base =
      (static_cast<std::size_t>(persistentId) * 2654435761U) % capacity;
  std::size_t tombstone = capacity;

  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    const std::uint8_t state = m_persistentIndexState[slot];

    if (state == 1U) {
      if (m_persistentIndexKeys[slot] == persistentId) {
        m_persistentIndexValues[slot] = entityIndex;
        return true;
      }
      continue;
    }

    if (state == 2U) {
      if (tombstone == capacity) {
        tombstone = slot;
      }
      continue;
    }

    const std::size_t target = (tombstone != capacity) ? tombstone : slot;
    m_persistentIndexState[target] = 1U;
    m_persistentIndexKeys[target] = persistentId;
    m_persistentIndexValues[target] = entityIndex;
    return true;
  }

  if (tombstone != capacity) {
    m_persistentIndexState[tombstone] = 1U;
    m_persistentIndexKeys[tombstone] = persistentId;
    m_persistentIndexValues[tombstone] = entityIndex;
    return true;
  }

  return false;
}

std::uint32_t
World::find_persistent_index(PersistentId persistentId) const noexcept {
  if (persistentId == kInvalidPersistentId) {
    return 0U;
  }

  const std::size_t capacity = m_persistentIndexState.size();
  if (capacity == 0U) {
    return 0U;
  }

  const std::size_t base =
      (static_cast<std::size_t>(persistentId) * 2654435761U) % capacity;
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    const std::uint8_t state = m_persistentIndexState[slot];
    if (state == 0U) {
      return 0U;
    }

    if ((state == 1U) && (m_persistentIndexKeys[slot] == persistentId)) {
      return m_persistentIndexValues[slot];
    }
  }

  return 0U;
}

void World::erase_persistent_index(PersistentId persistentId) noexcept {
  if (persistentId == kInvalidPersistentId) {
    return;
  }

  const std::size_t capacity = m_persistentIndexState.size();
  if (capacity == 0U) {
    return;
  }

  const std::size_t base =
      (static_cast<std::size_t>(persistentId) * 2654435761U) % capacity;
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    const std::uint8_t state = m_persistentIndexState[slot];
    if (state == 0U) {
      return;
    }

    if ((state == 1U) && (m_persistentIndexKeys[slot] == persistentId)) {
      m_persistentIndexState[slot] = 2U;
      m_persistentIndexKeys[slot] = kInvalidPersistentId;
      m_persistentIndexValues[slot] = 0U;
      return;
    }
  }
}

void World::reset_transform_cache(std::uint32_t entityIndex) noexcept {
  if ((entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxEntities))) {
    return;
  }

  m_transformNodes[entityIndex] = TransformNode{};
}

bool World::propagate_world_transforms() noexcept {
  const std::size_t previousActiveCount = m_transformActiveCount;
  for (std::size_t i = 0U; i < previousActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    m_transformQueueIndices[i] = index;

    TransformNode &node = m_transformNodes[index];
    node.parentIndex = 0U;
    node.firstChild = 0U;
    node.lastChild = 0U;
    node.nextSibling = 0U;
    node.traversalState = 0U;
    node.present = false;
    node.localDirty = false;
  }

  m_transformActiveCount = 0U;

  const std::size_t transformCount = m_transforms.count();
  for (std::size_t denseIndex = 0U; denseIndex < transformCount; ++denseIndex) {
    const Entity entity = m_transforms.entity_at(denseIndex);
    if (!is_valid_entity(entity)) {
      continue;
    }

    if (m_transformActiveCount >= m_transformActiveIndices.size()) {
      return false;
    }

    const std::uint32_t index = entity.index;
    m_transformActiveIndices[m_transformActiveCount] = index;
    ++m_transformActiveCount;

    TransformNode &node = m_transformNodes[index];
    node.parentIndex = 0U;
    node.firstChild = 0U;
    node.lastChild = 0U;
    node.nextSibling = 0U;
    node.traversalState = 0U;
    node.present = true;
    node.localDirty = false;

    const Transform &local =
        m_transforms.component_at(denseIndex, m_readStateIndex);

    std::uint32_t parentIndex = 0U;
    if (local.parentId != kInvalidPersistentId) {
      const std::uint32_t resolvedParentIndex =
          find_persistent_index(local.parentId);
      if ((resolvedParentIndex != 0U) && (resolvedParentIndex != index) &&
          m_entityAlive[resolvedParentIndex]) {
        const Entity parentEntity{resolvedParentIndex,
                                  m_entityGenerations[resolvedParentIndex]};
        if (m_transforms.get_ptr(parentEntity, m_readStateIndex) != nullptr) {
          parentIndex = resolvedParentIndex;
        }
      }
    }

    node.parentIndex = parentIndex;

    const bool cacheValid = node.cacheValid;
    const bool localChanged =
        !cacheValid || (local.position.x != node.position.x) ||
        (local.position.y != node.position.y) ||
        (local.position.z != node.position.z) ||
        (local.rotation.x != node.rotation.x) ||
        (local.rotation.y != node.rotation.y) ||
        (local.rotation.z != node.rotation.z) ||
        (local.rotation.w != node.rotation.w) ||
        (local.scale.x != node.scale.x) || (local.scale.y != node.scale.y) ||
        (local.scale.z != node.scale.z);
    const bool parentChanged = !cacheValid ||
                               (node.cachedParentId != local.parentId) ||
                               (node.cachedParentIndex != parentIndex);
    node.localDirty = localChanged || parentChanged;

    node.position = local.position;
    node.rotation = local.rotation;
    node.scale = local.scale;
    node.cachedParentId = local.parentId;
    node.cachedParentIndex = parentIndex;
    node.cacheValid = true;
  }

  // Invalidate cache only for entities that had transforms previously but no
  // longer do, instead of scanning the full entity ID range.
  for (std::size_t i = 0U; i < previousActiveCount; ++i) {
    const std::uint32_t index = m_transformQueueIndices[i];
    if (!m_transformNodes[index].present) {
      reset_transform_cache(index);
    }
  }

  std::size_t rootCount = 0U;
  for (std::size_t i = 0U; i < m_transformActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    if (!m_transformNodes[index].present) {
      continue;
    }

    const std::uint32_t parentIndex = m_transformNodes[index].parentIndex;
    if (parentIndex == 0U) {
      if (rootCount >= m_transformRoots.size()) {
        return false;
      }
      m_transformRoots[rootCount] = index;
      ++rootCount;
      continue;
    }

    if (m_transformNodes[parentIndex].firstChild == 0U) {
      m_transformNodes[parentIndex].firstChild = index;
      m_transformNodes[parentIndex].lastChild = index;
      continue;
    }

    const std::uint32_t lastChild = m_transformNodes[parentIndex].lastChild;
    m_transformNodes[lastChild].nextSibling = index;
    m_transformNodes[parentIndex].lastChild = index;
  }

  auto enqueue_node = [this](std::uint32_t entityIndex, bool inheritedDirty,
                             std::size_t *ioQueueTail) noexcept {
    if ((entityIndex == 0U) || (ioQueueTail == nullptr) ||
        (*ioQueueTail >= m_transformQueueIndices.size())) {
      return false;
    }

    if (m_transformNodes[entityIndex].traversalState != 0U) {
      return true;
    }

    m_transformQueueIndices[*ioQueueTail] = entityIndex;
    m_transformQueueInheritedDirty[*ioQueueTail] = inheritedDirty;
    m_transformNodes[entityIndex].traversalState = 1U;
    ++(*ioQueueTail);
    return true;
  };

  std::size_t queueHead = 0U;
  std::size_t queueTail = 0U;
  for (std::size_t i = 0U; i < rootCount; ++i) {
    if (!enqueue_node(m_transformRoots[i], false, &queueTail)) {
      return false;
    }
  }

  auto drain_queue = [this, &queueHead, &queueTail, &enqueue_node]() noexcept {
    while (queueHead < queueTail) {
      const std::uint32_t entityIndex = m_transformQueueIndices[queueHead];
      const bool inheritedDirty = m_transformQueueInheritedDirty[queueHead];
      ++queueHead;

      m_transformNodes[entityIndex].traversalState = 2U;

      const Entity entity{entityIndex, m_entityGenerations[entityIndex]};
      const Transform *local = m_transforms.get_ptr(entity, m_readStateIndex);
      if (local == nullptr) {
        continue;
      }

      const bool hasWorldTransform = m_worldTransforms.contains(entity);
      const bool worldDirty = inheritedDirty ||
                              m_transformNodes[entityIndex].localDirty ||
                              !hasWorldTransform;
      if (worldDirty) {
        WorldTransform world = world_transform_from_local(*local);
        const std::uint32_t parentIndex =
            m_transformNodes[entityIndex].parentIndex;
        if (parentIndex != 0U) {
          const Entity parent{parentIndex, m_entityGenerations[parentIndex]};
          const WorldTransform *parentWorld = m_worldTransforms.get_ptr(parent);
          if (parentWorld != nullptr) {
            const math::Mat4 localMatrix = math::compose_trs(
                local->position, local->rotation, local->scale);
            world.matrix = math::mul(parentWorld->matrix, localMatrix);
            world.position =
                math::Vec3(world.matrix.columns[3].x, world.matrix.columns[3].y,
                           world.matrix.columns[3].z);
            world.rotation = math::normalize(
                math::mul(parentWorld->rotation, local->rotation));
            world.scale = math::Vec3(parentWorld->scale.x * local->scale.x,
                                     parentWorld->scale.y * local->scale.y,
                                     parentWorld->scale.z * local->scale.z);
          }
        }

        static_cast<void>(m_worldTransforms.add(entity, world));
      }

      for (std::uint32_t child = m_transformNodes[entityIndex].firstChild;
           child != 0U; child = m_transformNodes[child].nextSibling) {
        if (!enqueue_node(child, worldDirty, &queueTail)) {
          return false;
        }
      }
    }

    return true;
  };

  if (!drain_queue()) {
    return false;
  }

  bool fullyAcyclic = true;
  for (std::size_t i = 0U; i < m_transformActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    if (!m_transformNodes[index].present ||
        (m_transformNodes[index].traversalState != 0U)) {
      continue;
    }

    fullyAcyclic = false;
    m_transformNodes[index].parentIndex = 0U;
    if (!enqueue_node(index, false, &queueTail)) {
      return false;
    }
  }

  if (!drain_queue()) {
    return false;
  }

  return fullyAcyclic;
}

} // namespace engine::runtime
