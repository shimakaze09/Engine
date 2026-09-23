// Implements World entity lifecycle: creation, hierarchy cascade
// destruction (immediate and deferred), aliveness queries, and the
// name-lookup and persistent-id indexes.

#include "engine/runtime/world.h"

#include <algorithm>

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
    note_refusal(core::FailureKind::InvariantViolated);
    return kInvalidEntity;
  }

  if ((persistentId != kInvalidPersistentId) &&
      (find_persistent_index(persistentId) != 0U)) {
    core::log_message(core::LogLevel::Error, "world",
                      "create_entity refused: persistent id already in use");
    note_refusal(core::FailureKind::InvalidArgument);
    return kInvalidEntity;
  }

  std::uint32_t index = 0U;
  if (m_freeEntityCount > 0U) {
    --m_freeEntityCount;
    index = m_freeEntityIndices[m_freeEntityCount];
  } else {
    if (m_nextEntityIndex > static_cast<std::uint32_t>(kMaxEntities)) {
      core::log_message(core::LogLevel::Error, "world",
                        "create_entity refused: entity capacity is full");
      note_refusal(core::FailureKind::CapacityExhausted);
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
      core::log_message(core::LogLevel::Error, "world",
                        "create_entity refused: persistent ids exhausted");
      note_refusal(core::FailureKind::CapacityExhausted, 1U);
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
    core::log_message(core::LogLevel::Error, "world",
                      "create_entity refused: persistent id table is full");
    note_refusal(core::FailureKind::CapacityExhausted, 2U);
    return kInvalidEntity;
  }
  ++m_aliveEntityCount;
  m_entityBeginPlayFired[index] = false;
  ++m_beginPlayPendingCount;
  return Entity{index, m_entityGenerations[index]};
}

std::size_t World::mark_hierarchy_descendants(Entity root) noexcept {
  clear_cascade_marks();
  ensure_hierarchy_links();
  m_cascadeMarkRoot = root.index;
  m_cascadeMarks[root.index] = true;

  // Depth-first over the child index; the marks make a cycle terminate.
  // The propagation queue array is free here: no propagation pass runs
  // while a mutation-phase operation walks the hierarchy.
  std::size_t stackTop = 0U;
  m_transformQueueIndices[stackTop++] = root.index;
  while (stackTop > 0U) {
    const std::uint32_t current = m_transformQueueIndices[--stackTop];
    for (std::uint32_t child = m_transformNodes[current].firstChild;
         child != 0U; child = m_transformNodes[child].nextSibling) {
      ++m_hierarchyVisits;
      if (m_cascadeMarks[child] || !m_entityAlive[child]) {
        continue;
      }
      m_cascadeMarks[child] = true;
      m_cascadeMarked[m_cascadeMarkedCount++] = child;
      if (stackTop < m_transformQueueIndices.size()) {
        m_transformQueueIndices[stackTop++] = child;
      }
    }
  }

  // Ascending index keeps the order every consumer had before the index
  // existed (deferred EndPlay order, editor delete records).
  std::sort(m_cascadeMarked.data(),
            m_cascadeMarked.data() + m_cascadeMarkedCount);
  return m_cascadeMarkedCount;
}

void World::clear_cascade_marks() noexcept {
  for (std::size_t i = 0U; i < m_cascadeMarkedCount; ++i) {
    m_cascadeMarks[m_cascadeMarked[i]] = false;
  }
  m_cascadeMarks[m_cascadeMarkRoot] = false;
  m_cascadeMarkedCount = 0U;
  m_cascadeMarkRoot = 0U;
}

void World::ensure_hierarchy_links() noexcept {
  if (m_hierarchyLinksStale) {
    rebuild_hierarchy_links();
  }
}

