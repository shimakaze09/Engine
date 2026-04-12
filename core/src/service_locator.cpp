#include "engine/core/service_locator.h"

#include "engine/core/logging.h"

namespace engine::core {

bool ServiceLocator::register_raw(TypeId id, void *service) noexcept {
  if (id == nullptr) {
    log_message(LogLevel::Error, "service_locator",
                "register_service: null TypeId");
    return false;
  }

  // Check for existing entry to overwrite.
  for (std::size_t i = 0U; i < m_count; ++i) {
    if (m_entries[i].typeId == id) {
      m_entries[i].service = service;
      return true;
    }
  }

  // Insert new entry.
  if (m_count >= kMaxServices) {
    log_message(LogLevel::Error, "service_locator",
                "register_service: registry full");
    return false;
  }

  m_entries[m_count].typeId = id;
  m_entries[m_count].service = service;
  ++m_count;
  return true;
}

void *ServiceLocator::get_raw(TypeId id) const noexcept {
  for (std::size_t i = 0U; i < m_count; ++i) {
    if (m_entries[i].typeId == id) {
      return m_entries[i].service;
    }
  }
  return nullptr;
}

bool ServiceLocator::remove_raw(TypeId id) noexcept {
  for (std::size_t i = 0U; i < m_count; ++i) {
    if (m_entries[i].typeId == id) {
      // Swap with last to keep dense.
      m_entries[i] = m_entries[m_count - 1U];
      m_entries[m_count - 1U] = {};
      --m_count;
      return true;
    }
  }
  return false;
}

void ServiceLocator::clear() noexcept {
  for (std::size_t i = 0U; i < m_count; ++i) {
    m_entries[i] = {};
  }
  m_count = 0U;
}

ServiceLocator &global_service_locator() noexcept {
  static ServiceLocator instance;
  return instance;
}

} // namespace engine::core
