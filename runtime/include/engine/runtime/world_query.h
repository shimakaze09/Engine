// Declares the variadic multi-component query machinery behind
// World::for_each (#166 W3): smallest-set selection, per-entity probing of
// the remaining component types, and the typed re-dispatch. Split from
// world.h so the storage type keeps entity/component/lifecycle state and
// this header keeps the query metaprogramming; World befriends WorldQuery,
// which reaches the per-type dispatch (component_count / try_get_component
// / for_each_primary) generated from the storage table.

#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

#include "engine/core/entity.h"

namespace engine::runtime::detail {

/// Stateless helper bundle for World's multi-component for_each; every
/// method takes the world as an explicit argument.
struct WorldQuery final {
  // Find the index (within a tuple) of the component type with lowest count.
  template <typename Tuple, typename WorldT, std::size_t... Is>
  /// Index (within the tuple) of the component type with fewest entries.
  static std::size_t
  smallest_component_index(const WorldT &world,
                           std::index_sequence<Is...>) noexcept {
    std::size_t minCount = (std::numeric_limits<std::size_t>::max)();
    std::size_t minIndex = 0U;
    const auto check = [&](std::size_t idx, std::size_t count) noexcept {
      if (count < minCount) {
        minCount = count;
        minIndex = idx;
      }
    };
    (check(Is, world.template component_count<
                   std::tuple_element_t<Is, Tuple>>()),
     ...);
    return minIndex;
  }

  // Dispatch: iterate the component set at PrimaryIdx, probe the rest.
  template <typename Tuple, std::size_t PrimaryIdx, typename WorldT,
            typename Fn, std::size_t... AllIs>
  /// Iterates the primary component set and probes the rest per entity.
  static void for_each_with_primary(const WorldT &world, Fn &&fn,
                                    std::index_sequence<AllIs...>) noexcept {
    using PrimaryC = std::tuple_element_t<PrimaryIdx, Tuple>;
    constexpr std::size_t N = std::tuple_size_v<Tuple>;
    world.template for_each_primary<PrimaryC>(
        [&fn, &world](core::Entity entity, const PrimaryC &primary) noexcept {
          std::array<const void *, N> ptrs{};
          ptrs[PrimaryIdx] = &primary;
          if (try_get_rest_excluding<Tuple, PrimaryIdx>(
                  world, entity, ptrs, std::make_index_sequence<N>{})) {
            invoke_for_each<Tuple>(fn, entity, ptrs,
                                   std::index_sequence<AllIs...>{});
          }
        });
  }

  template <typename Tuple, std::size_t PrimaryIdx, typename WorldT,
            std::size_t... AllIs>
  /// Fills ptrs for the non-primary components; false when any is absent.
  static bool try_get_rest_excluding(
      const WorldT &world, core::Entity entity,
      std::array<const void *, std::tuple_size_v<Tuple>> &ptrs,
      std::index_sequence<AllIs...>) noexcept {
    bool allPresent = true;
    const auto probe = [&](auto IndexConstant) noexcept {
      constexpr std::size_t I = decltype(IndexConstant)::value;
      if constexpr (I != PrimaryIdx) {
        if (allPresent) {
          ptrs[I] = world.template try_get_component<
              std::tuple_element_t<I, Tuple>>(entity);
          allPresent = (ptrs[I] != nullptr);
        }
      }
    };
    (probe(std::integral_constant<std::size_t, AllIs>{}), ...);
    return allPresent;
  }

  template <typename Tuple, typename Fn, std::size_t... Is>
  /// Calls fn with the typed component refs recovered from ptrs.
  static void invoke_for_each(
      Fn &&fn, core::Entity entity,
      const std::array<const void *, std::tuple_size_v<Tuple>> &ptrs,
      std::index_sequence<Is...>) noexcept {
    fn(entity,
       *static_cast<const std::tuple_element_t<Is, Tuple> *>(ptrs[Is])...);
  }

  // Entry point: picks smallest component at runtime and dispatches.
  template <typename Tuple, typename WorldT, typename Fn, std::size_t... Is>
  /// Picks the smallest component set at runtime and dispatches on it.
  static void for_each_variadic(const WorldT &world, Fn &&fn,
                                std::index_sequence<Is...>) noexcept {
    const std::size_t primaryIdx =
        smallest_component_index<Tuple>(world, std::index_sequence<Is...>{});
    const auto dispatch = [&](auto IndexConstant) noexcept {
      constexpr std::size_t I = decltype(IndexConstant)::value;
      if (I == primaryIdx) {
        for_each_with_primary<Tuple, I>(world, fn,
                                        std::index_sequence<Is...>{});
      }
    };
    (dispatch(std::integral_constant<std::size_t, Is>{}), ...);
  }
};

} // namespace engine::runtime::detail