void World::rebuild_hierarchy_links() noexcept {
  for (std::size_t i = 0U; i < m_transformActiveCount; ++i) {
    TransformNode &node = m_transformNodes[m_transformActiveIndices[i]];
    node.parentIndex = 0U;
    node.firstChild = 0U;
    node.lastChild = 0U;
    node.nextSibling = 0U;
    node.prevSibling = 0U;
    node.present = false;
    node.orphan = false;
  }
  m_transformActiveCount = 0U;
  m_hierarchyOrphanCount = 0U;

  const std::size_t transformCount = m_transforms.count();
  for (std::size_t denseIndex = 0U; denseIndex < transformCount; ++denseIndex) {
    const Entity entity = m_transforms.entity_at(denseIndex);
    ++m_hierarchyVisits;
    if (!is_valid_entity(entity) ||
        (m_transformActiveCount >= m_transformActiveIndices.size())) {
      continue;
    }
    m_transformActiveIndices[m_transformActiveCount++] = entity.index;
    TransformNode &node = m_transformNodes[entity.index];
    node.parentIndex = 0U;
    node.firstChild = 0U;
    node.lastChild = 0U;
    node.nextSibling = 0U;
    node.prevSibling = 0U;
    node.present = true;
    node.orphan = false;
  }
  for (std::size_t i = 0U; i < m_transformActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    const Entity entity{index, m_entityGenerations[index]};
    const Transform *local = m_transforms.get_ptr(entity, m_readStateIndex);
    if ((local == nullptr) || (local->parentId == kInvalidPersistentId)) {
      continue;
    }
    const std::uint32_t parentIndex = find_persistent_index(local->parentId);
    TransformNode &node = m_transformNodes[index];
    if ((parentIndex == 0U) || (parentIndex == index) ||
        !m_entityAlive[parentIndex] || !m_transformNodes[parentIndex].present) {
      node.orphan = true;
      ++m_hierarchyOrphanCount;
      continue;
    }
    node.parentIndex = parentIndex;
    TransformNode &parent = m_transformNodes[parentIndex];
    if (parent.firstChild == 0U) {
      parent.firstChild = index;
    } else {
      m_transformNodes[parent.lastChild].nextSibling = index;
      node.prevSibling = parent.lastChild;
    }
    parent.lastChild = index;
  }
  m_hierarchyLinksStale = false;
}

void World::link_transform_node(std::uint32_t index, PersistentId parentId,
                                bool hadTransform) noexcept {
  if (m_hierarchyLinksStale) {
    return;
  }
  TransformNode &node = m_transformNodes[index];
  if (node.present) {
    // Reparent: leave the old parent's list; the children stay attached.
    TransformNode &oldParent = m_transformNodes[node.parentIndex];
    if (node.parentIndex != 0U) {
      if (node.prevSibling != 0U) {
        m_transformNodes[node.prevSibling].nextSibling = node.nextSibling;
      } else if (oldParent.firstChild == index) {
        oldParent.firstChild = node.nextSibling;
      }
      if (node.nextSibling != 0U) {
        m_transformNodes[node.nextSibling].prevSibling = node.prevSibling;
      } else if (oldParent.lastChild == index) {
        oldParent.lastChild = node.prevSibling;
      }
    }
    if (node.orphan && (m_hierarchyOrphanCount > 0U)) {
      --m_hierarchyOrphanCount;
    }
  }
  node.present = true;
  node.orphan = false;
  node.parentIndex = 0U;
  node.nextSibling = 0U;
  node.prevSibling = 0U;

  std::uint32_t parentIndex = 0U;
  if (parentId != kInvalidPersistentId) {
    const std::uint32_t resolved = find_persistent_index(parentId);
    if ((resolved != 0U) && (resolved != index) && m_entityAlive[resolved] &&
        m_transformNodes[resolved].present) {
      parentIndex = resolved;
    } else {
      node.orphan = true;
      ++m_hierarchyOrphanCount;
    }
  }
  if (parentIndex != 0U) {
    node.parentIndex = parentIndex;
    TransformNode &parent = m_transformNodes[parentIndex];
    if (parent.firstChild == 0U) {
      parent.firstChild = index;
    } else {
      m_transformNodes[parent.lastChild].nextSibling = index;
      node.prevSibling = parent.lastChild;
    }
    parent.lastChild = index;
  }
  // A transform that just appeared may be the parent an orphan authored;
  // the next walk rebuilds rather than guess.
  if (!hadTransform && (m_hierarchyOrphanCount > 0U)) {
    m_hierarchyLinksStale = true;
  }
}

