// Declares the Scene view's gizmos for a selected light, drawn as the
// Unity, Unreal and Godot editors draw them: a point light's range as a
// wire sphere, a spot light's outer and inner cones out to its range, and
// a directional light's aim as an arrow. Each is placed and aimed through
// runtime::light_world_pose, the pose the renderer lights with.

#pragma once

#include <cstddef>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// One gizmo line segment.
struct GizmoLine final {
  math::Vec3 from{};
  math::Vec3 to{};
};

/// Segments in a cone's rim circle.
inline constexpr std::size_t kConeRimSegments = 32U;
/// Lines one cone takes: the rim plus four edges from the apex.
inline constexpr std::size_t kConeLineCount = kConeRimSegments + 4U;
/// Lines one arrow takes: the shaft plus four head strokes.
inline constexpr std::size_t kArrowLineCount = 5U;

/// A cone from `apex` along `direction` (any length) out to `range`, with
/// half-angle `halfAngle` (radians, as the renderer compares against the
/// cosine), as its rim circle at `range` and four edges from the apex to
/// the rim. The half-angle is drawn at most 1.5 rad (86 degrees), since a
/// wider cone's rim runs off to infinity. Returns kConeLineCount, or 0
/// with `out` untouched when `capacity` is short or the direction or range
/// is zero.
std::size_t build_cone_lines(const math::Vec3 &apex,
                             const math::Vec3 &direction, float range,
                             float halfAngle, GizmoLine *out,
                             std::size_t capacity) noexcept;

/// An arrow from `origin` along `direction` (any length), `length` long,
/// with a four-stroke head a fifth of its length. Returns kArrowLineCount,
/// or 0 with `out` untouched when `capacity` is short or the direction or
/// length is zero.
std::size_t build_arrow_lines(const math::Vec3 &origin,
                              const math::Vec3 &direction, float length,
                              GizmoLine *out, std::size_t capacity) noexcept;

/// Queues, as this frame's debug lines, the gizmo of every light
/// component on `entity`.
void draw_light_gizmos(const runtime::World &world,
                       runtime::Entity entity) noexcept;

} // namespace engine::editor
