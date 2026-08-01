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
class EntityPool final {
public:
  static constexpr std::size_t kMaxPoolSize = 1024U;

  inline bool init(World *world, std::size_t count) noexcept {
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

  /// Pops a pre-created entity from the free list; kInvalidEntity when empty.
  inline Entity acquire() noexcept {
    if (m_freeCount == 0U) {
      return kInvalidEntity;
    }
    --m_freeCount;
    const std::uint32_t slotIndex = m_freeList[m_freeCount];
    m_inUse[slotIndex] = true;
    return m_entities[slotIndex];
  }

  /// Returns an entity to the pool; false when it is not pool-owned or a
  /// mutation phase is not active. The full handle must match so
  /// stale-generation handles cannot free a slot, cleanup runs through the
  /// World's authoritative teardown, and the slot is only published free
  /// after cleanup completes so a reused entity can never inherit state.
  inline bool release(Entity entity) noexcept {
    for (std::size_t i = 0U; i < m_capacity; ++i) {
      if ((m_entities[i] == entity) && m_inUse[i]) {
        if (m_world == nullptr) {
          return false;
        }
        const WorldPhase phase = m_world->current_phase();
        if ((phase != WorldPhase::Input) && (phase != WorldPhase::BeginPlay) &&
            (phase != WorldPhase::EndPlay)) {
          core::log_message(core::LogLevel::Warning, "entity_pool",
                            "release refused outside a mutation phase");
          return false;
        }
        m_world->remove_all_components(entity);
        m_inUse[i] = false;
        m_freeList[m_freeCount] = static_cast<std::uint32_t>(i);
        ++m_freeCount;
        return true;
      }
    }
    return false;
  }

  /// Entities currently available.
  inline std::size_t available() const noexcept { return m_freeCount; }
  /// Total pool size.
  inline std::size_t capacity() const noexcept { return m_capacity; }
  /// True once initialise() succeeded.
  inline bool initialised() const noexcept { return m_world != nullptr; }

private:
  World *m_world = nullptr;
  std::size_t m_capacity = 0U;

  Entity m_entities[kMaxPoolSize]{};
  std::uint32_t m_freeList[kMaxPoolSize]{};
  std::size_t m_freeCount = 0U;
  bool m_inUse[kMaxPoolSize]{};
};

} // namespace engine::runtime
