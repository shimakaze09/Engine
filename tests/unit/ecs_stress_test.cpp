// ECS stress / performance benchmark test.
// Exercises entity creation, transform addition, and flat iteration at the
// maximum World capacity.  The test is registered as a CTest gate: if the
// iteration loop exceeds the threshold the test fails, providing a regression
// signal for hot-path regressions.
//
// World capacity is capped at 16384 entities (see world.h).  The "50k+"
// phased-todo item requires raising kMaxEntities and verifying the heap
// allocation path still holds; that cap raise is tracked separately.

#include <chrono>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

using namespace engine::runtime;
using Clock = std::chrono::high_resolution_clock;

namespace {

// Generous threshold: the tight inner loop must complete in under 500 ms even
// on a slow CI host.  A real performance gate (< 2 ms) is enforced separately
// via profiler benchmarks.
constexpr double kThresholdMs = 500.0;

bool bench_create_and_iterate() noexcept {
  auto world = std::unique_ptr<World>(new (std::nothrow) World());
  if (!world) {
    return false;
  }

  const std::size_t cap = World::kMaxEntities;

  // ---- creation ----
  const auto t0 = Clock::now();
  for (std::size_t i = 0U; i < cap; ++i) {
    const Entity e = world->create_entity();
    if (e == kInvalidEntity) {
      // World full – acceptable for the stress test
      break;
    }
    Transform t{};
    t.position = engine::math::Vec3(static_cast<float>(i), 0.0F, 0.0F);
    world->add_transform(e, t);
  }
  const auto t1 = Clock::now();

  // ---- flat iteration (visitor over all transforms) ----
  std::size_t visited = 0U;
  auto visitor = [](Entity /*e*/, const Transform & /*t*/, void *ud) noexcept {
    auto *count = static_cast<std::size_t *>(ud);
    ++(*count);
  };
  world->for_each_transform(visitor, &visited);

  const auto t2 = Clock::now();

  const double createMs =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double iterMs =
      std::chrono::duration<double, std::milli>(t2 - t1).count();

  std::printf("[ecs_stress] entities=%zu  create=%.2fms  iterate=%.2fms\n",
              visited, createMs, iterMs);

  const double totalMs = createMs + iterMs;
  if (totalMs > kThresholdMs) {
    std::printf("BENCH FAIL: total %.2fms exceeded threshold %.0fms\n", totalMs,
                kThresholdMs);
    return false;
  }
  return true;
}

} // namespace

int main() {
  if (!bench_create_and_iterate()) {
    return 1;
  }
  std::printf("PASS: ecs_stress\n");
  return 0;
}
