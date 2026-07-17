// Verifies FixedHashTable insert/find/erase/tombstone-reuse behavior,
// including probe-chain integrity across deletions and full-table fallback.

#include "engine/core/fixed_hash_table.h"

#include <cstdint>
#include <cstdio>

namespace {

using engine::core::FixedHashTable;

/// Basic upsert/find/erase round trip.
int test_round_trip() {
  FixedHashTable<std::uint32_t, int, 16U> table;

  if (!table.insert(1U, 10) || !table.insert(2U, 20) ||
      !table.insert(3U, 30)) {
    return 1;
  }
  if (table.size() != 3U) {
    return 2;
  }

  const int *found = table.find(2U);
  if ((found == nullptr) || (*found != 20)) {
    return 3;
  }
  if (table.find(99U) != nullptr) {
    return 4;
  }

  if (!table.insert(2U, 22)) {
    return 5; // overwrite
  }
  found = table.find(2U);
  if ((found == nullptr) || (*found != 22) || (table.size() != 3U)) {
    return 6;
  }

  if (!table.erase(2U) || table.erase(2U)) {
    return 7;
  }
  if ((table.find(2U) != nullptr) || (table.size() != 2U) ||
      (table.tombstone_count() != 1U)) {
    return 8;
  }

  return 0;
}

/// Erasing an entry mid-probe-chain must not hide later entries.
int test_probe_chain_integrity() {
  // Capacity 8; craft keys that collide by brute force: find three keys with
  // the same home slot by probing insert order behavior instead — insert
  // many keys, erase one, and confirm every other key stays findable.
  FixedHashTable<std::uint32_t, std::uint32_t, 32U> table;

  for (std::uint32_t key = 1U; key <= 24U; ++key) {
    if (!table.insert(key, key * 100U)) {
      return 10;
    }
  }

  // Erase every third key.
  for (std::uint32_t key = 3U; key <= 24U; key += 3U) {
    if (!table.erase(key)) {
      return 11;
    }
  }

  // Every remaining key must still resolve through tombstoned chains.
  for (std::uint32_t key = 1U; key <= 24U; ++key) {
    const std::uint32_t *found = table.find(key);
    if ((key % 3U) == 0U) {
      if (found != nullptr) {
        return 12;
      }
    } else if ((found == nullptr) || (*found != key * 100U)) {
      return 13;
    }
  }

  return 0;
}

/// Tombstoned slots are reused by later inserts.
int test_tombstone_reuse() {
  FixedHashTable<std::uint32_t, int, 8U> table;

  for (std::uint32_t key = 1U; key <= 8U; ++key) {
    if (!table.insert(key, static_cast<int>(key))) {
      return 20;
    }
  }
  // Table full of occupied slots: new key must fail.
  if (table.insert(100U, 1)) {
    return 21;
  }

  if (!table.erase(4U)) {
    return 22;
  }
  // The freed slot must accept a new key even with zero empty slots left.
  if (!table.insert(100U, 42)) {
    return 23;
  }
  const int *found = table.find(100U);
  if ((found == nullptr) || (*found != 42)) {
    return 24;
  }
  if (table.tombstone_count() != 0U) {
    return 25;
  }

  // All original keys except 4 must survive.
  for (std::uint32_t key = 1U; key <= 8U; ++key) {
    const bool present = table.find(key) != nullptr;
    if ((key == 4U) ? present : !present) {
      return 26;
    }
  }

  return 0;
}

/// clear() resets entries and tombstones.
int test_clear() {
  FixedHashTable<std::uint32_t, int, 8U> table;
  if (!table.insert(1U, 1) || !table.erase(1U) || !table.insert(2U, 2)) {
    return 30;
  }
  table.clear();
  if ((table.size() != 0U) || (table.tombstone_count() != 0U) ||
      (table.find(2U) != nullptr)) {
    return 31;
  }
  if (!table.insert(3U, 3) || (table.find(3U) == nullptr)) {
    return 32;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const int results[] = {
      test_round_trip(),
      test_probe_chain_integrity(),
      test_tombstone_reuse(),
      test_clear(),
  };

  for (const int result : results) {
    if (result != 0) {
      std::fprintf(stderr, "fixed_hash_table_test failed with code %d\n",
                   result);
      return result;
    }
  }

  return 0;
}
