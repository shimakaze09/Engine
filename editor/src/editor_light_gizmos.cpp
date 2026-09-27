// Implements the Scene view's light gizmos declared in
// editor_light_gizmos.h.

#include "editor_light_gizmos.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/debug_draw.h"
#include "engine/runtime/light_pose.h"

namespace engine::editor {

namespace {

constexpr float kMaxDrawnHalfAngle = 1.5F;
constexpr float kTwoPi = 6.28318530718F;
/// A directional light's arrow is drawn this long: it has an aim, not a
/// reach.
constexpr float kDirectionalArrowLength = 2.0F;
/// The marker drawn for a point light without a range.
constexpr float kMarkerRadius = 0.2F;

constexpr core::DebugColor kLightColor{1.0F, 0.9F, 0.4F, 0.9F};
constexpr core::DebugColor kInnerConeColor{1.0F, 0.9F, 0.4F, 0.4F};

/// Two unit vectors perpendicular to unit `axis` and to each other.
void perpendicular_basis(const math::Vec3 &axis, math::Vec3 *u,
                         math::Vec3 *v) noexcept {
  const math::Vec3 helper = (std::fabs(axis.y) < 0.99F)
                                ? math::Vec3(0.0F, 1.0F, 0.0F)
                                : math::Vec3(1.0F, 0.0F, 0.0F);
  *u = math::normalize(math::cross(axis, helper));
  *v = math::cross(axis, *u);
}

core::DebugVec3 to_debug(const math::Vec3 &point) noexcept {
  return {point.x, point.y, point.z};
}

void emit(const GizmoLine *lines, std::size_t count,
          const core::DebugColor &color) noexcept {
  for (std::size_t i = 0U; i < count; ++i) {
    core::debug_draw_line(to_debug(lines[i].from), to_debug(lines[i].to),
                          color);
  }
}

} // namespace

std::size_t build_cone_lines(const math::Vec3 &apex,
                             const math::Vec3 &direction, float range,
                             float halfAngle, GizmoLine *out,
                             std::size_t capacity) noexcept {
  const float length = math::length(direction);
  if ((out == nullptr) || (capacity < kConeLineCount) || !(length > 0.0F) ||
      !(range > 0.0F)) {
    return 0U;
  }
  const math::Vec3 axis = math::mul(direction, 1.0F / length);
  math::Vec3 u{};
  math::Vec3 v{};
  perpendicular_basis(axis, &u, &v);
  const float angle = std::clamp(halfAngle, 0.0F, kMaxDrawnHalfAngle);
  const float rimRadius = range * std::tan(angle);
  const math::Vec3 rimCenter = math::add(apex, math::mul(axis, range));
  const auto rim_point = [&](std::size_t i) noexcept {
    const float theta = (kTwoPi * static_cast<float>(i % kConeRimSegments)) /
                        static_cast<float>(kConeRimSegments);
    return math::add(rimCenter,
                     math::add(math::mul(u, rimRadius * std::cos(theta)),
                               math::mul(v, rimRadius * std::sin(theta))));
  };
  std::size_t count = 0U;
  for (std::size_t i = 0U; i < kConeRimSegments; ++i) {
    out[count++] = GizmoLine{rim_point(i), rim_point(i + 1U)};
  }
  for (std::size_t edge = 0U; edge < 4U; ++edge) {
    out[count++] = GizmoLine{apex, rim_point(edge * (kConeRimSegments / 4U))};
  }
  return count;
}

std::size_t build_arrow_lines(const math::Vec3 &origin,
                              const math::Vec3 &direction, float length,
                              GizmoLine *out, std::size_t capacity) noexcept {
  const float directionLength = math::length(direction);
  if ((out == nullptr) || (capacity < kArrowLineCount) ||
      !(directionLength > 0.0F) || !(length > 0.0F)) {
    return 0U;
  }
  const math::Vec3 axis = math::mul(direction, 1.0F / directionLength);
  math::Vec3 u{};
  math::Vec3 v{};
  perpendicular_basis(axis, &u, &v);
  const math::Vec3 tip = math::add(origin, math::mul(axis, length));
  const float head = 0.2F * length;
  const math::Vec3 back = math::sub(tip, math::mul(axis, head));
  const float spread = 0.5F * head;
  out[0] = GizmoLine{origin, tip};
  out[1] = GizmoLine{tip, math::add(back, math::mul(u, spread))};
  out[2] = GizmoLine{tip, math::sub(back, math::mul(u, spread))};
  out[3] = GizmoLine{tip, math::add(back, math::mul(v, spread))};
  out[4] = GizmoLine{tip, math::sub(back, math::mul(v, spread))};
  return kArrowLineCount;
}

void draw_light_gizmos(const runtime::World &world,
                       runtime::Entity entity) noexcept {
  std::array<GizmoLine, kConeLineCount> lines{};
  runtime::LightComponent light{};
  if (world.get_light_component(entity, &light)) {
    const runtime::LightPose pose =
        runtime::light_world_pose(world, entity, light.direction);
    if (light.type == runtime::LightType::Directional) {
      emit(lines.data(),
           build_arrow_lines(pose.position, pose.direction,
                             kDirectionalArrowLength, lines.data(),
                             lines.size()),
           kLightColor);
    } else {
      core::debug_draw_sphere(to_debug(pose.position), kMarkerRadius,
                              kLightColor);
    }
  }
  runtime::PointLightComponent point{};
  if (world.get_point_light_component(entity, &point)) {
    const runtime::LightPose pose =
        runtime::light_world_pose(world, entity, math::Vec3());
    core::debug_draw_sphere(to_debug(pose.position), point.radius, kLightColor);
  }
  runtime::SpotLightComponent spot{};
  if (world.get_spot_light_component(entity, &spot)) {
    const runtime::LightPose pose =
        runtime::light_world_pose(world, entity, spot.direction);
    emit(lines.data(),
         build_cone_lines(pose.position, pose.direction, spot.radius,
                          spot.outerConeAngle, lines.data(), lines.size()),
         kLightColor);
    emit(lines.data(),
         build_cone_lines(pose.position, pose.direction, spot.radius,
                          spot.innerConeAngle, lines.data(), lines.size()),
         kInnerConeColor);
  }
}

} // namespace engine::editor
