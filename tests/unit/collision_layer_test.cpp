#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

// Test: two entities on the same layer with matching masks SHOULD collide.
int test_same_layer_collides() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto a = world->create_entity();
  const auto b = world->create_entity();
  if ((a == engine::runtime::kInvalidEntity) ||
      (b == engine::runtime::kInvalidEntity)) {
    return 2;
  }

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.5F, 0.0F, 0.0F); // overlapping

  engine::runtime::Collider col{};
  col.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  col.collisionLayer = 1U;
  col.collisionMask = 0xFFFFFFFFU;

  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;

  if (!world->add_transform(a, tA) || !world->add_transform(b, tB)) {
    return 3;
  }
  if (!world->add_collider(a, col) || !world->add_collider(b, col)) {
    return 4;
  }
  if (!world->add_rigid_body(a, rb) || !world->add_rigid_body(b, rb)) {
    return 5;
  }

  world->begin_update_phase();
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updatedA{};
  engine::runtime::Transform updatedB{};
  world->get_transform(a, &updatedA);
  world->get_transform(b, &updatedB);

  // They should have been pushed apart.
  const float dist = std::fabs(updatedB.position.x - updatedA.position.x);
  if (dist <= 0.5F) {
    std::printf("FAIL: same layer — dist %.4f should be > 0.5\n", dist);
    return 6;
  }

  return 0;
}

// Test: entity on layer 2, other has mask excluding layer 2 → NO collision.
int test_layer_mask_excludes() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 10;
  }
  world->end_frame_phase();

  const auto a = world->create_entity();
  const auto b = world->create_entity();
  if ((a == engine::runtime::kInvalidEntity) ||
      (b == engine::runtime::kInvalidEntity)) {
    return 11;
  }

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.5F, 0.0F, 0.0F); // overlapping

  engine::runtime::Collider colA{};
  colA.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  colA.collisionLayer = 2U; // layer bit 1
  colA.collisionMask = 0xFFFFFFFFU;

  engine::runtime::Collider colB{};
  colB.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  colB.collisionLayer = 1U;
  colB.collisionMask = ~2U; // excludes layer 2

  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;

  if (!world->add_transform(a, tA) || !world->add_transform(b, tB)) {
    return 12;
  }
  if (!world->add_collider(a, colA) || !world->add_collider(b, colB)) {
    return 13;
  }
  if (!world->add_rigid_body(a, rb) || !world->add_rigid_body(b, rb)) {
    return 14;
  }

  world->begin_update_phase();
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updatedA{};
  engine::runtime::Transform updatedB{};
  world->get_transform(a, &updatedA);
  world->get_transform(b, &updatedB);

  // They should NOT have been pushed apart (positions unchanged).
  const float dx = std::fabs(updatedA.position.x - tA.position.x);
  const float dy = std::fabs(updatedB.position.x - tB.position.x);
  if (dx > 0.001F || dy > 0.001F) {
    std::printf("FAIL: layer mask excludes — dA=%.4f dB=%.4f should be ~0\n",
                dx, dy);
    return 15;
  }

  return 0;
}

// Test: bidirectional mask — A masks out B's layer AND B masks out A's layer.
int test_both_masks_needed() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }
  world->end_frame_phase();

  const auto a = world->create_entity();
  const auto b = world->create_entity();

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.5F, 0.0F, 0.0F);

  // A is on layer 4, B is on layer 8.
  // A's mask includes layer 8, B's mask includes layer 4.
  engine::runtime::Collider colA{};
  colA.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  colA.collisionLayer = 4U;
  colA.collisionMask = 8U; // only collides with layer 8

  engine::runtime::Collider colB{};
  colB.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  colB.collisionLayer = 8U;
  colB.collisionMask = 4U; // only collides with layer 4

  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;

  world->add_transform(a, tA);
  world->add_transform(b, tB);
  world->add_collider(a, colA);
  world->add_collider(b, colB);
  world->add_rigid_body(a, rb);
  world->add_rigid_body(b, rb);

  world->begin_update_phase();
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updatedA{};
  engine::runtime::Transform updatedB{};
  world->get_transform(a, &updatedA);
  world->get_transform(b, &updatedB);

  // Should collide (both masks match).
  const float dist = std::fabs(updatedB.position.x - updatedA.position.x);
  if (dist <= 0.5F) {
    std::printf("FAIL: both masks match — dist %.4f should be > 0.5\n", dist);
    return 21;
  }

  return 0;
}

// Test: one-sided mask failure → no collision.
int test_one_sided_mask_fails() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 30;
  }
  world->end_frame_phase();

  const auto a = world->create_entity();
  const auto b = world->create_entity();

  engine::runtime::Transform tA{};
  tA.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform tB{};
  tB.position = engine::math::Vec3(0.5F, 0.0F, 0.0F);

  engine::runtime::Collider colA{};
  colA.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  colA.collisionLayer = 4U;
  colA.collisionMask = 8U; // wants to collide with 8

  engine::runtime::Collider colB{};
  colB.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  colB.collisionLayer = 8U;
  colB.collisionMask = 0U; // collides with nothing

  engine::runtime::RigidBody rb{};
  rb.inverseMass = 1.0F;

  world->add_transform(a, tA);
  world->add_transform(b, tB);
  world->add_collider(a, colA);
  world->add_collider(b, colB);
  world->add_rigid_body(a, rb);
  world->add_rigid_body(b, rb);

  world->begin_update_phase();
  engine::runtime::resolve_collisions(*world);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();

  engine::runtime::Transform updatedA{};
  engine::runtime::Transform updatedB{};
  world->get_transform(a, &updatedA);
  world->get_transform(b, &updatedB);

  // Should NOT collide (B's mask doesn't include A's layer).
  const float dx = std::fabs(updatedA.position.x - tA.position.x);
  const float dy = std::fabs(updatedB.position.x - tB.position.x);
  if (dx > 0.001F || dy > 0.001F) {
    std::printf("FAIL: one-sided mask — dA=%.4f dB=%.4f should be ~0\n", dx,
                dy);
    return 31;
  }

  return 0;
}

} // namespace

int main() {
  int failed = 0;

  auto run = [&](const char *name, int (*fn)() noexcept) {
    const int result = fn();
    if (result != 0) {
      std::printf("FAIL: %s (code %d)\n", name, result);
      ++failed;
    } else {
      std::printf("PASS: %s\n", name);
    }
  };

  run("same_layer_collides", &test_same_layer_collides);
  run("layer_mask_excludes", &test_layer_mask_excludes);
  run("both_masks_needed", &test_both_masks_needed);
  run("one_sided_mask_fails", &test_one_sided_mask_fails);

  std::printf("\n%d test(s) failed\n", failed);
  return failed;
}