void World::unlink_transform_node(std::uint32_t index) noexcept {
  TransformNode &node = m_transformNodes[index];
  if (m_hierarchyLinksStale || !node.present) {
    return;
  }
  if (node.parentIndex != 0U) {
    TransformNode &parent = m_transformNodes[node.parentIndex];
    if (node.prevSibling != 0U) {
      m_transformNodes[node.prevSibling].nextSibling = node.nextSibling;
    } else if (parent.firstChild == index) {
      parent.firstChild = node.nextSibling;
    }
    if (node.nextSibling != 0U) {
      m_transformNodes[node.nextSibling].prevSibling = node.prevSibling;
    } else if (parent.lastChild == index) {
      parent.lastChild = node.prevSibling;
    }
  }
  // Children the current cascade is destroying too are left for their own
  // teardown; any other child loses its parent and waits as an orphan.
  for (std::uint32_t child = node.firstChild; child != 0U;) {
    const std::uint32_t next = m_transformNodes[child].nextSibling;
    ++m_hierarchyVisits;
    if (!m_cascadeMarks[child]) {
      TransformNode &childNode = m_transformNodes[child];
      childNode.parentIndex = 0U;
      childNode.nextSibling = 0U;
      childNode.prevSibling = 0U;
      if (!childNode.orphan) {
        childNode.orphan = true;
        ++m_hierarchyOrphanCount;
      }
    }
    child = next;
  }
  if (node.orphan && (m_hierarchyOrphanCount > 0U)) {
    --m_hierarchyOrphanCount;
  }
  node.parentIndex = 0U;
  node.firstChild = 0U;
  node.lastChild = 0U;
  node.nextSibling = 0U;
  node.prevSibling = 0U;
  node.present = false;
  node.orphan = false;
}

bool World::destroy_entity_immediate(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  // Children never survive their parent: destroy the whole subtree so no
  // orphan snaps to its local offset.
  const std::size_t descendantCount = mark_hierarchy_descendants(entity);
  for (std::size_t i = 0U; i < descendantCount; ++i) {
    const std::uint32_t index = m_cascadeMarked[i];
    if (m_entityAlive[index]) {
      static_cast<void>(
          destroy_single_entity(Entity{index, m_entityGenerations[index]}));
    }
  }

  const bool destroyed = destroy_single_entity(entity);
  clear_cascade_marks();
  return destroyed;
}

void World::remove_all_components(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return;
  }

  NameComponent removedName{};
  const bool hadName = m_nameComponents.get(entity, &removedName);
  // A dying collider leaves the body that owned it; resolved before the
  // hierarchy link goes so the owner is still reachable.
  const Entity inertiaOwner =
      (m_colliders.get_ptr(entity) != nullptr)
          ? find_rigid_body_owner(entity, m_readStateIndex)
          : kInvalidEntity;

  m_cameraManager.on_entity_destroyed(entity);

  physics::remove_shape_payloads(m_physicsContext, entity);
  physics::remove_joints_for_entity(m_physicsContext, entity);
  unlink_transform_node(entity.index);
  // Every set is removed via the storage table so a new component cannot be
  // stranded on a dead slot and inherited by the index's next entity;
  // removal order across sets is immaterial.
  static_cast<void>(m_transforms.remove(entity));
#define ENGINE_WUS_REMOVE(Type, member)                                        \
  static_cast<void>((member).remove(entity));
  ENGINE_WORLD_UNIFORM_STORAGE_TABLE(ENGINE_WUS_REMOVE)
#undef ENGINE_WUS_REMOVE

  m_movementAuthorities[entity.index] = MovementAuthority::None;
  reset_transform_cache(entity.index);
  if (hadName && (removedName.name[0] != '\0')) {
    name_lookup_erase(core::fnv1a_32(removedName.name), entity.index);
  }
  if ((inertiaOwner != kInvalidEntity) && (inertiaOwner != entity)) {
    rederive_inverse_inertia(inertiaOwner);
  }
}

bool World::destroy_single_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  remove_all_components(entity);

  const std::uint32_t index = entity.index;
  // The entity leaves the alive arrays before its persistent id leaves the
  // index: erase_persistent_index may rebuild the index from those arrays
  // once tombstones dominate, and a rebuild that still saw this entity as
  // alive re-inserted the id it was erasing. The stale mapping then
  // blocked re-creating the id and let a surviving child's parentId resolve
  // to whichever entity next took this index.
  const PersistentId persistentId = m_entityPersistentIds[index];
  m_entityAlive[index] = false;
  if (!m_entityBeginPlayFired[index] && (m_beginPlayPendingCount > 0U)) {
    --m_beginPlayPendingCount;
  }
  m_entityBeginPlayFired[index] = false;
  m_entityPersistentIds[index] = kInvalidPersistentId;
  if (m_aliveEntityCount > 0U) {
    --m_aliveEntityCount;
  }
  erase_persistent_index(persistentId);

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
  const std::size_t descendantCount = mark_hierarchy_descendants(entity);
  for (std::size_t i = 0U; i < descendantCount; ++i) {
    const std::uint32_t index = m_cascadeMarked[i];
    if (m_entityAlive[index]) {
      static_cast<void>(queue_single_deferred_destroy(
          Entity{index, m_entityGenerations[index]}));
    }
  }
  clear_cascade_marks();

  return queue_single_deferred_destroy(entity);
}

