// Verifies the Scene view's light gizmos and the pose they share with the
// renderer. A cone's rim lies at its range along the aim, at range times
// the tangent of the half-angle from the axis, closed, with four edges
// from the apex; an arrow runs its length along the aim with a four-stroke
// head at the tip; degenerate input is refused untouched.
// light_world_pose turns the authored aim by the entity's world rotation.
// For a selected spot light, both cones are queued; for a point light,
// its range sphere.

#include "editor_light_gizmos.h"

#include "engine/core/debug_draw.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/light_pose.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <array>
#include <cmath>
#include <memory>
#include <new>

namespace {

using engine::editor::GizmoLine;
using engine::math::Vec3;

// Rim points come from float trig at radii of order 10: a few ulps.
constexpr float kTol = 1.0e-5F;

bool near(float a, float b) noexcept {
  return std::fabs(a - b) <= kTol * std::fmax(1.0F, std::fabs(b));
}

bool near(const Vec3 &a, const Vec3 &b) noexcept {
  return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

/// Distance of `p` from the line x = 1, y = 2 (the test cone's axis).
float off_axis(const Vec3 &p) noexcept {
  return std::sqrt(((p.x - 1.0F) * (p.x - 1.0F)) +
                   ((p.y - 2.0F) * (p.y - 2.0F)));
}

void check_cone(engine::tests::TestContext &t) noexcept {
  using engine::editor::build_cone_lines;
  using engine::editor::kConeLineCount;
  using engine::editor::kConeRimSegments;
  std::array<GizmoLine, kConeLineCount> lines{};
  // Apex (1, 2, 3) aimed down -z (the aim's length does not matter),
  // range 10, half-angle 30 degrees: the rim circle sits at z = -7 with
  // radius 10 tan 30.
  const Vec3 apex(1.0F, 2.0F, 3.0F);
  const float half = 0.52359877F;
  t.check(build_cone_lines(apex, Vec3(0.0F, 0.0F, -2.0F), 10.0F, half,
                           lines.data(), lines.size()) == kConeLineCount,
          "a cone is its rim and four edges");
  const float rimRadius = 10.0F * std::tan(half);
  bool onRim = true;
  for (std::size_t i = 0U; i < kConeRimSegments; ++i) {
    onRim = onRim && near(lines[i].from.z, -7.0F) &&
            near(off_axis(lines[i].from), rimRadius) &&
            near(lines[i].to, lines[(i + 1U) % kConeRimSegments].from);
  }
  t.check(onRim, "the rim is a closed circle at the range and half-angle");
  bool edges = true;
  for (std::size_t e = kConeRimSegments; e < kConeLineCount; ++e) {
    edges = edges && (lines[e].from.x == apex.x) &&
            (lines[e].from.y == apex.y) && (lines[e].from.z == apex.z) &&
            near(lines[e].to.z, -7.0F) &&
            near(off_axis(lines[e].to), rimRadius);
  }
  t.check(edges, "the edges run from the apex to the rim");

  t.check((build_cone_lines(apex, Vec3(0.0F, 0.0F, -1.0F), 10.0F, 1.57F,
                            lines.data(), lines.size()) == kConeLineCount) &&
              near(off_axis(lines[0].from), 10.0F * std::tan(1.5F)),
          "a near-flat cone is drawn at 1.5 rad");

  lines[0].from = Vec3(9.0F, 9.0F, 9.0F);
  t.check((build_cone_lines(apex, Vec3(), 10.0F, half, lines.data(),
                            lines.size()) == 0U) &&
              (build_cone_lines(apex, Vec3(0.0F, 0.0F, -1.0F), 0.0F, half,
                                lines.data(), lines.size()) == 0U) &&
              (build_cone_lines(apex, Vec3(0.0F, 0.0F, -1.0F), 10.0F, half,
                                lines.data(), kConeLineCount - 1U) == 0U) &&
              (lines[0].from.x == 9.0F),
          "no aim, no range or no room is refused untouched");
}

void check_arrow(engine::tests::TestContext &t) noexcept {
  using engine::editor::build_arrow_lines;
  using engine::editor::kArrowLineCount;
  std::array<GizmoLine, kArrowLineCount> lines{};
  const Vec3 origin(1.0F, 2.0F, 3.0F);
  t.check(build_arrow_lines(origin, Vec3(0.0F, -3.0F, 0.0F), 2.0F, lines.data(),
                            lines.size()) == kArrowLineCount,
          "an arrow is a shaft and four strokes");
  const Vec3 tip(1.0F, 0.0F, 3.0F);
  t.check((lines[0].from.x == origin.x) && (lines[0].from.y == origin.y) &&
              (lines[0].from.z == origin.z) && near(lines[0].to, tip),
          "the shaft runs its length along the aim");
  bool head = true;
  for (std::size_t i = 1U; i < kArrowLineCount; ++i) {
    // A fifth of the length back up the shaft, half that to the side.
    const float aside =
        std::sqrt(((lines[i].to.x - 1.0F) * (lines[i].to.x - 1.0F)) +
                  ((lines[i].to.z - 3.0F) * (lines[i].to.z - 3.0F)));
    head = head && near(lines[i].from, tip) && near(lines[i].to.y, 0.4F) &&
           near(aside, 0.2F);
  }
  t.check(head, "the head strokes sweep back from the tip");
  t.check(build_arrow_lines(origin, Vec3(), 2.0F, lines.data(), lines.size()) ==
              0U,
          "an arrow with no aim is refused");
}

void check_pose_and_draw(engine::tests::TestContext &t,
                         engine::runtime::World &world) noexcept {
  // A quarter turn about +y turns an aim down -z to -x.
  engine::runtime::Transform transform{};
  transform.position = Vec3(4.0F, 5.0F, 6.0F);
  transform.rotation =
      engine::math::from_axis_angle(Vec3(0.0F, 1.0F, 0.0F), 1.57079632679F);
  const engine::runtime::Entity spotEntity =
      world.create_scene_object(transform);
  const engine::runtime::LightPose pose = engine::runtime::light_world_pose(
      world, spotEntity, Vec3(0.0F, 0.0F, -1.0F));
  t.check(near(pose.position, Vec3(4.0F, 5.0F, 6.0F)) &&
              near(pose.direction, Vec3(-1.0F, 0.0F, 0.0F)),
          "the pose is the entity's position and its turned aim");
  const engine::runtime::LightPose bare = engine::runtime::light_world_pose(
      world, engine::runtime::Entity{}, Vec3(0.0F, -1.0F, 0.0F));
  t.check(near(bare.position, Vec3()) &&
              near(bare.direction, Vec3(0.0F, -1.0F, 0.0F)),
          "no transform lights from the origin along the authored aim");

  engine::runtime::SpotLightComponent spot{};
  spot.direction = Vec3(0.0F, 0.0F, -1.0F);
  t.check(world.add_spot_light_component(spotEntity, spot),
          "add the spot light");
  const engine::runtime::Entity pointEntity =
      world.create_scene_object(transform);
  engine::runtime::PointLightComponent point{};
  point.radius = 7.0F;
  t.check(world.add_point_light_component(pointEntity, point),
          "add the point light");

  std::array<engine::core::DebugLine, 256> queued =
      std::array<engine::core::DebugLine, 256>();
  std::array<engine::core::DebugSphere, 4> spheres{};
  const std::size_t linesBefore =
      engine::core::debug_draw_get_lines(queued.data(), queued.size());
  engine::editor::draw_light_gizmos(world, spotEntity);
  t.check(engine::core::debug_draw_get_lines(queued.data(), queued.size()) ==
              linesBefore + (2U * engine::editor::kConeLineCount),
          "a spot light queues its outer and inner cones");
  engine::editor::draw_light_gizmos(world, pointEntity);
  const std::size_t sphereCount =
      engine::core::debug_draw_get_spheres(spheres.data(), spheres.size());
  t.check((sphereCount == 1U) && (spheres[0].radius == 7.0F),
          "a point light queues its range sphere");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  check_cone(t);
  check_arrow(t);
  if (!engine::core::initialize_debug_draw()) {
    return 98;
  }
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 99;
  }
  check_pose_and_draw(t, *world);
  engine::core::shutdown_debug_draw();
  return t.finish("editor_light_gizmos");
}
