#include "engine/runtime/entity_pool.h"

#include "engine/core/logging.h"

namespace engine::runtime {

bool EntityPool::init(World *world, std::size_t count) noexcept {
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
      // Clean up already-created entities.
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
    // Free list is filled from top-down so acquire returns in order.
    m_freeList[count - 1U - i] = static_cast<std::uint32_t>(i);
  }

  return true;
}

Entity EntityPool::acquire() noexcept {
  if (m_freeCount == 0U) {
    return kInvalidEntity;
  }

  --m_freeCount;
  const std::uint32_t slotIndex = m_freeList[m_freeCount];
  m_inUse[slotIndex] = true;
  return m_entities[slotIndex];
}

bool EntityPool::release(Entity entity) noexcept {
  // Find the slot for this entity.
  for (std::size_t i = 0U; i < m_capacity; ++i) {
    if ((m_entities[i].index == entity.index) && m_inUse[i]) {
      m_inUse[i] = false;
      m_freeList[m_freeCount] = static_cast<std::uint32_t>(i);
      ++m_freeCount;

      // Remove components so the entity is "dormant" until next acquire.
      if (m_world != nullptr) {
        static_cast<void>(m_world->remove_transform(entity));
        static_cast<void>(m_world->remove_rigid_body(entity));
        static_cast<void>(m_world->remove_collider(entity));
        static_cast<void>(m_world->remove_mesh_component(entity));
        static_cast<void>(m_world->remove_name_component(entity));
        static_cast<void>(m_world->remove_script_component(entity));
        static_cast<void>(m_world->remove_light_component(entity));
      }

      return true;
    }
  }

  return false;
}

std::size_t EntityPool::available() const noexcept { return m_freeCount; }

std::size_t EntityPool::capacity() const noexcept { return m_capacity; }

bool EntityPool::initialised() const noexcept { return m_world != nullptr; }

} // namespace engine::runtime
