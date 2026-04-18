#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"

namespace engine::renderer {

/// Callback invoked before an asset is evicted from the cache.
/// Implementations should release GPU handles, cooked physics data, etc.
/// @param id The asset being evicted.
/// @param userData Opaque pointer forwarded from the caller.
using EvictionCallback = void (*)(AssetId id, void *userData) noexcept;

/// Intrusive doubly-linked LRU node.  Embedded inside the cache's node array;
/// no per-node heap allocation.
struct LruNode final {
  AssetId assetId = kInvalidAssetId;
  std::uint64_t lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  std::uint32_t refCount = 0U;

  // Intrusive list pointers (indices into the node array, not raw pointers).
  // kInvalidIndex means "no link."
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFU;
  std::uint32_t prev = kInvalidIndex;
  std::uint32_t next = kInvalidIndex;
  bool occupied = false;
};

/// Fixed-capacity LRU cache backed by a flat node array and an intrusive
/// doubly-linked list ordered by access time (head = least recently used,
/// tail = most recently used).
///
/// Capacity is a template-free compile-time choice so that the cache can live
/// on the stack or as a plain member without heap allocation.
struct LruCache final {
  static constexpr std::size_t kMaxNodes = 4096U;

  LruNode nodes[kMaxNodes]{};
  std::uint32_t head = LruNode::kInvalidIndex; // LRU end (oldest)
  std::uint32_t tail = LruNode::kInvalidIndex; // MRU end (newest)
  std::size_t count = 0U;
  std::uint64_t totalSizeBytes = 0ULL;
};

/// Initialise / reset all nodes.
void clear_lru_cache(LruCache *cache) noexcept;

/// Insert or touch an asset.  If already present the node is moved to the tail
/// (most-recently-used position).  Returns true on success.
bool lru_touch(LruCache *cache, AssetId id, std::uint64_t accessFrame,
               std::uint64_t sizeBytes, std::uint32_t refCount) noexcept;

/// Remove a specific asset from the cache (explicit unload path).
bool lru_remove(LruCache *cache, AssetId id) noexcept;

/// Evict the least-recently-used asset whose refCount == 0.
/// Returns the evicted AssetId, or kInvalidAssetId if nothing can be evicted.
AssetId lru_evict_one(LruCache *cache) noexcept;

/// Evict assets (LRU order, skipping protected) until totalSizeBytes is at or
/// below @p targetBytes.  @p callback is invoked for each eviction (may be
/// nullptr).  Returns the number of assets evicted.
std::size_t lru_evict_to_budget(LruCache *cache, std::uint64_t targetBytes,
                                EvictionCallback callback,
                                void *userData) noexcept;

/// Query helpers.
std::size_t lru_count(const LruCache *cache) noexcept;
std::uint64_t lru_total_size(const LruCache *cache) noexcept;
bool lru_contains(const LruCache *cache, AssetId id) noexcept;

/// Update the ref count for an asset already in the cache.
bool lru_set_ref_count(LruCache *cache, AssetId id,
                       std::uint32_t refCount) noexcept;

} // namespace engine::renderer