bool World::queue_single_deferred_destroy(Entity entity) noexcept {
  // Per-index membership dedupes in O(1); a different generation
  // on the same index is a different entity and is queued as well.
  if (m_pendingDestroyQueued[entity.index] &&
      (m_pendingDestroyQueuedGeneration[entity.index] == entity.generation)) {
    return true;
  }

  if (m_pendingDestroyCount >= m_pendingDestroyEntities.size()) {
    return false;
  }

  m_pendingDestroyEntities[m_pendingDestroyCount] = entity;
  ++m_pendingDestroyCount;
  m_pendingDestroyQueued[entity.index] = true;
  m_pendingDestroyQueuedGeneration[entity.index] = entity.generation;
  return true;
}

void World::clear_pending_destroy_queue() noexcept {
  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    m_pendingDestroyQueued[m_pendingDestroyEntities[i].index] = false;
  }
  m_pendingDestroyCount = 0U;
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

  clear_pending_destroy_queue();
}

bool World::recycle_entity(Entity entity, Entity *outRecycled) noexcept {
  if ((outRecycled == nullptr) || !is_valid_entity(entity)) {
    return false;
  }
  if ((m_phase != WorldPhase::Input) && (m_phase != WorldPhase::BeginPlay) &&
      (m_phase != WorldPhase::EndPlay)) {
    core::log_message(core::LogLevel::Warning, "world",
                      "recycle_entity refused outside a mutation phase");
    return false;
  }
  // A queued deferred destroy anywhere in the subtree would either tear
  // down whoever acquires the recycled slot next or silently lose that
  // member's EndPlay, so recycling is refused while any member is queued.
  const std::size_t descendantCount = mark_hierarchy_descendants(entity);
  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    const Entity pending = m_pendingDestroyEntities[i];
    if (m_cascadeMarks[pending.index] && is_valid_entity(pending)) {
      core::log_message(core::LogLevel::Warning, "world",
                        "recycle_entity refused: destroy already queued");
      clear_cascade_marks();
      return false;
    }
  }

  // Children never survive their parent's teardown; the pool owns only
  // the root, so descendants are fully destroyed, not recycled.
  for (std::size_t i = 0U; i < descendantCount; ++i) {
    const std::uint32_t index = m_cascadeMarked[i];
    if (m_entityAlive[index]) {
      static_cast<void>(
          destroy_single_entity(Entity{index, m_entityGenerations[index]}));
    }
  }

  remove_all_components(entity);
  clear_cascade_marks();

  // The slot stays alive but under a new generation, so the handle the
  // pool hands out next is distinct from every handle held before this
  // recycle and those stale handles fail is_valid_entity. Same
  // wrap rule as destroy: zero is the invalid encoding.
  const std::uint32_t index = entity.index;
  ++m_entityGenerations[index];
  if (m_entityGenerations[index] == 0U) {
    m_entityGenerations[index] = 1U;
  }
  *outRecycled = Entity{index, m_entityGenerations[index]};
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

void World::reset_all_entities() noexcept {
  // Every entity goes, so EndPlay sequencing and hierarchy cascade order
  // do not matter; each live slot tears down through the authoritative
  // single-entity path, which cannot fail for a live entity.
  const std::uint32_t upperBound = m_nextEntityIndex;
  for (std::uint32_t index = 1U; index < upperBound; ++index) {
    if (!m_entityAlive[index]) {
      continue;
    }
    static_cast<void>(
        destroy_single_entity(Entity{index, m_entityGenerations[index]}));
  }

  // A destroy queued by a pre-reset Simulation step must not fire into
  // the replacement content after the reset.
  clear_pending_destroy_queue();
  m_hierarchyLinksStale = true;
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

core::Rng &World::random() noexcept { return m_random; }

const core::Rng &World::random() const noexcept { return m_random; }

void World::seed_random(std::uint64_t seed) noexcept {
  m_random = core::rng_from_seed(seed);
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
