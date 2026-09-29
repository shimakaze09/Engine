// Verifies the Scene view's click picking. A ray hits meshes by their
// bounds under the entity's transform and colliders by their exact shape,
// nearest first with one hit per entity; a mesh box or a collider the ray
// starts inside is skipped, so a room around the camera does not swallow
// clicks; a mesh still loading is not pickable. A repeated click on the
// same spot walks to the next hit behind the current pick, wrapping, and
// any other click takes the nearest. A marquee takes every mesh and
// collider its sub-rectangle frustum does not exclude. Lights and cameras
// are screen icons, picked first and taken by a marquee by position. A
// right-click creates where its ray meets the ground within reach, and a
// press is a click until it moves beyond the slop.

#include "editor_scene_query.h"

#include "engine/math/frustum.h"
#include "engine/math/ray.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

namespace {

using engine::editor::PickHit;
using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;

constexpr std::uint64_t kUnitMesh = 1U;
constexpr std::uint64_t kLoadingMesh = 2U;

/// Mesh 1 is a unit cube (half extents 0.5) about its origin; every other
/// mesh is still loading.
bool fake_mesh_bounds(std::uint64_t id, Vec3 *center, Vec3 *half) noexcept {
  if (id != kUnitMesh) {
    return false;
  }
  *center = Vec3();
  *half = Vec3(0.5F, 0.5F, 0.5F);
  return true;
}

Entity place(World &world, const Vec3 &position, float scale = 1.0F) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  transform.scale = Vec3(scale, scale, scale);
  return world.create_scene_object(transform);
}

bool add_mesh(World &world, Entity entity, std::uint64_t id) noexcept {
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = id;
  return world.add_mesh_component(entity, mesh);
}

bool add_box_collider(World &world, Entity entity, float half) noexcept {
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(half, half, half);
  return world.add_collider(entity, collider);
}

// Slab hits on unit-cube faces at distances of order 10: exact in float
// for these axis-aligned rays, but held to a few ulps.
bool near(float a, float b) noexcept { return std::fabs(a - b) <= 1.0e-5F; }

void check_hits(engine::tests::TestContext &t, World &world) noexcept {
  // Down -z from the origin: a mesh cube at depth 5 (front face 4.5), a
  // collider box of half 1 at depth 10 (front face 9), and one entity with
  // both a mesh (front face 14.5) and a larger collider (front face 13)
  // at depth 15.
  const Entity nearMesh = place(world, Vec3(0.0F, 0.0F, -5.0F));
  const Entity midCollider = place(world, Vec3(0.0F, 0.0F, -10.0F));
  const Entity both = place(world, Vec3(0.0F, 0.0F, -15.0F));
  t.check(add_mesh(world, nearMesh, kUnitMesh) &&
              add_box_collider(world, midCollider, 1.0F) &&
              add_mesh(world, both, kUnitMesh) &&
              add_box_collider(world, both, 2.0F),
          "build the row of targets");
  // A room around the camera, both as a mesh and as a collider, and a
  // mesh still loading in the line of sight.
  const Entity roomMesh = place(world, Vec3(), 100.0F);
  const Entity roomCollider = place(world, Vec3());
  const Entity loading = place(world, Vec3(0.0F, 0.0F, -2.0F));
  t.check(add_mesh(world, roomMesh, kUnitMesh) &&
              add_box_collider(world, roomCollider, 50.0F) &&
              add_mesh(world, loading, kLoadingMesh),
          "build the room and the loading mesh");

  engine::math::Ray ray{};
  ray.origin = Vec3();
  ray.direction = Vec3(0.0F, 0.0F, -1.0F);
  std::array<PickHit, 8> hits{};
  const std::size_t count = engine::editor::scene_pick_hits(
      world, ray, 100.0F, &fake_mesh_bounds, hits.data(), hits.size());
  t.check(count == 3U, "three pickable things lie on the ray");
  t.check((hits[0].entity == nearMesh) && near(hits[0].distance, 4.5F),
          "the nearest is the mesh, at its bounds' face");
  t.check((hits[1].entity == midCollider) && near(hits[1].distance, 9.0F),
          "then the collider, at its shape's face");
  t.check((hits[2].entity == both) && near(hits[2].distance, 13.0F),
          "an entity with both is listed once, at its nearer hit");

  std::array<PickHit, 2> two{};
  t.check(
      (engine::editor::scene_pick_hits(world, ray, 100.0F, &fake_mesh_bounds,
                                       two.data(), two.size()) == 2U) &&
          (two[0].entity == nearMesh) && (two[1].entity == midCollider),
      "a short list keeps the nearest");
  t.check(engine::editor::scene_pick_hits(world, ray, 4.0F, &fake_mesh_bounds,
                                          hits.data(), hits.size()) == 0U,
          "nothing within reach, nothing hit");
  ray.direction = Vec3(0.0F, 1.0F, 0.0F);
  t.check(engine::editor::scene_pick_hits(world, ray, 100.0F, &fake_mesh_bounds,
                                          hits.data(), hits.size()) == 0U,
          "a ray into empty space hits nothing but the room it starts in");
}

