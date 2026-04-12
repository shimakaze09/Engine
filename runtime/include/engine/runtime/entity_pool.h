#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/runtime/world.h"

namespace engine::runtime {

/// Pre-allocated entity pool for high-frequency spawn/release patterns.
/// Entities in the pool are created once and recycled via a free list.
/// Released entities have all components removed but their handle stays valid,
/// avoiding the overhead of full create/destroy cycles.
class EntityPool final {
public:
  static constexpr std::size_t kMaxPoolSize = 1024U;

  /// Initialise the pool by pre-creating \p count entities in \p world.
  /// Returns false if the pool was already initialised or count exceeds
  /// kMaxPoolSize.
  bool init(World *world, std::size_t count) noexcept;

  /// Acquire an entity from the pool. Returns kInvalidEntity if exhausted.
  Entity acquire() noexcept;

  /// Release an entity back to the pool for reuse.
  /// Returns false if the entity is not managed by this pool.
  bool release(Entity entity) noexcept;

  /// Number of entities currently available for acquisition.
  std::size_t available() const noexcept;

  /// Total capacity of the pool.
  std::size_t capacity() const noexcept;

  /// Whether the pool has been initialised.
  bool initialised() const noexcept;

private:
  World *m_world = nullptr;
  std::size_t m_capacity = 0U;

  // All entity handles owned by this pool.
  Entity m_entities[kMaxPoolSize]{};

  // Free list: indices into m_entities[] of available handles.
  std::uint32_t m_freeList[kMaxPoolSize]{};
  std::size_t m_freeCount = 0U;

  // Tracks whether each slot is currently acquired (in use) or free.
  bool m_inUse[kMaxPoolSize]{};
};

} // namespace engine::runtime
