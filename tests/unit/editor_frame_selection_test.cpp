// Verifies Frame Selected. The framed sphere circumscribes the union of
// what each selected subtree draws and collides with: a mesh by its
// bounds under the entity's transform, a collider by its world bounds,
// anything else (a mesh not loaded yet included) by its position. The
// distance fits the sphere in the narrower field of view with a tenth to
// spare, clamped to the camera's range. Through the action table, F moves
// the orbit target to the selection and keeps the camera's angle, and is
// disabled with nothing selected.

#include "editor_frame_selection.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#include "engine/editor/editor_camera.h"
#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <cmath>
#include <memory>
#include <new>

namespace {

using engine::editor::FramingSphere;
using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::Transform;
using engine::runtime::World;

constexpr std::uint64_t kLoadedMesh = 42U;
constexpr std::uint64_t kUnloadedMesh = 7U;
// Sums and square roots of values of order 10 in float: a few ulps.
constexpr float kTol = 1.0e-5F;

/// Mesh 42 is loaded with bounds centred at (0, 1, 0), half extents 1;
/// every other mesh is still loading.
bool fake_mesh_bounds(std::uint64_t id, Vec3 *center, Vec3 *half) noexcept {
  if (id != kLoadedMesh) {
    return false;
  }
  *center = Vec3(0.0F, 1.0F, 0.0F);
  *half = Vec3(1.0F, 1.0F, 1.0F);
  return true;
}

bool near(float a, float b, float tol) noexcept {
  return std::fabs(a - b) <= tol * std::fmax(1.0F, std::fabs(b));
}

bool near(const Vec3 &a, const Vec3 &b) noexcept {
  return near(a.x, b.x, kTol) && near(a.y, b.y, kTol) && near(a.z, b.z, kTol);
}

Entity place(World &world, const Vec3 &position, Entity parent,
             float scale = 1.0F) noexcept {
  Transform transform{};
  transform.position = position;
  transform.scale = Vec3(scale, scale, scale);
  if (parent != engine::runtime::kInvalidEntity) {
    transform.parentId = world.persistent_id(parent);
  }
  return world.create_scene_object(transform);
}

void check_sphere(engine::tests::TestContext &t, World &world) noexcept {
  // A loaded mesh at (10, 0, 0), scale 2: box centre (10, 2, 0), half
  // extents 2, so the sphere's radius is 2 * sqrt(3).
  const Entity meshed = place(world, Vec3(10.0F, 0.0F, 0.0F),
                              engine::runtime::kInvalidEntity, 2.0F);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = kLoadedMesh;
  t.check(world.add_mesh_component(meshed, mesh), "add the loaded mesh");
  FramingSphere sphere{};
  t.check(engine::editor::selection_framing_sphere(world, &meshed, 1U,
                                                   &fake_mesh_bounds, &sphere),
          "a meshed entity frames");
  t.check(near(sphere.center, Vec3(10.0F, 2.0F, 0.0F)) &&
              near(sphere.radius, 2.0F * std::sqrt(3.0F), kTol),
          "a mesh frames by its bounds under the transform");

  // A parent at the origin with a child collider box around (0, 0, -6):
  // the union spans z from -7 to 0, x and y from -1 to 1.
  const Entity parent = place(world, Vec3(), engine::runtime::kInvalidEntity);
  const Entity child = place(world, Vec3(0.0F, 0.0F, -6.0F), parent);
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(1.0F, 1.0F, 1.0F);
  t.check(world.add_collider(child, collider), "add the child's collider");
  t.check(engine::editor::selection_framing_sphere(world, &parent, 1U,
                                                   &fake_mesh_bounds, &sphere),
          "a parent frames");
  t.check(near(sphere.center, Vec3(0.0F, 0.0F, -3.5F)) &&
              near(sphere.radius, std::sqrt(14.25F), kTol),
          "the subtree's collider and the parent's position are framed");

  // A mesh still loading counts as its position: the smallest sphere.
  const Entity loading =
      place(world, Vec3(3.0F, 4.0F, 5.0F), engine::runtime::kInvalidEntity);
  mesh.meshAssetId = kUnloadedMesh;
  t.check(world.add_mesh_component(loading, mesh), "add the loading mesh");
  t.check(engine::editor::selection_framing_sphere(world, &loading, 1U,
                                                   &fake_mesh_bounds, &sphere),
          "a loading mesh frames");
  t.check(near(sphere.center, Vec3(3.0F, 4.0F, 5.0F)) &&
              (sphere.radius == engine::editor::kFramePointRadius),
          "a mesh not loaded yet frames as its position");

  // Two bare positions ten apart: centre between them, radius five.
  const Entity a =
      place(world, Vec3(20.0F, 0.0F, 0.0F), engine::runtime::kInvalidEntity);
  const Entity b =
      place(world, Vec3(30.0F, 0.0F, 0.0F), engine::runtime::kInvalidEntity);
  const Entity pair[2] = {a, b};
  t.check(engine::editor::selection_framing_sphere(
              world, pair, 2U, &fake_mesh_bounds, &sphere) &&
              near(sphere.center, Vec3(25.0F, 0.0F, 0.0F)) &&
              near(sphere.radius, 5.0F, kTol),
          "a selection frames the union of its members");

  const Entity gone = place(world, Vec3(), engine::runtime::kInvalidEntity);
  t.check(world.destroy_entity(gone), "destroy an entity");
  sphere.radius = -1.0F;
  t.check(!engine::editor::selection_framing_sphere(
              world, &gone, 1U, &fake_mesh_bounds, &sphere) &&
              (sphere.radius == -1.0F),
          "nothing alive is refused, the output untouched");
}

void check_distance(engine::tests::TestContext &t) noexcept {
  constexpr float kFov = 1.0471975512F; // 60 degrees
  // Landscape: the vertical half angle, 30 degrees, is narrower.
  t.check(near(engine::editor::framing_distance(1.0F, kFov, 16.0F / 9.0F), 2.2F,
               kTol),
          "landscape fits the vertical field: 1.1 / sin 30");
  // Portrait at 1:2: the horizontal half angle is atan(tan 30 / 2).
  const float halfH = std::atan(std::tan(kFov * 0.5F) * 0.5F);
  t.check(near(engine::editor::framing_distance(1.0F, kFov, 0.5F),
               1.1F / std::sin(halfH), kTol),
          "portrait fits the horizontal field");
  t.check(engine::editor::framing_distance(1.0e4F, kFov, 1.0F) ==
              engine::editor::EditorCamera::kMaxDistance,
          "a huge selection clamps to the farthest orbit");
  t.check(engine::editor::framing_distance(0.01F, kFov, 1.0F) ==
              engine::editor::EditorCamera::kMinDistance,
          "a tiny one clamps to the nearest");
}

void check_action(engine::tests::TestContext &t, World &world) noexcept {
  engine::editor::EditorSession &session = engine::editor::editor_session();
  engine::editor::clear_entity_selection();
  t.check(!engine::editor::editor_action_enabled(
              engine::editor::EditorAction::FrameSelected),
          "Frame Selected is off with nothing selected");
  const Entity target =
      place(world, Vec3(-4.0F, 2.0F, 8.0F), engine::runtime::kInvalidEntity);
  engine::editor::select_entity(target, false);
  session.editorCamera.yaw = 0.3F;
  session.editorCamera.pitch = 0.2F;
  session.sceneViewPixelWidth = 0;
  session.sceneViewPixelHeight = 0;
  t.check(engine::editor::run_editor_action(
              engine::editor::EditorAction::FrameSelected),
          "F frames the selection");
  const float fov =
      engine::editor::editor_camera_state(session.editorCamera).fovRadians;
  t.check(near(session.editorCamera.target, Vec3(-4.0F, 2.0F, 8.0F)) &&
              (session.editorCamera.distance ==
               engine::editor::framing_distance(
                   engine::editor::kFramePointRadius, fov, 1.0F)),
          "the orbit centres on the selection at the fitted distance");
  t.check((session.editorCamera.yaw == 0.3F) &&
              (session.editorCamera.pitch == 0.2F),
          "the camera keeps its angle");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 99;
  }
  engine::editor::EditorSession &session = engine::editor::editor_session();
  World *const previous = session.world;
  session.world = world.get();
  check_sphere(t, *world);
  check_distance(t);
  check_action(t, *world);
  engine::editor::clear_entity_selection();
  session.world = previous;
  return t.finish("editor_frame_selection");
}
