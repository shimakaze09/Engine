// Declares the Scene view's reference grid: lines on the ground plane
// (y = 0) around the orbit target, their spacing a power of ten that
// follows the zoom so a cell always spans a similar share of the view, as
// the Unity, Unreal and Godot scene grids do. The builder is a pure
// function; the Scene panel queues its lines as depth-tested debug lines,
// so geometry hides the grid and the Game view never shows it.

#pragma once

#include <cstddef>

#include "engine/math/vec3.h"

namespace engine::editor {

/// One grid line with its RGBA colour.
struct GridLine final {
  math::Vec3 from{};
  math::Vec3 to{};
  float color[4] = {};
};

/// Lines on each side of the centre line, per direction.
inline constexpr int kGridHalfLines = 20;
/// Lines one grid holds: 2 * kGridHalfLines + 1 per direction.
inline constexpr std::size_t kGridLineCount =
    2U * ((2U * static_cast<std::size_t>(kGridHalfLines)) + 1U);

/// The cell size at orbit distance `distance`: the largest power of ten
/// no greater than distance / 4, so a 20-cell half grid spans about 5 to
/// 50 times the distance. Floors at 0.01 for a distance at or below zero.
float grid_spacing(float distance) noexcept;

/// Builds the grid seen from orbit distance `distance` around `target`
/// into `out`, returning the line count (kGridLineCount), or 0 with `out`
/// untouched when `capacity` is smaller.
///
/// - Lines run along x and z through multiples of the spacing, centred
///   on the multiple nearest the target, so the grid stays put as the
///   target moves within a cell.
/// - Lines on multiples of ten cells are brighter, and every line fades
///   toward the grid's edge.
/// - The line on z = 0 is the X axis, drawn red; the line on x = 0 is the
///   Z axis, drawn blue.
/// - The plane is lifted by 1e-3 * distance, a few dozen depth steps at
///   that distance, so the grid does not flicker against a ground plane
///   at y = 0.
std::size_t build_reference_grid(const math::Vec3 &target, float distance,
                                 GridLine *out, std::size_t capacity) noexcept;

/// Queues the grid for the Scene camera as this frame's debug lines.
void emit_reference_grid(const math::Vec3 &target, float distance) noexcept;

} // namespace engine::editor
