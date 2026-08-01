// Implements World entity lifecycle: creation, hierarchy cascade
// destruction (immediate and deferred), aliveness queries, and the
// name-lookup and persistent-id indexes.

#include "engine/runtime/world.h"

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/transform.h"
#include "engine/runtime/reflect_types.h"
#include "world_internal.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace engine::runtime {

Entity World::create_entity() noexcept {
  return create_entity_with_persistent_id(kInvalidPersistentId);
}

Entity World::create_scene_object(const Transform &localTransform) noexcept {
  return create_scene_object_with_persistent_id(kInvalidPersistentId,
                                                localTransform);
}

Entity World::create_scene_object_with_persistent_id(
    PersistentId persistentId, const Transform &localTransform) noexcept {
  const Entity entity = create_entity_with_persistent_id(persistentId);
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }

  if (add_transform(entity, localTransform)) {
    return entity;
  }

  static_cast<void>(destroy_entity_immediate(entity));
  core::log_message(core::LogLevel::Error, "world",
                    "create_scene_object failed to add Transform");
  return kInvalidEntity;
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
  m_entityBeginPlayFired[index] = false;
  ++m_beginPlayPendingCount;
  return Entity{index, m_entityGenerations[index]};
}

std::size_t World::mark_hierarchy_descendants(Entity root) noexcept {
  m_cascadeMarks.fill(false);
  m_cascadeMarks[root.index] = true;

  // Fixpoint marking: a transform whose resolved parent index is marked
  // joins the subtree. Pass count is bounded by hierarchy depth, and cycles
  // terminate because the mark set only grows.
  std::size_t markedCount = 0U;
  bool changed = true;
  while (changed) {
    changed = false;
    const std::size_t transformCount = m_transforms.count();
    for (std::size_t denseIndex = 0U; denseIndex < transformCount;
         ++denseIndex) {
      const Entity entity = m_transforms.entity_at(denseIndex);
      if (m_cascadeMarks[entity.index] || !m_entityAlive[entity.index]) {
        continue;
      }

      const Transform &local =
          m_transforms.component_at(denseIndex, m_readStateIndex);
      if (local.parentId == kInvalidPersistentId) {
        continue;
      }

      const std::uint32_t parentIndex = find_persistent_index(local.parentId);
      if ((parentIndex != 0U) && m_cascadeMarks[parentIndex]) {
        m_cascadeMarks[entity.index] = true;
        ++markedCount;
        changed = true;
      }
    }
  }

  return markedCount;
}

bool World::destroy_entity_immediate(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  // Children never survive their parent: destroy the whole subtree so no
  // orphan snaps to its local offset.
  if (mark_hierarchy_descendants(entity) > 0U) {
    const std::uint32_t upperBound = m_nextEntityIndex;
    for (std::uint32_t index = 1U; index < upperBound; ++index) {
      if (!m_cascadeMarks[index] || (index == entity.index) ||
          !m_entityAlive[index]) {
        continue;
      }
      static_cast<void>(
          destroy_single_entity(Entity{index, m_entityGenerations[index]}));
    }
  }

  return destroy_single_entity(entity);
}

void World::remove_all_components(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return;
  }

  NameComponent removedName{};
  const bool hadName = m_nameComponents.get(entity, &removedName);

  m_cameraManager.on_entity_destroyed(entity);

  physics::remove_shape_payloads(m_physicsContext, entity);
  static_cast<void>(m_transforms.remove(entity));
  static_cast<void>(m_worldTransforms.remove(entity));
  static_cast<void>(m_rigidBodies.remove(entity));
  static_cast<void>(m_colliders.remove(entity));
  static_cast<void>(m_meshComponents.remove(entity));
  static_cast<void>(m_nameComponents.remove(entity));
  static_cast<void>(m_lightComponents.remove(entity));
  static_cast<void>(m_pointLights.remove(entity));
  static_cast<void>(m_spotLights.remove(entity));
  static_cast<void>(m_reflectionProbes.remove(entity));
  static_cast<void>(m_sceneCaptures.remove(entity));
  static_cast<void>(m_foliagePatches.remove(entity));
  static_cast<void>(m_springArms.remove(entity));
  static_cast<void>(m_scriptComponents.remove(entity));
  static_cast<void>(m_animationComponents.remove(entity));

  m_movementAuthorities[entity.index] = MovementAuthority::None;
  reset_transform_cache(entity.index);
  if (hadName && (removedName.name[0] != '\0')) {
    name_lookup_erase(core::fnv1a_32(removedName.name), entity.index);
  }
}

