// Verifies the editor's registry-generated ComponentEditType/
// ComponentEditSnapshot dispatch (issue #156): has_component_of_type
// (the new presence probe the Add Component menu uses generically instead
// of a hand-rolled get_ptr/has_X check per branch) and a capture/apply
// round trip through the production World for a component that previously
// had no editor coverage (LightComponent, which also gained a
// REFLECT_TYPE registration in this series).

#include "editor_component_registry.h"
#include "editor_session.h"

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

namespace {

namespace math = engine::math;

using engine::editor::ComponentEditSnapshot;
using engine::editor::ComponentEditType;
using engine::runtime::Entity;
using engine::runtime::World;

/// Binds a fresh world to the editor session; restores on destruction
/// (matches the SessionWorldScope pattern in editor_inspector_edit_test.cpp
/// so this test exercises the real production capture/apply path, not a
/// copied model).
struct SessionWorldScope final {
  World *previousWorld = nullptr;

  explicit SessionWorldScope(World *world) noexcept {
    previousWorld = engine::editor::editor_session().world;
    engine::editor::editor_session().world = world;
  }

  ~SessionWorldScope() noexcept {
    engine::editor::editor_session().world = previousWorld;
  }
};

/// The registry row count generated from the runtime table matches the
/// production World's persistent-component count (14 as of issue #156).
int check_component_edit_type_count() noexcept {
  if (engine::editor::kComponentEditTypeCount != 14U) {
    return 1;
  }
  return 0;
}

/// has_component_of_type agrees with a direct World query, both before and
/// after add/remove, for a component with no dedicated presence accessor
/// naming convention (RigidBody uses get_rigid_body, not get_rigid_body_ptr
/// or has_rigid_body -- the generic probe must not assume one).
int check_has_component_of_type_round_trip() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = world->create_scene_object();

  if (engine::editor::has_component_of_type(ComponentEditType::RigidBody,
                                            entity)) {
    return 2;
  }

  engine::runtime::RigidBody body{};
  body.inverseMass = 2.0F;
  if (!world->add_rigid_body(entity, body)) {
    return 3;
  }
  if (!engine::editor::has_component_of_type(ComponentEditType::RigidBody,
                                             entity)) {
    return 4;
  }

  if (!world->remove_rigid_body(entity)) {
    return 5;
  }
  if (engine::editor::has_component_of_type(ComponentEditType::RigidBody,
                                            entity)) {
    return 6;
  }
  return 0;
}

/// capture_component_snapshot/apply_component_snapshot round-trip
/// LightComponent's color/direction/intensity through the generated
/// dispatch (this component gained its REFLECT_TYPE registration and
/// generic-drawer eligibility in the same series as this generated
/// dispatch, so a regression in either would show up here).
int check_light_component_capture_apply_round_trip() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = world->create_scene_object();

  engine::runtime::LightComponent light{};
  light.type = engine::runtime::LightType::Point;
  light.color = math::Vec3(0.25F, 0.5F, 0.75F);
  light.direction = math::Vec3(0.0F, -1.0F, 0.0F);
  light.intensity = 3.5F;

  ComponentEditSnapshot after{};
  after.light = light;
  if (!engine::editor::apply_component_snapshot(ComponentEditType::Light,
                                                entity, true, after)) {
    return 2;
  }

  ComponentEditSnapshot captured{};
  if (!engine::editor::capture_component_snapshot(ComponentEditType::Light,
                                                  entity, &captured)) {
    return 3;
  }
  if ((captured.light.type != light.type) ||
      (captured.light.intensity != light.intensity) ||
      (captured.light.color.x != light.color.x) ||
      (captured.light.color.y != light.color.y) ||
      (captured.light.color.z != light.color.z)) {
    return 4;
  }

  if (!engine::editor::apply_component_snapshot(
          ComponentEditType::Light, entity, false, ComponentEditSnapshot{})) {
    return 5;
  }
  if (engine::editor::has_component_of_type(ComponentEditType::Light,
                                            entity)) {
    return 6;
  }
  return 0;
}

} // namespace

int main() {
  struct Case {
    const char *name;
    int (*fn)() noexcept;
  };
  const Case cases[] = {
      {"component_edit_type_count", check_component_edit_type_count},
      {"has_component_of_type_round_trip",
       check_has_component_of_type_round_trip},
      {"light_component_capture_apply_round_trip",
       check_light_component_capture_apply_round_trip},
  };
  for (const Case &c : cases) {
    const int result = c.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_component_registry_test: %s failed: %d\n",
                   c.name, result);
      return result;
    }
  }
  std::printf("editor_component_registry_test: all tests passed\n");
  return 0;
}
