#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr std::size_t kEntityCount = 128U;
constexpr int kStepCount = 240;
constexpr float kStepSeconds = 1.0F / 60.0F;
constexpr std::uint64_t kExpectedHash = 0ULL;

bool populate_world(engine::runtime::World *world) noexcept {
  if (world == nullptr) {
    return false;
  }

  for (std::size_t i = 0U; i < kEntityCount; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }

    engine::runtime::Transform transform{};
    transform.position =
        engine::math::Vec3(static_cast<float>(i) * 0.1F, 8.0F, 0.5F);
    if (!world->add_transform(entity, transform)) {
      return false;
    }

    engine::runtime::RigidBody body{};
    body.inverseMass = 1.0F;
    body.velocity = engine::math::Vec3(0.1F, 0.0F, 0.05F);
    body.angularVelocity = engine::math::Vec3(0.0F, 0.01F, 0.0F);
    if (!world->add_rigid_body(entity, body)) {
      return false;
    }
  }

  return true;
}

bool run_sim(std::uint64_t *outHash) noexcept {
  if (outHash == nullptr) {
    return false;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }

  if (!populate_world(world.get())) {
    return false;
  }

  for (int step = 0; step < kStepCount; ++step) {
    world->begin_update_phase();
    engine::runtime::step_physics(*world, kStepSeconds);
    world->end_frame_phase();
  }

  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 1U; i <= kEntityCount; ++i) {
    const engine::runtime::Entity entity =
        world->find_entity_by_index(static_cast<std::uint32_t>(i));
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }

    engine::runtime::Transform transform{};
    if (!world->get_transform(entity, &transform)) {
      return false;
    }

    const engine::runtime::RigidBody *body = world->get_rigid_body_ptr(entity);
    if (body == nullptr) {
      return false;
    }

    std::uint32_t xBits = 0U;
    std::uint32_t yBits = 0U;
    std::uint32_t zBits = 0U;
    std::uint32_t vxBits = 0U;
    std::uint32_t vyBits = 0U;
    std::uint32_t vzBits = 0U;

    std::memcpy(&xBits, &transform.position.x, sizeof(xBits));
    std::memcpy(&yBits, &transform.position.y, sizeof(yBits));
    std::memcpy(&zBits, &transform.position.z, sizeof(zBits));
    std::memcpy(&vxBits, &body->velocity.x, sizeof(vxBits));
    std::memcpy(&vyBits, &body->velocity.y, sizeof(vyBits));
    std::memcpy(&vzBits, &body->velocity.z, sizeof(vzBits));

    hash ^= static_cast<std::uint64_t>(entity.index);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(xBits);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(yBits);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(zBits);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(vxBits);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(vyBits);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(vzBits);
    hash *= 1099511628211ULL;
  }

  *outHash = hash;
  return true;
}

} // namespace

int main() {
  std::uint64_t hashA = 0U;
  std::uint64_t hashB = 0U;

  if (!run_sim(&hashA) || !run_sim(&hashB)) {
    std::printf("FAIL: determinism simulation setup\n");
    return 1;
  }

  if (hashA != hashB) {
    std::printf("FAIL: deterministic replay mismatch a=%llu b=%llu\n",
                static_cast<unsigned long long>(hashA),
                static_cast<unsigned long long>(hashB));
    return 2;
  }

  std::printf("[determinism] hash=%llu\n",
              static_cast<unsigned long long>(hashA));

  if ((kExpectedHash != 0ULL) && (hashA != kExpectedHash)) {
    std::printf(
        "FAIL: cross-platform hash mismatch expected=%llu actual=%llu\n",
        static_cast<unsigned long long>(kExpectedHash),
        static_cast<unsigned long long>(hashA));
    return 3;
  }

  return 0;
}
