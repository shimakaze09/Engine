// ECS stress test: creates entities up to the World capacity, attaches a
// transform to each, and iterates them once through the flat visitor. The
// contract asserted is that the iteration visits exactly the transforms
// that were created. The timings it prints are diagnostic only: functional
// tests never gate on wall-clock thresholds, and the ECS iteration budget
// lives in engine_bench_ecs_perf against tests/benchmark/perf_baseline.json.
//
// World capacity defaults to 65536 entities, and the 50k integration stress
// test covers the raised-capacity path.

#include <chrono>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

using namespace engine::runtime;
using Clock = std::chrono::high_resolution_clock;

namespace {

bool stress_create_and_iterate() noexcept {
  auto world = std::unique_ptr<World>(new (std::nothrow) World());
  if (!world) {
    return false;
  }

  const std::size_t cap = World::kMaxEntities;

  // ---- creation ----
  const auto t0 = Clock::now(); // wall-clock: diagnostic
  std::size_t created = 0U;
  for (std::size_t i = 0U; i < cap; ++i) {
    const Entity e = world->create_entity();
    if (e == kInvalidEntity) {
      // World full: the capacity boundary, reached one below the nominal
      // capacity because index 0 is the invalid handle.
      break;
    }
    Transform t{};
    t.position = engine::math::Vec3(static_cast<float>(i), 0.0F, 0.0F);
    if (!world->add_transform(e, t)) {
      std::printf("FAIL: add_transform refused entity %zu\n", i);
      return false;
    }
    ++created;
  }
  const auto t1 = Clock::now(); // wall-clock: diagnostic

  // ---- flat iteration (visitor over all transforms) ----
  std::size_t visited = 0U;
  auto visitor = [](Entity /*e*/, const Transform & /*t*/, void *ud) noexcept {
    auto *count = static_cast<std::size_t *>(ud);
    ++(*count);
  };
  world->for_each_transform(visitor, &visited);

  const auto t2 = Clock::now(); // wall-clock: diagnostic

  const double createMs =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double iterMs =
      std::chrono::duration<double, std::milli>(t2 - t1).count();

  std::printf("[ecs_stress] entities=%zu  create=%.2fms  iterate=%.2fms\n",
              visited, createMs, iterMs);

  if (created == 0U) {
    std::printf("FAIL: no entity could be created\n");
    return false;
  }
  if (visited != created) {
    std::printf("FAIL: iterated %zu transforms, created %zu\n", visited,
                created);
    return false;
  }
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!stress_create_and_iterate()) {
    return 1;
  }
  std::printf("PASS: ecs_stress\n");
  return 0;
}
