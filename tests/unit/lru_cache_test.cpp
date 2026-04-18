// P1-M4-D2d: LRU cache test — load 100 assets into a 50-asset-sized cache,
// access subset repeatedly, verify LRU evicts the stale ones.

#include "engine/renderer/lru_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace engine::renderer;

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

static AssetId make_id(std::uint64_t n) noexcept {
  return static_cast<AssetId>(n + 1U); // Avoid kInvalidAssetId (0).
}

// --- Basic insert/evict ---
static void test_basic_insert_and_evict() noexcept {
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  CHECK(lru_count(cache.get()) == 0U, "empty cache count");
  CHECK(lru_total_size(cache.get()) == 0ULL, "empty cache size");

  // Insert 3 items.
  CHECK(lru_touch(cache.get(), make_id(0), 1U, 100U, 0U), "insert 0");
  CHECK(lru_touch(cache.get(), make_id(1), 2U, 200U, 0U), "insert 1");
  CHECK(lru_touch(cache.get(), make_id(2), 3U, 300U, 0U), "insert 2");

  CHECK(lru_count(cache.get()) == 3U, "count after 3 inserts");
  CHECK(lru_total_size(cache.get()) == 600U, "total size after 3 inserts");
  CHECK(lru_contains(cache.get(), make_id(0)), "contains 0");
  CHECK(lru_contains(cache.get(), make_id(1)), "contains 1");
  CHECK(lru_contains(cache.get(), make_id(2)), "contains 2");

  // Evict one — should be item 0 (oldest).
  const AssetId evicted = lru_evict_one(cache.get());
  CHECK(evicted == make_id(0), "evicted item 0");
  CHECK(lru_count(cache.get()) == 2U, "count after eviction");
  CHECK(!lru_contains(cache.get(), make_id(0)), "0 no longer in cache");

  std::printf("  test_basic_insert_and_evict: %s\n",
              g_failures == 0 ? "PASS" : "FAIL");
}