bool World::destroy_single_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  remove_all_components(entity);

  const std::uint32_t index = entity.index;
  erase_persistent_index(m_entityPersistentIds[index]);
  m_entityAlive[index] = false;
  if (!m_entityBeginPlayFired[index] && (m_beginPlayPendingCount > 0U)) {
    --m_beginPlayPendingCount;
  }
  m_entityBeginPlayFired[index] = false;
  m_entityPersistentIds[index] = kInvalidPersistentId;
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
  // Children join the deferred queue too, so their EndPlay callbacks fire
  // before the flush removes the subtree.
  if (mark_hierarchy_descendants(entity) > 0U) {
    const std::uint32_t upperBound = m_nextEntityIndex;
    for (std::uint32_t index = 1U; index < upperBound; ++index) {
      if (!m_cascadeMarks[index] || (index == entity.index) ||
          !m_entityAlive[index]) {
        continue;
      }
      static_cast<void>(queue_single_deferred_destroy(
          Entity{index, m_entityGenerations[index]}));
    }
  }

  return queue_single_deferred_destroy(entity);
}

bool World::queue_single_deferred_destroy(Entity entity) noexcept {
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

bool World::recycle_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }
  if ((m_phase != WorldPhase::Input) && (m_phase != WorldPhase::BeginPlay) &&
      (m_phase != WorldPhase::EndPlay)) {
    core::log_message(core::LogLevel::Warning, "world",
                      "recycle_entity refused outside a mutation phase");
    return false;
  }
  // A queued deferred destroy would fire after the slot re-publishes and
  // tear down whoever acquires the recycled entity next.
  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    if (m_pendingDestroyEntities[i] == entity) {
      core::log_message(core::LogLevel::Warning, "world",
                        "recycle_entity refused: destroy already queued");
      return false;
    }
  }

  // Children never survive their parent's teardown; the pool owns only
  // the root, so descendants are fully destroyed, not recycled.
  if (mark_hierarchy_descendants(entity) > 0U) {
    const std::uint32_t upperBound = m_nextEntityIndex;
    for (std::uint32_t index = 1U; index < upperBound; ++index) {
      if (m_cascadeMarks[index] && (index != entity.index) &&
          m_entityAlive[index]) {
        static_cast<void>(destroy_single_entity(
            Entity{index, m_entityGenerations[index]}));
      }
    }
  }

  remove_all_components(entity);
  return true;
}

bool World::activate_recycled_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }
  if ((m_phase != WorldPhase::Input) && (m_phase != WorldPhase::BeginPlay) &&
      (m_phase != WorldPhase::EndPlay)) {
    core::log_message(core::LogLevel::Warning, "world",
                      "activate_recycled_entity refused outside a mutation "
                      "phase");
    return false;
  }
  // Dormant pool entities keep their fired flag set so the per-frame
  // BeginPlay dispatch skips them; activation re-arms it exactly once so
  // components attached after acquisition get fresh-entity callbacks.
  if (m_entityBeginPlayFired[entity.index]) {
    m_entityBeginPlayFired[entity.index] = false;
    ++m_beginPlayPendingCount;
  }
  return true;
}

bool World::destroy_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "destroy_entity requires a live entity");
    return false;
  }

  // During Simulation, defer so EndPlay callbacks fire before removal.
  if (m_phase == WorldPhase::Simulation) {
    return queue_deferred_destroy(entity);
  }

  // Immediate destruction is allowed during Input, BeginPlay, and EndPlay.
  if (m_phase != WorldPhase::Input && m_phase != WorldPhase::BeginPlay &&
      m_phase != WorldPhase::EndPlay) {
    return false;
  }

  return destroy_entity_immediate(entity);
}

bool World::is_alive(Entity entity) const noexcept {
  return is_valid_entity(entity);
}

std::uint32_t World::content_epoch() const noexcept { return m_contentEpoch; }

