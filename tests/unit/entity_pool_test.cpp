#include "engine/runtime/entity_pool.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

using namespace engine::runtime;

bool test_pool_init() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world) {
    std::fprintf(stderr, "FAIL: could not allocate World\n");
    return false;
  }
  EntityPool pool;

  if (!pool.init(world.get(), 10U)) {
    std::fprintf(stderr, "FAIL: pool init\n");
    return false;
  }
  if (!pool.initialised()) {
    std::fprintf(stderr, "FAIL: pool not marked initialised\n");
    return false;
  }
  if (pool.capacity() != 10U) {
    std::fprintf(stderr, "FAIL: expected capacity 10, got %zu\n",
                 pool.capacity());
    return false;
  }
  if (pool.available() != 10U) {
    std::fprintf(stderr, "FAIL: expected available 10, got %zu\n",
                 pool.available());
    return false;
  }
  return true;
}

bool test_pool_acquire_release() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 5U);

  Entity entities[5]{};
  for (int i = 0; i < 5; ++i) {
    entities[i] = pool.acquire();
    if (entities[i] == kInvalidEntity) {
      std::fprintf(stderr, "FAIL: acquire %d returned invalid\n", i);
      return false;
    }
  }

  if (pool.available() != 0U) {
    std::fprintf(stderr, "FAIL: expected 0 available after 5 acquires\n");
    return false;
  }

  // Exhausted pool returns invalid.
  if (pool.acquire() != kInvalidEntity) {
    std::fprintf(stderr, "FAIL: expected invalid from exhausted pool\n");
    return false;
  }

  // Release all.
  for (auto &entity : entities) {
    if (!pool.release(entity)) {
      std::fprintf(stderr, "FAIL: release failed\n");
      return false;
    }
  }

  if (pool.available() != 5U) {
    std::fprintf(stderr, "FAIL: expected 5 available after release\n");
    return false;
  }

  return true;
}

bool test_pool_handle_reuse() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  constexpr std::size_t kCount = 100U;
  pool.init(world.get(), kCount);

  // Acquire 100 entities and record their indices.
  std::uint32_t firstRoundIndices[kCount]{};
  for (std::size_t i = 0U; i < kCount; ++i) {
    const Entity e = pool.acquire();
    if (e == kInvalidEntity) {
      std::fprintf(stderr, "FAIL: first acquire %zu failed\n", i);
      return false;
    }
    firstRoundIndices[i] = e.index;
  }

  // Release all 100.
  for (std::size_t i = 0U; i < kCount; ++i) {
    const Entity e = world->find_entity_by_index(firstRoundIndices[i]);
    if (!pool.release(e)) {
      std::fprintf(stderr, "FAIL: release %zu failed\n", i);
      return false;
    }
  }

  // Re-acquire 100 — all should reuse the same indices (no new entity IDs).
  std::uint32_t secondRoundIndices[kCount]{};
  for (std::size_t i = 0U; i < kCount; ++i) {
    const Entity e = pool.acquire();
    if (e == kInvalidEntity) {
      std::fprintf(stderr, "FAIL: second acquire %zu failed\n", i);
      return false;
    }
    secondRoundIndices[i] = e.index;
  }

  // Verify each second-round index appeared in the first round.
  for (std::size_t i = 0U; i < kCount; ++i) {
    bool found = false;
    for (std::size_t j = 0U; j < kCount; ++j) {
      if (secondRoundIndices[i] == firstRoundIndices[j]) {
        found = true;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr,
                   "FAIL: second-round index %u not in first-round set\n",
                   secondRoundIndices[i]);
      return false;
    }
  }

  return true;
}

bool test_pool_double_init() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 5U);

  // Second init should fail.
  if (pool.init(world.get(), 5U)) {
    std::fprintf(stderr, "FAIL: double init should fail\n");
    return false;
  }
  return true;
}

bool test_pool_release_unknown() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (!world)
    return false;
  EntityPool pool;
  pool.init(world.get(), 2U);

  // Create an entity outside the pool.
  const Entity outsider = world->create_entity();
  if (pool.release(outsider)) {
    std::fprintf(stderr, "FAIL: releasing non-pool entity should fail\n");
    return false;
  }
  return true;
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    bool (*fn)();
  };

  const TestCase tests[] = {
      {"pool_init", test_pool_init},
      {"pool_acquire_release", test_pool_acquire_release},
      {"pool_handle_reuse_100", test_pool_handle_reuse},
      {"pool_double_init", test_pool_double_init},
      {"pool_release_unknown", test_pool_release_unknown},
  };

  int failures = 0;
  for (const auto &tc : tests) {
    std::printf("  %-40s ", tc.name);
    if (tc.fn()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
