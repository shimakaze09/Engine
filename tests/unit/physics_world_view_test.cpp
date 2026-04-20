#include "engine/physics/physics_world_view.h"

#include "engine/math/vec3.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_types.h"

#include <cstddef>
#include <cstdint>
#include <new>

namespace {

using engine::physics::Collider;
using engine::physics::Entity;
using engine::physics::MovementAuthority;
using engine::physics::PhysicsContext;
using engine::physics::PhysicsWorldView;
using engine::physics::RigidBody;
using engine::physics::Transform;

// Minimal concrete implementation to verify the interface compiles and links.
class StubPhysicsWorld final : public PhysicsWorldView {
public:
  std::size_t transform_count() const noexcept override { return 0U; }

  bool get_transform(Entity /*entity*/,
                     Transform * /*outTransform*/) const noexcept override {
    return false;
  }

  bool get_transform_update_range(
      std::size_t /*startIndex*/, std::size_t /*count*/,
      const Entity ** /*outEntities*/, const Transform ** /*outReadTransforms*/,
      Transform ** /*outWriteTransforms*/) noexcept override {
    return false;
  }

  SimulationAccessToken simulation_access_token() const noexcept override {
    return make_token(m_inSimulation);
  }

  Transform *get_transform_write_ptr(
      Entity /*entity*/,
      const SimulationAccessToken & /*token*/) noexcept override {
    return nullptr;
  }

  std::size_t collider_count() const noexcept override { return 0U; }

  bool get_collider_range(
      std::size_t /*startIndex*/, std::size_t /*count*/,
      const Entity ** /*outEntities*/,
      const Collider ** /*outColliders*/) const noexcept override {
    return false;
  }

  const Collider *get_collider_ptr(Entity /*entity*/) const noexcept override {
    return nullptr;
  }

  RigidBody *get_rigid_body_ptr(Entity /*entity*/) noexcept override {
    return nullptr;
  }

  const RigidBody *
  get_rigid_body_ptr(Entity /*entity*/) const noexcept override {
    return nullptr;
  }

  bool get_rigid_body(Entity /*entity*/,
                      RigidBody * /*outRigidBody*/) const noexcept override {
    return false;
  }

  MovementAuthority
  movement_authority(Entity /*entity*/) const noexcept override {
    return MovementAuthority::None;
  }

  PhysicsContext &physics_context() noexcept override { return m_ctx; }
  const PhysicsContext &physics_context() const noexcept override {
    return m_ctx;
  }

  bool m_inSimulation = false;

private:
  PhysicsContext m_ctx{};
};

// --- Tests ------------------------------------------------------------------

int check_interface_instantiation() {
  StubPhysicsWorld stub;
  PhysicsWorldView &view = stub;

  if (view.transform_count() != 0U) {
    return 1;
  }
  if (view.collider_count() != 0U) {
    return 2;
  }
  return 0;
}

int check_simulation_token_invalid() {
  StubPhysicsWorld stub;
  stub.m_inSimulation = false;

  const auto token = stub.simulation_access_token();
  if (token.valid()) {
    return 1;
  }
  return 0;
}

int check_simulation_token_valid() {
  StubPhysicsWorld stub;
  stub.m_inSimulation = true;

  const auto token = stub.simulation_access_token();
  if (!token.valid()) {
    return 1;
  }
  return 0;
}

int check_physics_context_accessible() {
  StubPhysicsWorld stub;
  PhysicsWorldView &view = stub;

  PhysicsContext &ctx = view.physics_context();
  // Default gravity is (0, -9.8, 0).
  if (ctx.gravity.y > -9.7F || ctx.gravity.y < -9.9F) {
    return 1;
  }

  ctx.gravity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  if (view.physics_context().gravity.y != 0.0F) {
    return 2;
  }
  return 0;
}

using TestFn = int (*)();

struct TestEntry {
  const char *name;
  TestFn fn;
};

const TestEntry g_tests[] = {
    {"interface_instantiation", check_interface_instantiation},
    {"simulation_token_invalid", check_simulation_token_invalid},
    {"simulation_token_valid", check_simulation_token_valid},
    {"physics_context_accessible", check_physics_context_accessible},
};

} // namespace

int main() {
  int failures = 0;
  for (const auto &test : g_tests) {
    const int result = test.fn();
    if (result != 0) {
      ++failures;
    }
  }
  return failures;
}