void check_cycle(engine::tests::TestContext &t) noexcept {
  const PickHit hits[3] = {
      {Entity{1U, 1U}, 1.0F}, {Entity{2U, 1U}, 2.0F}, {Entity{3U, 1U}, 3.0F}};
  using engine::editor::choose_pick;
  t.check(choose_pick(hits, 3U, kInvalidEntity, false) == hits[0].entity,
          "a click takes the nearest");
  t.check(choose_pick(hits, 3U, hits[0].entity, true) == hits[1].entity,
          "a repeat click on the same spot takes the next behind");
  t.check(choose_pick(hits, 3U, hits[2].entity, true) == hits[0].entity,
          "past the farthest it wraps to the nearest");
  t.check(choose_pick(hits, 3U, hits[1].entity, false) == hits[0].entity,
          "a click elsewhere takes the nearest again");
  t.check(choose_pick(hits, 3U, Entity{9U, 1U}, true) == hits[0].entity,
          "a pick that is not under the cursor starts from the nearest");
  t.check(choose_pick(hits, 0U, kInvalidEntity, true) == kInvalidEntity,
          "no hits, no pick");
}

/// Collects what a marquee visits, in order.
struct Visited final {
  std::array<Entity, 16> entities{};
  std::size_t count = 0U;
};

void record(void *context, Entity entity) noexcept {
  auto *visited = static_cast<Visited *>(context);
  if (visited->count < visited->entities.size()) {
    visited->entities[visited->count++] = entity;
  }
}

bool visited_has(const Visited &visited, Entity entity) noexcept {
  for (std::size_t i = 0U; i < visited.count; ++i) {
    if (visited.entities[i] == entity) {
      return true;
    }
  }
  return false;
}

/// A marquee over the right half of a 90 degree view down -z takes what
/// its frustum does not exclude: a mesh or a collider right of the centre
/// line, and one straddling it, but not one left of it, one behind the
/// camera, or a mesh still loading.
void check_box_select(engine::tests::TestContext &t, World &world) noexcept {
  const Entity rightMesh = place(world, Vec3(5.0F, 0.0F, -10.0F));
  const Entity leftMesh = place(world, Vec3(-5.0F, 0.0F, -10.0F));
  const Entity rightCollider = place(world, Vec3(3.0F, 0.0F, -20.0F));
  const Entity straddling = place(world, Vec3(0.0F, 0.0F, -10.0F));
  const Entity behind = place(world, Vec3(5.0F, 0.0F, 10.0F));
  const Entity loading = place(world, Vec3(6.0F, 0.0F, -10.0F));
  t.check(add_mesh(world, rightMesh, kUnitMesh) &&
              add_mesh(world, leftMesh, kUnitMesh) &&
              add_box_collider(world, rightCollider, 1.0F) &&
              add_mesh(world, straddling, kUnitMesh) &&
              add_mesh(world, behind, kUnitMesh) &&
              add_mesh(world, loading, kLoadingMesh),
          "build the marquee scene");
  const engine::math::Mat4 projection =
      engine::math::perspective(1.57079632679F, 1.0F, 1.0F, 100.0F);
  const engine::math::Frustum rightHalf =
      engine::math::frustum_from_view_projection(
          engine::math::sub_rect_projection(projection, 0.0F, -1.0F, 1.0F,
                                            1.0F),
          false);
  Visited visited{};
  const std::size_t count = engine::editor::scene_box_select(
      world, rightHalf, &fake_mesh_bounds, &record, &visited);
  t.check((count == 3U) && (visited.count == 3U), "the marquee takes three");
  t.check(visited_has(visited, rightMesh) &&
              visited_has(visited, rightCollider) &&
              visited_has(visited, straddling),
          "a mesh and a collider inside, and a mesh straddling its edge");
  t.check(!visited_has(visited, leftMesh) && !visited_has(visited, behind) &&
              !visited_has(visited, loading),
          "not what lies beside it or behind the camera, nor a loading mesh");
  t.check(engine::editor::scene_box_select(world, rightHalf, &fake_mesh_bounds,
                                           nullptr, nullptr) == 3U,
          "a count needs no visitor");
}