void World::mark_content_replaced(std::uint32_t previousEpoch) noexcept {
  m_contentEpoch = previousEpoch + 1U;
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


bool World::name_lookup_insert(std::uint32_t nameHash,
                               std::uint32_t entityIndex) noexcept {
  if (entityIndex == 0U) {
    return false;
  }

  std::size_t slot = static_cast<std::size_t>(nameHash) %
                     static_cast<std::size_t>(kNameLookupCapacity);
  std::size_t tombstone = kNameLookupCapacity;
  for (std::size_t probe = 0U; probe < kNameLookupCapacity; ++probe) {
    if (m_nameLookupState[slot] == kNameSlotEmpty) {
      const std::size_t writeSlot =
          (tombstone != kNameLookupCapacity) ? tombstone : slot;
      if (writeSlot == tombstone) {
        --m_nameLookupTombstones;
      }
      m_nameLookupState[writeSlot] = kNameSlotOccupied;
      m_nameLookupHashes[writeSlot] = nameHash;
      m_nameLookupEntityIndices[writeSlot] = entityIndex;
      return true;
    }

    if (m_nameLookupState[slot] == kNameSlotTombstone) {
      if (tombstone == kNameLookupCapacity) {
        tombstone = slot;
      }
    } else if ((m_nameLookupHashes[slot] == nameHash) &&
               (m_nameLookupEntityIndices[slot] == entityIndex)) {
      // Same entity re-registered under the same hash; keep its slot.
      return true;
    }
    // Entities sharing a name each keep their own slot so erasing one does
    // not orphan the others; lookups skip entries whose entity died.

    slot = (slot + 1U) % kNameLookupCapacity;
  }

  return false;
}

void World::name_lookup_erase(std::uint32_t nameHash,
                              std::uint32_t entityIndex) noexcept {
  std::size_t slot = static_cast<std::size_t>(nameHash) %
                     static_cast<std::size_t>(kNameLookupCapacity);
  for (std::size_t probe = 0U; probe < kNameLookupCapacity; ++probe) {
    if (m_nameLookupState[slot] == kNameSlotEmpty) {
      return;
    }

    if ((m_nameLookupState[slot] == kNameSlotOccupied) &&
        (m_nameLookupHashes[slot] == nameHash) &&
        (m_nameLookupEntityIndices[slot] == entityIndex)) {
      m_nameLookupState[slot] = kNameSlotTombstone;
      m_nameLookupHashes[slot] = 0U;
      m_nameLookupEntityIndices[slot] = 0U;
      ++m_nameLookupTombstones;

      // Rebuild once tombstones dominate so misses stay cheap: a probe stops
      // at the first empty slot, and churn erodes empty slots over time.
      if (m_nameLookupTombstones > (kNameLookupCapacity / 4U)) {
        rebuild_name_lookup();
      }
      return;
    }

    slot = (slot + 1U) % kNameLookupCapacity;
  }
}

void World::rebuild_name_lookup() noexcept {
  m_nameLookupHashes.fill(0U);
  m_nameLookupEntityIndices.fill(0U);
  m_nameLookupState.fill(kNameSlotEmpty);
  m_nameLookupTombstones = 0U;

  const std::size_t count = m_nameComponents.count();
  for (std::size_t i = 0U; i < count; ++i) {
    const Entity entity = m_nameComponents.entity_at(i);
    if (!is_valid_entity(entity)) {
      continue;
    }

    const NameComponent &nameComponent = m_nameComponents.component_at(i);
    if (nameComponent.name[0] == '\0') {
      continue;
    }

    const std::uint32_t hash = core::fnv1a_32(nameComponent.name);
    if (!name_lookup_insert(hash, entity.index)) {
      core::log_message(
          core::LogLevel::Warning, "world",
          "name lookup table overflow; name lookup may miss entries");
      return;
    }
  }
}

bool World::insert_persistent_index(PersistentId persistentId,
                                    std::uint32_t entityIndex) noexcept {
  if ((persistentId == kInvalidPersistentId) || (entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxEntities))) {
    return false;
  }

  return m_persistentIndex.insert(persistentId, entityIndex);
}

std::uint32_t
World::find_persistent_index(PersistentId persistentId) const noexcept {
  if (persistentId == kInvalidPersistentId) {
    return 0U;
  }

  const std::uint32_t *entityIndex = m_persistentIndex.find(persistentId);
  return (entityIndex != nullptr) ? *entityIndex : 0U;
}

void World::erase_persistent_index(PersistentId persistentId) noexcept {
  if (persistentId == kInvalidPersistentId) {
    return;
  }

  static_cast<void>(m_persistentIndex.erase(persistentId));

  // Entity churn accumulates tombstones; rebuild from the alive arrays once
  // they dominate so lookup misses stay cheap.
  if (m_persistentIndex.tombstone_count() > (kPersistentIndexCapacity / 4U)) {
    rebuild_persistent_index();
  }
}

void World::rebuild_persistent_index() noexcept {
  m_persistentIndex.clear();

  std::size_t visited = 0U;
  for (std::uint32_t index = 1U;
       (index < m_nextEntityIndex) && (visited < m_aliveEntityCount); ++index) {
    if (!m_entityAlive[index]) {
      continue;
    }
    ++visited;
    if (m_entityPersistentIds[index] != kInvalidPersistentId) {
      static_cast<void>(
          m_persistentIndex.insert(m_entityPersistentIds[index], index));
    }
  }
}


} // namespace engine::runtime
