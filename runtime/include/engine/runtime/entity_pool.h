// Declares entity pool types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/logging.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

/// Pre-allocated entity pool for high-frequency spawn/release patterns.
/// Entities in the pool are created once and recycled via a free list.
/// Released entities have all components removed but their handle stays valid,
/// avoiding the overhead of full create/destroy cycles.
/// Ownership contract (#57): the pool records the World's content epoch at
/// init. When the world's entire contents are replaced (scene load commit,
/// reset_world), the epoch advances and the pool expires: every operation
/// fails closed (invalid entity / false / zero), the slots and free list are
/// cleared so stale handles can never alias replacement-world entities, and
/// a warning logs once per expiry episode. An expired pool returns to the
/// uninitialised state and may be re-seeded with init(); init() on a live
/// pool still fails. The pool must not outlive the World it was seeded from.
class EntityPool final {
public:
  static constexpr std::size_t kMaxPoolSize = 1024U;

  /// Seeds the pool with count pre-created entities and records the
  /// world's content epoch. Fails on a live initialised pool or invalid
  /// parameters; succeeds on a fresh or expired pool.
  inline bool init(World *world, std::size_t count) noexcept {
    expire_if_content_replaced();
    if (m_world != nullptr) {
      core::log_message(core::LogLevel::Error, "entity_pool",
                        "pool already initialised");
      return false;
    }
    if ((world == nullptr) || (count == 0U) || (count > kMaxPoolSize)) {
      core::log_message(core::LogLevel::Error, "entity_pool",
                        "invalid pool parameters");
      return false;
    }
    m_world = world;
    m_epoch = world->content_epoch();
    m_capacity = count;
    m_freeCount = count;
    for (std::size_t i = 0U; i < count; ++i) {
      const Entity entity = world->create_entity();
      if (entity == kInvalidEntity) {
        core::log_message(core::LogLevel::Error, "entity_pool",
                          "failed to pre-allocate entity");
        for (std::size_t j = 0U; j < i; ++j) {
          static_cast<void>(world->destroy_entity(m_entities[j]));
        }
        m_world = nullptr;
        m_capacity = 0U;
        m_freeCount = 0U;
        return false;
      }
      m_entities[i] = entity;
      m_inUse[i] = false;
      m_freeList[count - 1U - i] = static_cast<std::uint32_t>(i);
    }
    return true;
  }

  /// Pops a pre-created entity from the free list and re-arms its
  /// lifecycle through the World (components attached afterwards get
  /// fresh-entity BeginPlay); kInvalidEntity when empty, expired, or when
  /// the World refuses activation outside a mutation phase.
  inline Entity acquire() noexcept {
    expire_if_content_replaced();
    if ((m_freeCount == 0U) || (m_world == nullptr)) {
      return kInvalidEntity;
    }
    const std::uint32_t slotIndex = m_freeList[m_freeCount - 1U];
    if (!m_world->activate_recycled_entity(m_entities[slotIndex])) {
      return kInvalidEntity;
    }
    --m_freeCount;
    m_inUse[slotIndex] = true;
    return m_entities[slotIndex];
  }

  /// Returns an entity to the pool; false when the pool is expired, the
  /// entity is not owned by this pool's own slot array (cross-pool and
  /// outsider handles never match a slot), or the World refuses the
  /// recycle (wrong phase, or a deferred destroy is already queued for
  /// it). The full handle must match so stale-generation handles cannot
  /// free a slot, cleanup runs through the World's guarded recycle
  /// operation, and the slot is only published free after cleanup
  /// completes so a reused entity can never inherit state.
  inline bool release(Entity entity) noexcept {
    expire_if_content_replaced();
    for (std::size_t i = 0U; i < m_capacity; ++i) {
      if ((m_entities[i] == entity) && m_inUse[i]) {
        if ((m_world == nullptr) || !m_world->recycle_entity(entity)) {
          return false;
        }
        m_inUse[i] = false;
        m_freeList[m_freeCount] = static_cast<std::uint32_t>(i);
        ++m_freeCount;
        return true;
      }
    }
    return false;
  }

  /// Entities currently available; 0 once expired.
  inline std::size_t available() const noexcept {
    return content_matches() ? m_freeCount : 0U;
  }
  /// Total pool size; 0 once expired.
  inline std::size_t capacity() const noexcept {
    return content_matches() ? m_capacity : 0U;
  }
  /// True while the pool is seeded against the world's current contents;
  /// false before init() and false again once a content replacement
  /// expires the pool (const accessors report expiry without mutating).
  inline bool initialised() const noexcept { return content_matches(); }

private:
  /// True when the pool is seeded and its recorded epoch still matches
  /// the world's content epoch.
  inline bool content_matches() const noexcept {
    return (m_world != nullptr) && (m_epoch == m_world->content_epoch());
  }

  /// Self-invalidates on content-epoch mismatch: logs once, then clears
  /// every slot and the free list back to the uninitialised state so no
  /// stale handle can reach the replacement world through this pool.
  inline void expire_if_content_replaced() noexcept {
    if ((m_world == nullptr) || (m_epoch == m_world->content_epoch())) {
      return;
    }
    core::log_message(core::LogLevel::Warning, "entity_pool",
                      "pool expired: world contents were replaced");
    m_world = nullptr;
    m_epoch = 0U;
    m_capacity = 0U;
    m_freeCount = 0U;
    for (std::size_t i = 0U; i < kMaxPoolSize; ++i) {
      m_entities[i] = Entity{};
      m_freeList[i] = 0U;
      m_inUse[i] = false;
    }
  }

  World *m_world = nullptr;
  std::uint32_t m_epoch = 0U;
  std::size_t m_capacity = 0U;

  Entity m_entities[kMaxPoolSize]{};
  std::uint32_t m_freeList[kMaxPoolSize]{};
  std::size_t m_freeCount = 0U;
  bool m_inUse[kMaxPoolSize]{};
};

} // namespace engine::runtime