/// Lights and cameras are icons where they project: in front of the eye
/// and within the view only. The icon nearest the cursor within its
/// radius is picked, the nearer in depth on a tie, and a marquee takes an
/// icon by its position.
void check_icons(engine::tests::TestContext &t, World &world) noexcept {
  const Entity point = place(world, Vec3(0.0F, 0.0F, -10.0F));
  const Entity spot = place(world, Vec3(5.0F, 0.0F, -10.0F));
  const Entity camera = place(world, Vec3(-5.0F, 0.0F, -10.0F));
  const Entity behind = place(world, Vec3(0.0F, 0.0F, 10.0F));
  const Entity aside = place(world, Vec3(20.0F, 0.0F, -10.0F));
  const Entity deeper = place(world, Vec3(0.0F, 0.0F, -20.0F));
  const Entity meshOnly = place(world, Vec3(1.0F, 0.0F, -10.0F));
  engine::runtime::PointLightComponent pointLight{};
  engine::runtime::SpotLightComponent spotLight{};
  engine::runtime::CameraComponent cameraComponent{};
  t.check(world.add_point_light_component(point, pointLight) &&
              world.add_spot_light_component(spot, spotLight) &&
              world.add_camera_component(camera, cameraComponent) &&
              world.add_point_light_component(behind, pointLight) &&
              world.add_point_light_component(aside, pointLight) &&
              world.add_point_light_component(deeper, pointLight) &&
              add_mesh(world, meshOnly, kUnitMesh),
          "build the icon scene");
  t.check(engine::editor::entity_has_icon(world, camera) &&
              !engine::editor::entity_has_icon(world, meshOnly),
          "lights and cameras have icons, meshes do not");

  // 90 degrees down -z: x = 5 at depth 10 projects to x = 0.5.
  const engine::math::Mat4 viewProjection =
      engine::math::perspective(1.57079632679F, 1.0F, 1.0F, 100.0F);
  std::array<engine::editor::SceneIcon, 8> icons{};
  const std::size_t count = engine::editor::scene_icons(
      world, viewProjection, icons.data(), icons.size());
  t.check(count == 4U, "four icons are in view");
  bool placed = true;
  for (std::size_t i = 0U; i < count; ++i) {
    const engine::editor::SceneIcon &icon = icons[i];
    if (icon.entity == spot) {
      placed = placed && near(icon.ndc.x, 0.5F) && near(icon.ndc.y, 0.0F) &&
               (icon.kind == engine::editor::SceneIconKind::Light);
    } else if (icon.entity == camera) {
      placed = placed && near(icon.ndc.x, -0.5F) &&
               (icon.kind == engine::editor::SceneIconKind::Camera);
    } else {
      placed = placed && ((icon.entity == point) || (icon.entity == deeper));
    }
  }
  t.check(placed, "each icon sits where its entity projects");

  using engine::editor::pick_icon;
  t.check(pick_icon(icons.data(), count, 0.52F, 0.0F, 0.05F, 0.05F) == spot,
          "an icon is picked within its radius");
  t.check(pick_icon(icons.data(), count, 0.6F, 0.0F, 0.05F, 0.05F) ==
              kInvalidEntity,
          "nothing is picked outside it");
  t.check(pick_icon(icons.data(), count, 0.0F, 0.0F, 0.05F, 0.05F) == point,
          "of two icons on one spot, the nearer is picked");

  Visited visited{};
  const engine::math::Frustum rightHalf =
      engine::math::frustum_from_view_projection(
          engine::math::sub_rect_projection(viewProjection, 0.0F, -1.0F, 1.0F,
                                            1.0F),
          false);
  static_cast<void>(engine::editor::scene_box_select(
      world, rightHalf, &fake_mesh_bounds, &record, &visited));
  t.check(visited_has(visited, spot) && !visited_has(visited, camera) &&
              !visited_has(visited, aside),
          "a marquee takes the icons within it");
}

