// Declares service locator types and APIs for the Engine core engine.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::core {

// Lightweight type identity without RTTI. Each instantiation of type_id<T>()
// returns a unique, stable pointer that acts as a key.
using TypeId = const void *;

namespace detail {
template <typename T> struct TypeIdTag final {
  static const char kTag;
};
template <typename T> const char TypeIdTag<T>::kTag = '\0';
} // namespace detail

/// Handles type id.
template <typename T> TypeId type_id() noexcept {
  return &detail::TypeIdTag<T>::kTag;
}

// Type-erased service registry. Maps TypeId → void*.
// Fixed-capacity, no heap allocation, safe for use in hot-path init code.
class ServiceLocator final {
public:
  static constexpr std::size_t kMaxServices = 64U;

  // Register a service pointer. A null pointer removes that service type.
  // Returns false if the registry is full and the type is new.
  template <typename T> bool register_service(T *service) noexcept {
    return register_raw(type_id<T>(), static_cast<void *>(service));
  }

  // Retrieve a previously registered service. Returns nullptr if not found.
  template <typename T> T *get_service() const noexcept {
    return static_cast<T *>(get_raw(type_id<T>()));
  }

  // Check if a service type is registered.
  template <typename T> bool has_service() const noexcept {
    return get_raw(type_id<T>()) != nullptr;
  }

  // Remove a service from the registry. Returns true if it was found.
  template <typename T> bool remove_service() noexcept {
    return remove_raw(type_id<T>());
  }

  // Remove all registered services.
  void clear() noexcept;

  // Number of currently registered services.
  std::size_t count() const noexcept { return m_count; }

/// Handles register raw.
private:
  /// Handles register raw.
  bool register_raw(TypeId id, void *service) noexcept;
  /// Returns the requested value for raw.
  void *get_raw(TypeId id) const noexcept;
  /// Removes a value or component from the target system for raw.
  bool remove_raw(TypeId id) noexcept;

  /// Stores entry data used by the engine.
  struct Entry final {
    TypeId typeId = nullptr;
    void *service = nullptr;
  };

  std::array<Entry, kMaxServices> m_entries{};
  std::size_t m_count = 0U;
};

// Global engine-wide service locator instance.
ServiceLocator &global_service_locator() noexcept;

} // namespace engine::core
