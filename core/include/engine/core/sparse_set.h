// Declares sparse set types and APIs for the Engine core engine.

#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace engine::core {

template <typename T>
concept HasIndexMember = requires(T value) {
  { value.index } -> std::convertible_to<std::uint32_t>;
};

template <typename EntityType, typename Component, std::size_t MaxEntities,
          std::size_t MaxComponents, std::size_t StateCount = 1U>
/// Owns the sparse set behavior and state.
class SparseSet final {
public:
  static constexpr std::int32_t kMissingIndex = -1;

  static_assert(HasIndexMember<EntityType>,
                "EntityType must have a uint32_t index member");

  SparseSet() noexcept { m_sparse.fill(kMissingIndex); }

  /// Handles clear.
  void clear() noexcept {
    /// Handles fill.
    m_sparse.fill(kMissingIndex);
    m_count = 0U;
  }

  /// Adds a value or component to the target system.
  bool add(EntityType entity, const Component &component) noexcept {
    const std::int32_t existingIndex = sparse_index(entity);
    if (existingIndex != kMissingIndex) {
      for (std::size_t state = 0U; state < StateCount; ++state) {
        m_components[state][static_cast<std::size_t>(existingIndex)] =
            component;
      }
      return true;
    }

    if (!is_entity_in_range(entity) || (m_count >= MaxComponents)) {
      return false;
    }

    const std::size_t newIndex = m_count;
    m_entities[newIndex] = entity;
    /// Handles entity index.
    m_sparse[entity_index(entity)] = static_cast<std::int32_t>(newIndex);
    for (std::size_t state = 0U; state < StateCount; ++state) {
      m_components[state][newIndex] = component;
    }

    ++m_count;
    return true;
  }

  /// Removes a value or component from the target system.
  bool remove(EntityType entity) noexcept {
    const std::int32_t removeIndex = sparse_index(entity);
    if (removeIndex == kMissingIndex) {
      return false;
    }

    const std::size_t removeSlot = static_cast<std::size_t>(removeIndex);
    const std::size_t lastSlot = m_count - 1U;

    if (removeSlot != lastSlot) {
      for (std::size_t state = 0U; state < StateCount; ++state) {
        m_components[state][removeSlot] = m_components[state][lastSlot];
      }

      const EntityType movedEntity = m_entities[lastSlot];
      m_entities[removeSlot] = movedEntity;
      /// Handles entity index.
      m_sparse[entity_index(movedEntity)] =
          static_cast<std::int32_t>(removeSlot);
    }

    /// Handles entity index.
    m_sparse[entity_index(entity)] = kMissingIndex;
    --m_count;
    return true;
  }

  /// Handles contains.
  bool contains(EntityType entity) const noexcept {
    return sparse_index(entity) != kMissingIndex;
  }

  /// Returns the requested value.
  bool get(EntityType entity, Component *out,
           std::size_t stateIndex = 0U) const noexcept {
    if ((out == nullptr) || (stateIndex >= StateCount)) {
      return false;
    }

    const std::int32_t index = sparse_index(entity);
    if (index == kMissingIndex) {
      return false;
    }

    *out = m_components[stateIndex][static_cast<std::size_t>(index)];
    return true;
  }

  /// Returns the requested value for ptr.
  Component *get_ptr(EntityType entity, std::size_t stateIndex = 0U) noexcept {
    if (stateIndex >= StateCount) {
      return nullptr;
    }

    const std::int32_t index = sparse_index(entity);
    if (index == kMissingIndex) {
      return nullptr;
    }

    return &m_components[stateIndex][static_cast<std::size_t>(index)];
  }

  /// Returns the requested value for ptr.
  const Component *get_ptr(EntityType entity,
                           std::size_t stateIndex = 0U) const noexcept {
    if (stateIndex >= StateCount) {
      return nullptr;
    }

    const std::int32_t index = sparse_index(entity);
    if (index == kMissingIndex) {
      return nullptr;
    }

    return &m_components[stateIndex][static_cast<std::size_t>(index)];
  }

  /// Handles count.
  std::size_t count() const noexcept { return m_count; }

  /// Handles entity at.
  EntityType entity_at(std::size_t denseIndex) const noexcept {
    assert(denseIndex < m_count && "SparseSet::entity_at: index out of range");
    return m_entities[denseIndex];
  }

  /// Handles component at.
  Component &component_at(std::size_t denseIndex,
                          std::size_t stateIndex = 0U) noexcept {
    assert(denseIndex < m_count &&
           "SparseSet::component_at: index out of range");
    assert(stateIndex < StateCount &&
           "SparseSet::component_at: state index out of range");
    return m_components[stateIndex][denseIndex];
  }

  /// Handles component at.
  const Component &component_at(std::size_t denseIndex,
                                std::size_t stateIndex = 0U) const noexcept {
    assert(denseIndex < m_count &&
           "SparseSet::component_at: index out of range");
    assert(stateIndex < StateCount &&
           "SparseSet::component_at: state index out of range");
    return m_components[stateIndex][denseIndex];
  }

  /// Handles entity data.
  const EntityType *entity_data() const noexcept { return m_entities.data(); }

  /// Handles component data.
  Component *component_data(std::size_t stateIndex = 0U) noexcept {
    if (stateIndex >= StateCount) {
      return nullptr;
    }

    return m_components[stateIndex].data();
  }

  /// Handles component data.
  const Component *component_data(std::size_t stateIndex = 0U) const noexcept {
    if (stateIndex >= StateCount) {
      return nullptr;
    }

    return m_components[stateIndex].data();
  }

/// Handles entity index.
private:
  /// Handles entity index.
  static std::uint32_t entity_index(EntityType entity) noexcept {
    return static_cast<std::uint32_t>(entity.index);
  }

  /// Returns whether is entity in range.
  bool is_entity_in_range(EntityType entity) const noexcept {
    const std::uint32_t index = entity_index(entity);
    return (index > 0U) && (index <= static_cast<std::uint32_t>(MaxEntities));
  }

  /// Handles sparse index.
  std::int32_t sparse_index(EntityType entity) const noexcept {
    if (!is_entity_in_range(entity)) {
      return kMissingIndex;
    }

    return m_sparse[entity_index(entity)];
  }

  std::array<std::array<Component, MaxComponents>, StateCount> m_components{};
  std::array<EntityType, MaxComponents> m_entities{};
  std::array<std::int32_t, MaxEntities + 1U> m_sparse{};
  std::size_t m_count = 0U;
};

} // namespace engine::core
