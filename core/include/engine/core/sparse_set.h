// Fixed-capacity sparse-set component storage keyed by entity index, with
// optional generation validation when the entity type carries a generation.

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

template <typename T>
concept HasGenerationMember = requires(T value) {
  { value.generation } -> std::convertible_to<std::uint32_t>;
};

/// Dense component storage with O(1) add/remove/lookup by entity index.
/// When EntityType has a generation member, lookups reject handles whose
/// generation differs from the stored entity, so stale handles miss instead
/// of aliasing a recycled index.
template <typename EntityType, typename Component, std::size_t MaxEntities,
          std::size_t MaxComponents, std::size_t StateCount = 1U>
class SparseSet final {
public:
  static constexpr std::int32_t kMissingIndex = -1;

  static_assert(HasIndexMember<EntityType>,
                "EntityType must have a uint32_t index member");

  SparseSet() noexcept { m_sparse.fill(kMissingIndex); }

  /// Removes every entry and resets the sparse index mapping.
  void clear() noexcept {
    m_sparse.fill(kMissingIndex);
    m_count = 0U;
  }

  /// Inserts or overwrites the component for an entity across all states.
  /// A slot held under an older generation of the same index is adopted:
  /// distinct generations of one index cannot both be alive, so the newer
  /// entity owns the slot and the stale data is replaced.
  bool add(EntityType entity, const Component &component) noexcept {
    if (!is_entity_in_range(entity)) {
      return false;
    }

    const std::int32_t rawIndex = m_sparse[entity_index(entity)];
    if (rawIndex != kMissingIndex) {
      const std::size_t slot = static_cast<std::size_t>(rawIndex);
      m_entities[slot] = entity;
      for (std::size_t state = 0U; state < StateCount; ++state) {
        m_components[state][slot] = component;
      }
      return true;
    }

    if (m_count >= MaxComponents) {
      return false;
    }

    const std::size_t newIndex = m_count;
    m_entities[newIndex] = entity;
    m_sparse[entity_index(entity)] = static_cast<std::int32_t>(newIndex);
    for (std::size_t state = 0U; state < StateCount; ++state) {
      m_components[state][newIndex] = component;
    }

    ++m_count;
    return true;
  }

  /// Removes the entity's component with swap-and-pop; rejects stale handles.
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
      m_sparse[entity_index(movedEntity)] =
          static_cast<std::int32_t>(removeSlot);
    }

    m_sparse[entity_index(entity)] = kMissingIndex;
    --m_count;
    return true;
  }

  /// Returns whether a live entry exists for this exact entity handle.
  bool contains(EntityType entity) const noexcept {
    return sparse_index(entity) != kMissingIndex;
  }

  /// Copies the entity's component for one state; false on miss.
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

  /// Returns a mutable pointer to the entity's component or nullptr on miss.
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

  /// Returns a const pointer to the entity's component or nullptr on miss.
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

  /// Returns the number of live entries.
  std::size_t count() const noexcept { return m_count; }

  /// Returns the entity stored at a dense slot (slot must be < count()).
  EntityType entity_at(std::size_t denseIndex) const noexcept {
    assert(denseIndex < m_count && "SparseSet::entity_at: index out of range");
    return m_entities[denseIndex];
  }

  /// Returns the component stored at a dense slot (bounds asserted).
  Component &component_at(std::size_t denseIndex,
                          std::size_t stateIndex = 0U) noexcept {
    assert(denseIndex < m_count &&
           "SparseSet::component_at: index out of range");
    assert(stateIndex < StateCount &&
           "SparseSet::component_at: state index out of range");
    return m_components[stateIndex][denseIndex];
  }

  /// Returns the component stored at a dense slot (bounds asserted).
  const Component &component_at(std::size_t denseIndex,
                                std::size_t stateIndex = 0U) const noexcept {
    assert(denseIndex < m_count &&
           "SparseSet::component_at: index out of range");
    assert(stateIndex < StateCount &&
           "SparseSet::component_at: state index out of range");
    return m_components[stateIndex][denseIndex];
  }

  /// Returns the dense entity array for range iteration.
  const EntityType *entity_data() const noexcept { return m_entities.data(); }

  /// Returns the dense component array for one state, or nullptr.
  Component *component_data(std::size_t stateIndex = 0U) noexcept {
    if (stateIndex >= StateCount) {
      return nullptr;
    }

    return m_components[stateIndex].data();
  }

  /// Returns the dense component array for one state, or nullptr.
  const Component *component_data(std::size_t stateIndex = 0U) const noexcept {
    if (stateIndex >= StateCount) {
      return nullptr;
    }

    return m_components[stateIndex].data();
  }

private:
  /// Extracts the sparse array index from an entity handle.
  static std::uint32_t entity_index(EntityType entity) noexcept {
    return static_cast<std::uint32_t>(entity.index);
  }

  /// Index 0 is reserved as the invalid entity across the engine.
  bool is_entity_in_range(EntityType entity) const noexcept {
    const std::uint32_t index = entity_index(entity);
    return (index > 0U) && (index <= static_cast<std::uint32_t>(MaxEntities));
  }

  /// Resolves an entity to its dense slot; kMissingIndex when absent or, for
  /// generation-carrying entity types, when the stored generation differs.
  std::int32_t sparse_index(EntityType entity) const noexcept {
    if (!is_entity_in_range(entity)) {
      return kMissingIndex;
    }

    const std::int32_t denseIndex = m_sparse[entity_index(entity)];
    if constexpr (HasGenerationMember<EntityType>) {
      if (denseIndex != kMissingIndex) {
        const std::size_t slot = static_cast<std::size_t>(denseIndex);
        if (static_cast<std::uint32_t>(m_entities[slot].generation) !=
            static_cast<std::uint32_t>(entity.generation)) {
          return kMissingIndex;
        }
      }
    }

    return denseIndex;
  }

  std::array<std::array<Component, MaxComponents>, StateCount> m_components{};
  std::array<EntityType, MaxComponents> m_entities{};
  std::array<std::int32_t, MaxEntities + 1U> m_sparse{};
  std::size_t m_count = 0U;
};

} // namespace engine::core
