// Implements world behavior for the Engine runtime world.

#include "engine/runtime/world.h"

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/transform.h"
#include "engine/runtime/reflect_types.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "world_internal.h"

namespace engine::runtime {

World::SimulationAccessToken World::simulation_access_token() const noexcept {
  return make_token(m_phase == WorldPhase::Simulation);
}

Transform *
World::get_transform_write_ptr(Entity entity,
                               const SimulationAccessToken &token) noexcept {
  if (!token.valid() || !is_valid_entity(entity) ||
      (m_phase != WorldPhase::Simulation)) {
    return nullptr;
  }

  return m_transforms.get_ptr(entity, m_writeStateIndex);
}

bool World::get_simulation_physics_transform(
    Entity entity, const SimulationAccessToken &token,
    physics::PhysicsTransform *outTransform) const noexcept {
  if (!token.valid() || (m_phase != WorldPhase::Simulation)) {
    return false;
  }
  return build_physics_transform(entity, m_writeStateIndex, outTransform);
}

const WorldTransform *
World::get_world_transform_read_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_worldTransforms, entity);
}

void World::begin_update_phase() noexcept {
  if (m_phase != WorldPhase::Input) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_phase requires Input phase");
    return;
  }

  snapshot_world_transform_history();
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
  snapshot_world_transform_history();
  m_updateSwapPending = true;
}

void World::snapshot_world_transform_history() noexcept {
  ++m_worldTransformHistoryEpoch;
  const std::size_t count = m_worldTransforms.count();
  const Entity *entities = m_worldTransforms.entity_data();
  const WorldTransform *transforms = m_worldTransforms.component_data();
  if ((entities == nullptr) || (transforms == nullptr)) {
    return;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    const std::uint32_t index = entities[i].index;
    WorldTransformHistoryEntry &entry = m_worldTransformHistory[index];
    entry.position = transforms[i].position;
    entry.rotation = transforms[i].rotation;
    entry.scale = transforms[i].scale;
    entry.generation = entities[i].generation;
    entry.epoch = m_worldTransformHistoryEpoch;
  }
}

bool World::get_previous_world_trs(Entity entity, math::Vec3 *outPosition,
                                   math::Quat *outRotation,
                                   math::Vec3 *outScale) const noexcept {
  if ((outPosition == nullptr) || (outRotation == nullptr) ||
      (outScale == nullptr) || (entity.index == 0U) ||
      (entity.index > kMaxEntities)) {
    return false;
  }
  const WorldTransformHistoryEntry &entry =
      m_worldTransformHistory[entity.index];
  if ((entry.epoch != m_worldTransformHistoryEpoch) ||
      (entry.generation != entity.generation)) {
    return false;
  }
  *outPosition = entry.position;
  *outRotation = entry.rotation;
  *outScale = entry.scale;
  return true;
}

void World::clear_world_transform_history() noexcept {
  ++m_worldTransformHistoryEpoch;
}

void World::commit_update_phase() noexcept {
  if ((m_phase != WorldPhase::Simulation) || !m_updateSwapPending) {
    core::log_message(core::LogLevel::Error, "world",
                      "commit_update_phase requires active Simulation phase");
    return;
  }

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

void World::begin_begin_play_phase() noexcept {
  if (m_phase != WorldPhase::Input) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_begin_play_phase requires Input phase");
    return;
  }
  m_phase = WorldPhase::BeginPlay;
}

void World::end_begin_play_phase() noexcept {
  if (m_phase != WorldPhase::BeginPlay) {
    core::log_message(core::LogLevel::Error, "world",
                      "end_begin_play_phase requires BeginPlay phase");
    return;
  }
  m_phase = WorldPhase::Input;
}

void World::mark_begin_play_done(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return;
  }
  if (!m_entityBeginPlayFired[entity.index] && (m_beginPlayPendingCount > 0U)) {
    --m_beginPlayPendingCount;
  }
  m_entityBeginPlayFired[entity.index] = true;
}

