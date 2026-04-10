// Determinism baseline test.
// Verifies that running the physics step twice with identical initial
// conditions produces identical entity positions.  This is the minimum
// required "deterministic test replay baseline" for P1-M1.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

using namespace engine::runtime;

namespace {

constexpr float kDt = 1.0F / 60.0F;
constexpr float kEpsilon = 1e-5F;
constexpr int kStepCount = 10;

struct SimSnapshot final {
  engine::math::Vec3 position{};
  engine::math::Vec3 velocity{};
};

bool snapshots_equal(const SimSnapshot &a, const SimSnapshot &b) noexcept {
  const auto veq = [](float x, float y) noexcept {
    return std::fabs(x - y) <= kEpsilon;
  };
  return veq(a.position.x, b.position.x) && veq(a.position.y, b.position.y) &&
         veq(a.position.z, b.position.z) && veq(a.velocity.x, b.velocity.x) &&
         veq(a.velocity.y, b.velocity.y) && veq(a.velocity.z, b.velocity.z);
}

// Build and step a world; fill snapshot array.  Returns entity count stepped.
int run_sim(SimSnapshot *out, int maxEntities) noexcept {
  auto world = std::unique_ptr<World>(new (std::nothrow) World());
  if (!world) {
    return 0;
  }

  world->end_frame_phase();

  // Spawn 8 dynamic bodies with deterministic initial conditions.
  constexpr int kCount = 8;
  if (kCount > maxEntities) {
    return 0;
  }

  Entity entities[kCount] = {};
  for (int i = 0; i < kCount; ++i) {
    entities[i] = world->create_entity();
    if (entities[i] == kInvalidEntity) {
      return 0;
    }

    Transform t{};
    t.position = engine::math::Vec3(static_cast<float>(i), 5.0F, 0.0F);
    world->add_transform(entities[i], t);

    RigidBody rb{};
    rb.inverseMass = 1.0F;
    world->add_rigid_body(entities[i], rb);
  }

  // Step
  for (int s = 0; s < kStepCount; ++s) {
    world->begin_update_phase();
    step_physics(*world, kDt);
    world->end_frame_phase();
  }

  // Snapshot
  for (int i = 0; i < kCount; ++i) {
    Transform t{};
    world->get_transform(entities[i], &t);
    out[i].position = t.position;

    const RigidBody *rb = world->get_rigid_body_ptr(entities[i]);
    out[i].velocity = (rb != nullptr) ? rb->velocity : engine::math::Vec3{};
  }
  return kCount;
}

bool test_deterministic_replay() noexcept {
  constexpr int kMax = 16;
  SimSnapshot runA[kMax] = {};
  SimSnapshot runB[kMax] = {};

  const int countA = run_sim(runA, kMax);
  const int countB = run_sim(runB, kMax);

  if ((countA == 0) || (countA != countB)) {
    std::printf("FAIL: sim did not produce expected entity count\n");
    return false;
  }

  for (int i = 0; i < countA; ++i) {
    if (!snapshots_equal(runA[i], runB[i])) {
      std::printf("FAIL: entity %d non-deterministic: "
                  "runA pos=(%.6f,%.6f,%.6f)  runB pos=(%.6f,%.6f,%.6f)\n",
                  i, static_cast<double>(runA[i].position.x),
                  static_cast<double>(runA[i].position.y),
                  static_cast<double>(runA[i].position.z),
                  static_cast<double>(runB[i].position.x),
                  static_cast<double>(runB[i].position.y),
                  static_cast<double>(runB[i].position.z));
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  if (!test_deterministic_replay()) {
    return 1;
  }
  std::printf("PASS: determinism_baseline\n");
  return 0;
}
