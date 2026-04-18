#include "engine/renderer/lru_cache.h"

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

namespace {

std::uint32_t find_node_index(const LruCache *cache, AssetId id) noexcept {
  for (std::uint32_t i = 0U; i < LruCache::kMaxNodes; ++i) {
    if (cache->nodes[i].occupied && (cache->nodes[i].assetId == id)) {
      return i;
    }
  }
  return LruNode::kInvalidIndex;
}

std::uint32_t find_free_slot(const LruCache *cache) noexcept {
  for (std::uint32_t i = 0U; i < LruCache::kMaxNodes; ++i) {
    if (!cache->nodes[i].occupied) {
      return i;
    }
  }
  return LruNode::kInvalidIndex;
}

void unlink_node(LruCache *cache, std::uint32_t idx) noexcept {
  LruNode &node = cache->nodes[idx];
  const std::uint32_t p = node.prev;
  const std::uint32_t n = node.next;

  if (p != LruNode::kInvalidIndex) {
    cache->nodes[p].next = n;
  } else {
    cache->head = n;
  }

  if (n != LruNode::kInvalidIndex) {
    cache->nodes[n].prev = p;
  } else {
    cache->tail = p;
  }

  node.prev = LruNode::kInvalidIndex;
  node.next = LruNode::kInvalidIndex;
}

void push_to_tail(LruCache *cache, std::uint32_t idx) noexcept {
  LruNode &node = cache->nodes[idx];
  node.prev = cache->tail;
  node.next = LruNode::kInvalidIndex;

  if (cache->tail != LruNode::kInvalidIndex) {
    cache->nodes[cache->tail].next = idx;
  } else {
    cache->head = idx;
  }

  cache->tail = idx;
}

} // namespace

void clear_lru_cache(LruCache *cache) noexcept {
  if (cache == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < LruCache::kMaxNodes; ++i) {
    cache->nodes[i] = LruNode{};
  }
  cache->head = LruNode::kInvalidIndex;
  cache->tail = LruNode::kInvalidIndex;
  cache->count = 0U;
  cache->totalSizeBytes = 0ULL;
}

bool lru_touch(LruCache *cache, AssetId id, std::uint64_t accessFrame,
               std::uint64_t sizeBytes, std::uint32_t refCount) noexcept {
  if ((cache == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  // Check if already present — move to tail.
  const std::uint32_t existing = find_node_index(cache, id);
  if (existing != LruNode::kInvalidIndex) {
    LruNode &node = cache->nodes[existing];
    node.lastAccessFrame = accessFrame;
    node.refCount = refCount;
    // Update size if changed.
    cache->totalSizeBytes -= node.sizeBytes;
    node.sizeBytes = sizeBytes;
    cache->totalSizeBytes += sizeBytes;
    unlink_node(cache, existing);
    push_to_tail(cache, existing);
    return true;
  }

  // Insert new node.
  const std::uint32_t slot = find_free_slot(cache);
  if (slot == LruNode::kInvalidIndex) {
    return false; // Cache is full — caller should evict first.
  }

  LruNode &node = cache->nodes[slot];
  node.assetId = id;
  node.lastAccessFrame = accessFrame;
  node.sizeBytes = sizeBytes;
  node.refCount = refCount;
  node.occupied = true;
  node.prev = LruNode::kInvalidIndex;
  node.next = LruNode::kInvalidIndex;

  push_to_tail(cache, slot);
  ++cache->count;
  cache->totalSizeBytes += sizeBytes;
  return true;
}

bool lru_remove(LruCache *cache, AssetId id) noexcept {
  if ((cache == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::uint32_t idx = find_node_index(cache, id);
  if (idx == LruNode::kInvalidIndex) {
    return false;
  }

  cache->totalSizeBytes -= cache->nodes[idx].sizeBytes;
  unlink_node(cache, idx);
  cache->nodes[idx] = LruNode{};
  --cache->count;
  return true;
}

AssetId lru_evict_one(LruCache *cache) noexcept {
  if ((cache == nullptr) || (cache->head == LruNode::kInvalidIndex)) {
    return kInvalidAssetId;
  }

  // Walk from head (LRU) and find the first node with refCount == 0.
  std::uint32_t cur = cache->head;
  while (cur != LruNode::kInvalidIndex) {
    LruNode &node = cache->nodes[cur];
    if (node.refCount == 0U) {
      const AssetId evictedId = node.assetId;
      cache->totalSizeBytes -= node.sizeBytes;
      unlink_node(cache, cur);
      node = LruNode{};
      --cache->count;
      return evictedId;
    }
    cur = node.next;
  }

  return kInvalidAssetId; // All assets are protected.
}

std::size_t lru_evict_to_budget(LruCache *cache, std::uint64_t targetBytes,
                                EvictionCallback callback,
                                void *userData) noexcept {
  if (cache == nullptr) {
    return 0U;
  }

  std::size_t evicted = 0U;
  while (cache->totalSizeBytes > targetBytes) {
    const AssetId id = lru_evict_one(cache);
    if (id == kInvalidAssetId) {
      break; // Nothing more can be evicted.
    }
    if (callback != nullptr) {
      callback(id, userData);
    }
    ++evicted;
  }
  return evicted;
}

std::size_t lru_count(const LruCache *cache) noexcept {
  return (cache != nullptr) ? cache->count : 0U;
}

std::uint64_t lru_total_size(const LruCache *cache) noexcept {
  return (cache != nullptr) ? cache->totalSizeBytes : 0ULL;
}

bool lru_contains(const LruCache *cache, AssetId id) noexcept {
  if ((cache == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }
  return find_node_index(cache, id) != LruNode::kInvalidIndex;
}

bool lru_set_ref_count(LruCache *cache, AssetId id,
                       std::uint32_t refCount) noexcept {
  if ((cache == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::uint32_t idx = find_node_index(cache, id);
  if (idx == LruNode::kInvalidIndex) {
    return false;
  }

  cache->nodes[idx].refCount = refCount;
  return true;
}

} // namespace engine::renderer