void World::begin_end_play_phase() noexcept {
  if ((m_phase != WorldPhase::Render) &&
      (m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::TransformPropagation) &&
      (m_phase != WorldPhase::Simulation) && (m_phase != WorldPhase::Input)) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_end_play_phase called from invalid phase");
    return;
  }
  m_phase = WorldPhase::EndPlay;
}

void World::end_end_play_phase() noexcept {
  if (m_phase != WorldPhase::EndPlay) {
    core::log_message(core::LogLevel::Error, "world",
                      "end_end_play_phase requires EndPlay phase");
    return;
  }
  flush_deferred_destroys();
  m_phase = WorldPhase::Input;
}

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

  // Same contract as the other range APIs: an out-of-bounds range is an
  // error, not a request to clamp.
  const Entity *entities = nullptr;
  const Transform *readState = nullptr;
  Transform *writeState = nullptr;
  if (!get_transform_update_range(startIndex, count, &entities, &readState,
                                  &writeState)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
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

physics::PhysicsContext &World::physics_context() noexcept {
  return m_physicsContext;
}

const physics::PhysicsContext &World::physics_context() const noexcept {
  return m_physicsContext;
}

RigidBody *World::get_rigid_body_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_rigidBodies, entity);
}

const RigidBody *World::get_rigid_body_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_rigidBodies, entity);
}

const Collider *World::get_collider_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_colliders, entity);
}

Collider *World::get_collider_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_colliders, entity);
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

bool World::build_physics_transform(
    Entity entity, std::size_t stateIndex,
    physics::PhysicsTransform *outTransform) const noexcept {
  if ((outTransform == nullptr) || (stateIndex >= kStateBufferCount) ||
      !is_valid_entity(entity)) {
    return false;
  }

  const auto parent_of = [this, stateIndex](Entity child) noexcept -> Entity {
    const Transform *local = m_transforms.get_ptr(child, stateIndex);
    if ((local == nullptr) || (local->parentId == kInvalidPersistentId)) {
      return kInvalidEntity;
    }

    const std::uint32_t parentIndex = find_persistent_index(local->parentId);
    if ((parentIndex == 0U) || (parentIndex == child.index) ||
        !m_entityAlive[parentIndex]) {
      return kInvalidEntity;
    }

    const Entity parent{parentIndex, m_entityGenerations[parentIndex]};
    return (m_transforms.get_ptr(parent, stateIndex) != nullptr)
               ? parent
               : kInvalidEntity;
  };

  // Reject cycles before composing so a malformed imported hierarchy cannot
  // turn one collider lookup into a full-capacity walk.
  Entity slow = entity;
  Entity fast = entity;
  for (std::size_t depth = 0U; depth < kMaxEntities; ++depth) {
    slow = parent_of(slow);
    fast = parent_of(fast);
    if (fast != kInvalidEntity) {
      fast = parent_of(fast);
    }
    if ((slow == kInvalidEntity) || (fast == kInvalidEntity)) {
      break;
    }
    if (slow == fast) {
      return false;
    }
  }

  math::Mat4 matrix = math::identity();
  math::Quat rotation{};
  math::Vec3 scale(1.0F, 1.0F, 1.0F);
  Entity current = entity;
  for (std::size_t depth = 0U; depth < kMaxEntities; ++depth) {
    const Transform *local = m_transforms.get_ptr(current, stateIndex);
    if (local == nullptr) {
      return false;
    }

    matrix = math::mul(
        math::compose_trs(local->position, local->rotation, local->scale),
        matrix);
    rotation = math::normalize(math::mul(local->rotation, rotation));
    scale = math::Vec3(local->scale.x * scale.x, local->scale.y * scale.y,
                       local->scale.z * scale.z);

    const Entity parent = parent_of(current);
    if (parent == kInvalidEntity) {
      physics::PhysicsTransform result{};
      result.position = math::Vec3(matrix.columns[3].x, matrix.columns[3].y,
                                   matrix.columns[3].z);
      result.rotation = rotation;
      result.scale = scale;
      result.matrix = matrix;
      *outTransform = result;
      return true;
    }
    current = parent;
  }

  return false;
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