/// Icons are 32 px across at scale 1, grow with the UI scale and the
/// icon-size preference, stay within the preference's range, and are
/// picked within exactly the radius they are drawn in.
void check_icon_metrics(engine::tests::TestContext &t) noexcept {
  using engine::editor::scene_icon_metrics;
  using engine::editor::SceneIconMetrics;
  const SceneIconMetrics base = scene_icon_metrics(1.0F, 1.0F);
  t.check((base.radius == 16.0F) && (base.stroke == 1.5F) &&
              (base.selectionRadius == 19.0F),
          "an icon is 32 px across at scale 1, ringed just outside it");
  const SceneIconMetrics hiDpi = scene_icon_metrics(2.0F, 1.0F);
  t.check((hiDpi.radius == 32.0F) && (hiDpi.stroke == 3.0F),
          "the UI scale scales the icon");
  const SceneIconMetrics larger = scene_icon_metrics(1.0F, 1.5F);
  t.check(larger.radius == 24.0F, "the preference scales the icon");
  t.check((scene_icon_metrics(1.0F, 10.0F).radius == 48.0F) &&
              (scene_icon_metrics(1.0F, 0.1F).radius == 8.0F),
          "the preference is clamped to 0.5x..3x");
  t.check((scene_icon_metrics(0.0F, 1.0F).radius == 16.0F) &&
              (scene_icon_metrics(std::nanf(""), 1.0F).radius == 16.0F) &&
              (scene_icon_metrics(1.0F, -2.0F).radius == 16.0F),
          "a scale that is not a positive number counts as 1");
  t.check(base.selectionRadius > base.radius,
          "the selection ring surrounds the icon");
}

/// A ray meets the ground plane y = 0 where it descends onto it within
/// reach; one running level, climbing, or landing beyond reach does not.
void check_ground_point(engine::tests::TestContext &t) noexcept {
  using engine::editor::ray_ground_point;
  engine::math::Ray ray{};
  ray.origin = Vec3(1.0F, 4.0F, 2.0F);
  ray.direction = Vec3(0.6F, -0.8F, 0.0F);
  // 0.6 and 0.8 round in binary, so the landing point (4, 0, 2) is met to
  // within a few float ulps at this magnitude: 1e-5 is ~40 ulps of 4.
  const auto near = [](float a, float b) noexcept {
    return std::fabs(a - b) <= 1.0e-5F;
  };
  const float landing = -ray.origin.y / ray.direction.y;
  Vec3 hit(9.0F, 9.0F, 9.0F);
  t.check(ray_ground_point(ray, 100.0F, &hit) && near(hit.x, 4.0F) &&
              (hit.y == 0.0F) && (hit.z == 2.0F),
          "a descending ray meets the ground where it lands");
  t.check(ray_ground_point(ray, landing, &hit) && near(hit.x, 4.0F),
          "a hit exactly at the reach counts");
  hit = Vec3(9.0F, 9.0F, 9.0F);
  t.check(!ray_ground_point(ray, landing * 0.999F, &hit) && (hit.x == 9.0F),
          "a hit beyond the reach does not, and leaves the output");
  ray.direction = Vec3(0.6F, 0.8F, 0.0F);
  t.check(!ray_ground_point(ray, 100.0F, &hit), "a climbing ray misses");
  ray.direction = Vec3(1.0F, 0.0F, 0.0F);
  t.check(!ray_ground_point(ray, 100.0F, &hit), "a level ray misses");
  ray.origin = Vec3(0.0F, -1.0F, 0.0F);
  ray.direction = Vec3(0.0F, -1.0F, 0.0F);
  t.check(!ray_ground_point(ray, 100.0F, &hit),
          "a ray below the ground heading down misses");
  ray.origin = Vec3(0.0F, 0.0F, 0.0F);
  t.check(ray_ground_point(ray, 100.0F, &hit) && (hit.x == 0.0F) &&
              (hit.z == 0.0F),
          "a ray starting on the ground meets it there");
  t.check(!ray_ground_point(ray, 100.0F, nullptr), "no output, no hit");
}