// --- Touch moves to tail ---
static void test_touch_moves_to_tail() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  lru_touch(cache.get(), make_id(0), 1U, 100U, 0U);
  lru_touch(cache.get(), make_id(1), 2U, 100U, 0U);
  lru_touch(cache.get(), make_id(2), 3U, 100U, 0U);

  // Touch item 0 again — now it should be the MRU.
  lru_touch(cache.get(), make_id(0), 4U, 100U, 0U);

  // Evict — should now be item 1 (was second-oldest, now oldest).
  const AssetId evicted = lru_evict_one(cache.get());
  CHECK(evicted == make_id(1), "evict oldest after touch reorder");

  std::printf("  test_touch_moves_to_tail: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- Protected assets are skipped ---
static void test_protected_assets_skipped() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  lru_touch(cache.get(), make_id(0), 1U, 100U, 1U); // refCount=1 (protected)
  lru_touch(cache.get(), make_id(1), 2U, 100U, 0U); // refCount=0
  lru_touch(cache.get(), make_id(2), 3U, 100U, 0U);

  const AssetId evicted = lru_evict_one(cache.get());
  CHECK(evicted == make_id(1), "skip protected, evict next LRU");

  std::printf("  test_protected_assets_skipped: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- Evict to budget ---
static void test_evict_to_budget() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  for (std::uint64_t i = 0U; i < 10U; ++i) {
    lru_touch(cache.get(), make_id(i), i + 1U, 100U, 0U);
  }

  CHECK(lru_total_size(cache.get()) == 1000U, "total size = 1000");

  // Evict down to 500 bytes budget.
  std::size_t evictedCount = 0U;
  std::size_t callbackCount = 0U;

  auto cb = [](AssetId /*id*/, void *userData) noexcept {
    auto *cnt = static_cast<std::size_t *>(userData);
    ++(*cnt);
  };

  evictedCount = lru_evict_to_budget(cache.get(), 500U, cb, &callbackCount);
  CHECK(evictedCount == 5U, "evicted 5 to reach budget");
  CHECK(callbackCount == 5U, "callback called 5 times");
  CHECK(lru_total_size(cache.get()) == 500U, "total size = 500 after eviction");
  CHECK(lru_count(cache.get()) == 5U, "5 items remaining");

  // Items 0-4 should be gone, 5-9 should remain.
  for (std::uint64_t i = 0U; i < 5U; ++i) {
    CHECK(!lru_contains(cache.get(), make_id(i)), "evicted item gone");
  }
  for (std::uint64_t i = 5U; i < 10U; ++i) {
    CHECK(lru_contains(cache.get(), make_id(i)), "remaining item present");
  }

  std::printf("  test_evict_to_budget: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- All protected = nothing evictable ---
static void test_all_protected_no_eviction() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  for (std::uint64_t i = 0U; i < 5U; ++i) {
    lru_touch(cache.get(), make_id(i), i + 1U, 100U, 1U);
  }

  const AssetId evicted = lru_evict_one(cache.get());
  CHECK(evicted == kInvalidAssetId, "nothing evictable when all protected");

  const std::size_t n = lru_evict_to_budget(cache.get(), 0U, nullptr, nullptr);
  CHECK(n == 0U, "zero evictions when all protected");

  std::printf("  test_all_protected_no_eviction: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- Explicit remove ---
static void test_explicit_remove() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  lru_touch(cache.get(), make_id(0), 1U, 100U, 0U);
  lru_touch(cache.get(), make_id(1), 2U, 200U, 0U);

  CHECK(lru_remove(cache.get(), make_id(0)), "remove succeeds");
  CHECK(!lru_contains(cache.get(), make_id(0)), "removed item gone");
  CHECK(lru_count(cache.get()) == 1U, "count after remove");
  CHECK(lru_total_size(cache.get()) == 200U, "size after remove");

  // Remove non-existent.
  CHECK(!lru_remove(cache.get(), make_id(99)), "remove non-existent fails");

  std::printf("  test_explicit_remove: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- set_ref_count ---
static void test_set_ref_count() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  lru_touch(cache.get(), make_id(0), 1U, 100U, 0U);
  lru_touch(cache.get(), make_id(1), 2U, 100U, 0U);

  // Protect item 0.
  CHECK(lru_set_ref_count(cache.get(), make_id(0), 1U), "set refcount");

  // Evict — should skip 0 and evict 1.
  const AssetId evicted = lru_evict_one(cache.get());
  CHECK(evicted == make_id(1), "evict unprotected item");

  std::printf("  test_set_ref_count: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- Stress: 100 assets into effectively 50-asset budget ---
// Load 100 assets (each 1MB), set budget to 50MB, access subset of 20
// repeatedly, then evict. Verify the 20 accessed survive, stale are evicted.
static void test_100_into_50_asset_cache() noexcept {
  const int before = g_failures;
  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());

  constexpr std::uint64_t kAssetSizeBytes = 1024ULL * 1024ULL; // 1 MB each
  constexpr std::size_t kTotalAssets = 100U;
  constexpr std::size_t kBudgetAssets = 50U;
  constexpr std::uint64_t kBudgetBytes = kBudgetAssets * kAssetSizeBytes;

  // Insert all 100 assets with sequential access frames.
  for (std::uint64_t i = 0U; i < kTotalAssets; ++i) {
    lru_touch(cache.get(), make_id(i), i + 1U, kAssetSizeBytes, 0U);
  }

  CHECK(lru_count(cache.get()) == kTotalAssets, "all 100 inserted");
  CHECK(lru_total_size(cache.get()) == kTotalAssets * kAssetSizeBytes,
        "total size = 100 MB");

  // Access a hot subset (indices 40..59) repeatedly at recent frames.
  const std::uint64_t touchFrame = kTotalAssets + 1U;
  for (std::uint64_t i = 40U; i < 60U; ++i) {
    lru_touch(cache.get(), make_id(i), touchFrame + (i - 40U), kAssetSizeBytes,
              0U);
  }

  // Evict down to 50 MB budget.
  std::size_t evictedCount =
      lru_evict_to_budget(cache.get(), kBudgetBytes, nullptr, nullptr);

  CHECK(evictedCount == 50U, "evicted 50 assets to meet budget");
  CHECK(lru_count(cache.get()) == 50U, "50 assets remaining");
  CHECK(lru_total_size(cache.get()) <= kBudgetBytes, "under budget");

  // All 20 hot-subset assets (40..59) should still be in cache.
  for (std::uint64_t i = 40U; i < 60U; ++i) {
    CHECK(lru_contains(cache.get(), make_id(i)), "hot asset survives");
  }

  // The oldest 50 assets that were NOT in the hot subset should be gone.
  // Since 0..39 were the oldest untouched, they should all be evicted.
  // 60..99 are newer but untouched; whether they survive depends on LRU order.
  // With 50 evictions, all of 0..39 (40 items) + 10 of 60..79 are evicted.
  for (std::uint64_t i = 0U; i < 40U; ++i) {
    CHECK(!lru_contains(cache.get(), make_id(i)), "stale asset evicted");
  }

  std::printf("  test_100_into_50_asset_cache: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

// --- Null safety ---
static void test_null_safety() noexcept {
  const int before = g_failures;

  CHECK(!lru_touch(nullptr, make_id(0), 1U, 100U, 0U), "null cache touch");
  CHECK(!lru_remove(nullptr, make_id(0)), "null cache remove");
  CHECK(lru_evict_one(nullptr) == kInvalidAssetId, "null cache evict");
  CHECK(lru_evict_to_budget(nullptr, 0U, nullptr, nullptr) == 0U,
        "null cache budget eviction");
  CHECK(lru_count(nullptr) == 0U, "null cache count");
  CHECK(lru_total_size(nullptr) == 0ULL, "null cache size");
  CHECK(!lru_contains(nullptr, make_id(0)), "null cache contains");
  CHECK(!lru_set_ref_count(nullptr, make_id(0), 0U), "null cache refcount");

  auto cache = std::make_unique<LruCache>();
  clear_lru_cache(cache.get());
  CHECK(!lru_touch(cache.get(), kInvalidAssetId, 1U, 100U, 0U),
        "invalid id touch");

  std::printf("  test_null_safety: %s\n",
              g_failures == before ? "PASS" : "FAIL");
}

int main() {
  std::printf("=== LRU Cache Tests ===\n");

  test_basic_insert_and_evict();
  test_touch_moves_to_tail();
  test_protected_assets_skipped();
  test_evict_to_budget();
  test_all_protected_no_eviction();
  test_explicit_remove();
  test_set_ref_count();
  test_100_into_50_asset_cache();
  test_null_safety();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
