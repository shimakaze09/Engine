// Implements the Scene view's reference grid declared in editor_grid.h.

#include "editor_grid.h"

#include <array>
#include <cmath>
#include <cstdlib>

#include "engine/core/debug_draw.h"

namespace engine::editor {

namespace {

constexpr float kLiftPerDistance = 1.0e-3F;
constexpr float kMinorAlpha = 0.28F;
constexpr float kMajorAlpha = 0.55F;
constexpr float kAxisAlpha = 0.9F;

/// Colour of the grid line `index` spacings from the world origin, `ring`
/// lines from the grid's centre, on the axis named by `axisColor` when it
/// is the origin line.
void line_color(long long index, int ring, const float axisColor[3],
                float out[4]) noexcept {
  // Fades linearly to a quarter of full strength at the outermost line.
  const float fade = 1.0F - (0.75F * static_cast<float>(std::abs(ring)) /
                             static_cast<float>(kGridHalfLines));
  if (index == 0) {
    out[0] = axisColor[0];
    out[1] = axisColor[1];
    out[2] = axisColor[2];
    out[3] = kAxisAlpha * fade;
    return;
  }
  const bool major = (index % 10) == 0;
  out[0] = 0.55F;
  out[1] = 0.55F;
  out[2] = 0.58F;
  out[3] = (major ? kMajorAlpha : kMinorAlpha) * fade;
}

} // namespace

float grid_spacing(float distance) noexcept {
  const float reach = distance / 4.0F;
  if (!(reach > 0.01F)) {
    return 0.01F;
  }
  // Stepped by exact decades rather than log10, so a distance on a
  // boundary (40, 400) lands on the larger spacing every time.
  float spacing = 1.0F;
  while ((spacing * 10.0F) <= reach) {
    spacing *= 10.0F;
  }
  while ((spacing > reach) && (spacing > 0.01F)) {
    spacing /= 10.0F;
  }
  return spacing;
}

std::size_t build_reference_grid(const math::Vec3 &target, float distance,
                                 GridLine *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity < kGridLineCount)) {
    return 0U;
  }
  const float spacing = grid_spacing(distance);
  const float lift = kLiftPerDistance * ((distance > 1.0F) ? distance : 1.0F);
  const long long centerX = std::llround(target.x / spacing);
  const long long centerZ = std::llround(target.z / spacing);
  const float extent = static_cast<float>(kGridHalfLines) * spacing;
  const float minX = (static_cast<float>(centerX) * spacing) - extent;
  const float maxX = (static_cast<float>(centerX) * spacing) + extent;
  const float minZ = (static_cast<float>(centerZ) * spacing) - extent;
  const float maxZ = (static_cast<float>(centerZ) * spacing) + extent;
  constexpr float kXAxis[3] = {0.9F, 0.25F, 0.25F};
  constexpr float kZAxis[3] = {0.3F, 0.5F, 0.95F};

  std::size_t count = 0U;
  for (int ring = -kGridHalfLines; ring <= kGridHalfLines; ++ring) {
    // Along x, at z = (centerZ + ring) * spacing: z = 0 is the X axis.
    const long long zIndex = centerZ + ring;
    const float z = static_cast<float>(zIndex) * spacing;
    GridLine &alongX = out[count++];
    alongX.from = math::Vec3(minX, lift, z);
    alongX.to = math::Vec3(maxX, lift, z);
    line_color(zIndex, ring, kXAxis, alongX.color);

    // Along z, at x = (centerX + ring) * spacing: x = 0 is the Z axis.
    const long long xIndex = centerX + ring;
    const float x = static_cast<float>(xIndex) * spacing;
    GridLine &alongZ = out[count++];
    alongZ.from = math::Vec3(x, lift, minZ);
    alongZ.to = math::Vec3(x, lift, maxZ);
    line_color(xIndex, ring, kZAxis, alongZ.color);
  }
  return count;
}

void emit_reference_grid(const math::Vec3 &target, float distance) noexcept {
  std::array<GridLine, kGridLineCount> lines =
      std::array<GridLine, kGridLineCount>();
  const std::size_t count =
      build_reference_grid(target, distance, lines.data(), lines.size());
  for (std::size_t i = 0U; i < count; ++i) {
    const GridLine &line = lines[i];
    core::debug_draw_line(
        {line.from.x, line.from.y, line.from.z},
        {line.to.x, line.to.y, line.to.z},
        {line.color[0], line.color[1], line.color[2], line.color[3]});
  }
}

} // namespace engine::editor