/// A press stays a click while it moves at most the slop, inclusive.
void check_click_slop(engine::tests::TestContext &t) noexcept {
  using engine::editor::kClickSlopPixels;
  using engine::editor::within_click_slop;
  t.check(within_click_slop(0.0F, 0.0F), "no movement is a click");
  t.check(within_click_slop(kClickSlopPixels, 0.0F) &&
              within_click_slop(0.0F, -kClickSlopPixels),
          "movement of exactly the slop is still a click");
  t.check(within_click_slop(3.0F * kClickSlopPixels / 5.0F,
                            4.0F * kClickSlopPixels / 5.0F),
          "the slop is a radius, not a box");
  t.check(
      !within_click_slop(kClickSlopPixels * 0.75F, kClickSlopPixels * 0.75F),
      "a diagonal beyond the radius is a drag");
  t.check(!within_click_slop(kClickSlopPixels + 0.01F, 0.0F),
          "beyond the slop is a drag");
}

/// The Game view names what stops the game rendering: no active camera,
/// or a winner tied in priority; one camera alone gives no notice.
void check_game_camera_notice(engine::tests::TestContext &t,
                              World &world) noexcept {
  using engine::editor::game_camera_notice;
  char notice[192] = "stale";
  t.check(game_camera_notice(world, notice, sizeof(notice)) &&
              (std::strstr(notice, "No camera") != nullptr),
          "a world with no camera says so");

  engine::runtime::CameraComponent camera{};
  camera.priority = 1.0F;
  const Entity first = world.create_scene_object();
  t.check(world.add_camera_component(first, camera), "add a camera");
  t.check(!game_camera_notice(world, notice, sizeof(notice)) &&
              (notice[0] == '\0'),
          "one camera rendering alone gives no notice");

  const Entity second = world.create_scene_object();
  t.check(world.add_camera_component(second, camera), "add a tied camera");
  t.check(
      game_camera_notice(world, notice, sizeof(notice)) &&
          (std::strstr(notice, "ties in priority with 1 other:") != nullptr),
      "a priority tie names the count");

  camera.active = false;
  t.check(world.add_camera_component(second, camera),
          "deactivate the second camera");
  t.check(!game_camera_notice(world, notice, sizeof(notice)),
          "an inactive camera does not tie");

  char tiny[8] = "stale";
  camera.active = true;
  t.check(world.add_camera_component(second, camera), "tie again");
  t.check(!game_camera_notice(world, tiny, sizeof(tiny)) && (tiny[0] == '\0'),
          "a notice that does not fit is refused, not cut");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 99;
  }
  check_hits(t, *world);
  check_cycle(t);
  std::unique_ptr<World> marqueeWorld(new (std::nothrow) World());
  if (marqueeWorld == nullptr) {
    return 98;
  }
  check_box_select(t, *marqueeWorld);
  std::unique_ptr<World> iconWorld(new (std::nothrow) World());
  if (iconWorld == nullptr) {
    return 97;
  }
  check_icons(t, *iconWorld);
  check_icon_metrics(t);
  check_ground_point(t);
  check_click_slop(t);
  std::unique_ptr<World> cameraWorld(new (std::nothrow) World());
  if (cameraWorld == nullptr) {
    return 96;
  }
  check_game_camera_notice(t, *cameraWorld);
  return t.finish("editor_scene_query");
}
