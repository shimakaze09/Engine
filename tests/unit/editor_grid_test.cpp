// Verifies the Scene view's reference grid builder: the spacing is the
// largest power of ten no greater than a quarter of the orbit distance,
// boundaries included; the lines sit on the lifted ground plane, centred
// on the grid multiple nearest the orbit target; the X and Z axes are the
// red and blue lines through the origin, drawn only while the grid covers
// them; lines on multiples of ten cells are brighter and every line fades
// toward the edge; and a buffer too small is refused untouched.

#include "editor_grid.h"

#include "engine/math/vec3.h"

#include "../test_harness.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace {

using engine::editor::build_reference_grid;
using engine::editor::grid_spacing;
using engine::editor::GridLine;
using engine::editor::kGridLineCount;
using engine::math::Vec3;

using Lines = std::array<GridLine, kGridLineCount>;

/// The spacings are built by exact decades (x10, /10), so each is compared
/// exactly with the same decade built the same way.
void check_spacing(engine::tests::TestContext &t) noexcept {
  t.check(grid_spacing(10.0F) == 1.0F, "10 m out, 1 m cells");
  t.check(grid_spacing(4.0F) == 1.0F, "4 m out is the 1 m boundary");
  t.check(grid_spacing(3.9F) == (1.0F / 10.0F), "just under 4 m, 0.1 m cells");
  t.check(grid_spacing(40.0F) == 10.0F, "40 m out is the 10 m boundary");
  t.check(grid_spacing(399.0F) == 10.0F, "just under 400 m, 10 m cells");
  t.check(grid_spacing(1000.0F) == 100.0F, "1000 m out, 100 m cells");
  t.check(grid_spacing(0.5F) == (1.0F / 10.0F),
          "the closest orbit, 0.1 m cells");
  t.check(grid_spacing(0.0F) == 0.01F, "a zero distance floors at 1 cm");
}

bool is_red(const GridLine &line) noexcept {
  return (line.color[0] > 0.8F) && (line.color[2] < 0.4F);
}

bool is_blue(const GridLine &line) noexcept {
  return (line.color[2] > 0.8F) && (line.color[0] < 0.4F);
}

void check_layout(engine::tests::TestContext &t) noexcept {
  Lines lines{};
  // 1 m cells; the nearest multiple to (12.3, -7.7) is (12, -8).
  const std::size_t count = build_reference_grid(
      Vec3(12.3F, 5.0F, -7.7F), 10.0F, lines.data(), lines.size());
  t.check(count == kGridLineCount, "82 lines: 41 each way");
  bool flat = true;
  float minX = 1.0e9F;
  float maxX = -1.0e9F;
  float minZ = 1.0e9F;
  float maxZ = -1.0e9F;
  for (std::size_t i = 0U; i < count; ++i) {
    const GridLine &line = lines[i];
    // Lifted by 1e-3 * distance = 0.01 m.
    flat = flat && (line.from.y == line.to.y) &&
           (std::fabs(line.from.y - 0.01F) <= 1.0e-7F);
    minX = std::fmin(minX, std::fmin(line.from.x, line.to.x));
    maxX = std::fmax(maxX, std::fmax(line.from.x, line.to.x));
    minZ = std::fmin(minZ, std::fmin(line.from.z, line.to.z));
    maxZ = std::fmax(maxZ, std::fmax(line.from.z, line.to.z));
  }
  t.check(flat, "every line lies on the lifted ground plane");
  t.check((minX == -8.0F) && (maxX == 32.0F) && (minZ == -28.0F) &&
              (maxZ == 12.0F),
          "the grid spans 20 cells each side of (12, -8)");

  // x = 0 lies 12 cells left of centre and z = 0 lies 8 cells above it,
  // both inside: one blue and one red line.
  int red = 0;
  int blue = 0;
  for (const GridLine &line : lines) {
    if (is_red(line)) {
      ++red;
      t.check((line.from.z == 0.0F) && (line.to.z == 0.0F),
              "the red line is the X axis, z = 0");
    }
    if (is_blue(line)) {
      ++blue;
      t.check((line.from.x == 0.0F) && (line.to.x == 0.0F),
              "the blue line is the Z axis, x = 0");
    }
  }
  t.check((red == 1) && (blue == 1), "one line per axis");

  build_reference_grid(Vec3(500.0F, 0.0F, 500.0F), 10.0F, lines.data(),
                       lines.size());
  bool noAxis = true;
  for (const GridLine &line : lines) {
    noAxis = noAxis && !is_red(line) && !is_blue(line);
  }
  t.check(noAxis, "no axis line when the grid does not reach the origin");
}

void check_emphasis(engine::tests::TestContext &t) noexcept {
  Lines lines{};
  build_reference_grid(Vec3(5.0F, 0.0F, 5.0F), 10.0F, lines.data(),
                       lines.size());
  // Lines alternate along x then along z, per ring from -20. Along z at
  // ring r sits x = 5 + r: x = 10 (ring 5) is a decade line, x = 9
  // (ring 4) and x = 11 (ring 6) are not.
  const auto alongZ = [&](int ring) -> const GridLine & {
    return lines[static_cast<std::size_t>(((ring + 20) * 2) + 1)];
  };
  t.check(alongZ(5).from.x == 10.0F, "ring 5 along z is x = 10");
  t.check((alongZ(5).color[3] > alongZ(4).color[3]) &&
              (alongZ(5).color[3] > alongZ(6).color[3]),
          "a decade line is brighter than its neighbours");
  t.check((alongZ(1).color[3] > alongZ(3).color[3]) &&
              (alongZ(-1).color[3] > alongZ(-3).color[3]),
          "lines fade away from the centre");
  t.check(alongZ(20).color[3] > 0.0F, "the edge line is faint, not gone");
}

void check_refusal(engine::tests::TestContext &t) noexcept {
  std::array<GridLine, kGridLineCount - 1U> small{};
  small[0].color[3] = 0.5F;
  t.check(
      (build_reference_grid(Vec3(), 10.0F, small.data(), small.size()) == 0U) &&
          (small[0].color[3] == 0.5F),
      "a buffer too small is refused untouched");
  t.check(build_reference_grid(Vec3(), 10.0F, nullptr, kGridLineCount) == 0U,
          "no buffer is refused");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  check_spacing(t);
  check_layout(t);
  check_emphasis(t);
  check_refusal(t);
  return t.finish("editor_grid");
}
