// Compatibility shim: the LRU cache moved to engine::content (#171 C1).
// Deletion condition: the C4 consumer migration removes the last include
// of this renderer-path header.

#pragma once

#include "engine/content/lru_cache.h"
#include "engine/renderer/asset_metadata.h"

namespace engine::renderer {

using content::EvictionCallback;
using content::LruNode;
using content::LruCache;
using content::clear_lru_cache;
using content::lru_touch;
using content::lru_remove;
using content::lru_evict_one;
using content::lru_evict_to_budget;
using content::lru_count;
using content::lru_total_size;
using content::lru_contains;
using content::lru_set_ref_count;

} // namespace engine::renderer
