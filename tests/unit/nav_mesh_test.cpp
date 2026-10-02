// Verifies the navigation mesh bake and its queries on synthetic levels
// described by a box-and-ramp sampler, so each rule is exercised alone:
// - a flat floor bakes, is eroded by the agent radius, and a path across
//   it is one straight segment;
// - a wall is walked around: every point of the path is on the mesh and
//   its length is within a cell's rounding of the shortest route;
// - a step within the climb is walked up and one past it is not;
// - a slope within the limit is walkable and one past it, rising no more
//   than the climb per column, is not;
// - a ceiling lower than the agent removes the floor under it;
// - a bridge over a floor keeps both surfaces, and a path on the bridge
//   stays on it;
// - islands with no walkable link are Unreachable, a point far from the
//   mesh is OffMesh, and a path longer than the output is TooLong;
// - invalid settings are refused with the mesh unchanged;
// - the same level bakes to the same mesh, and the same query gives the
//   same path, bit for bit.

#include "engine/navigation/nav_mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../test_harness.h"

namespace {

namespace nav = engine::navigation;
using engine::math::Vec3;

engine::tests::TestContext g_tests;

/// An axis-aligned solid box.
struct Box final {
  Vec3 min{};
  Vec3 max{};
};

/// A ramp rising along +x: y = baseY + slope * (x - x0) over [x0, x1] and
/// [z0, z1].
struct Ramp final {
  float x0 = 0.0F;
  float x1 = 0.0F;
  float z0 = 0.0F;
  float z1 = 0.0F;
  float baseY = 0.0F;
  float slope = 0.0F;
};

struct Level final {
  std::vector<Box> boxes;
  std::vector<Ramp> ramps;
};

std::size_t sample(void *context, float x, float z, nav::NavSurface *out,
                   std::size_t capacity) noexcept {
  const auto &level = *static_cast<const Level *>(context);
  std::size_t count = 0U;
  // Undersides of everything above, to measure headroom.
  const auto clearance_above = [&](float y) noexcept {
    float clear = 1000.0F;
    for (const Box &b : level.boxes) {
      if ((x >= b.min.x) && (x <= b.max.x) && (z >= b.min.z) &&
          (z <= b.max.z) && (b.min.y >= y) && (b.max.y > y)) {
        clear = std::fmin(clear, b.min.y - y);
      }
    }
    return clear;
  };
  for (const Box &b : level.boxes) {
    if ((count < capacity) && (x >= b.min.x) && (x <= b.max.x) &&
        (z >= b.min.z) && (z <= b.max.z)) {
      out[count++] = nav::NavSurface{b.max.y, 1.0F, clearance_above(b.max.y)};
    }
  }
  for (const Ramp &r : level.ramps) {
    if ((count < capacity) && (x >= r.x0) && (x <= r.x1) && (z >= r.z0) &&
        (z <= r.z1)) {
      const float y = r.baseY + (r.slope * (x - r.x0));
      out[count++] = nav::NavSurface{
          y, 1.0F / std::sqrt(1.0F + (r.slope * r.slope)), clearance_above(y)};
    }
  }
  return count;
}

nav::NavBakeSettings settings_for(float halfX, float halfZ) {
  nav::NavBakeSettings s{};
  s.boundsMin = Vec3(-halfX, -1.0F, -halfZ);
  s.boundsMax = Vec3(halfX, 5.0F, halfZ);
  return s;
}

/// The 10 by 10 m floor with its top at y = 0.
Box floor_box() {
  return Box{Vec3(-5.0F, -1.0F, -5.0F), Vec3(5.0F, 0.0F, 5.0F)};
}

bool bake(const Level &level, const nav::NavBakeSettings &s,
          nav::NavMesh *mesh) {
  return nav::bake_nav_mesh(s, &sample, const_cast<Level *>(&level), mesh);
}

float path_length(const Vec3 *points, std::size_t count) {
  float total = 0.0F;
  for (std::size_t i = 1U; i < count; ++i) {
    const float dx = points[i].x - points[i - 1U].x;
    const float dy = points[i].y - points[i - 1U].y;
    const float dz = points[i].z - points[i - 1U].z;
    total += std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
  }
  return total;
}

/// True when every point along the path, every 5 cm, is over a walkable
/// column at the path's height.
bool path_on_mesh(const nav::NavQuery &query, const Vec3 *points,
                  std::size_t count) {
  for (std::size_t i = 1U; i < count; ++i) {
    const Vec3 a = points[i - 1U];
    const Vec3 b = points[i];
    const float length =
        std::sqrt(((b.x - a.x) * (b.x - a.x)) + ((b.z - a.z) * (b.z - a.z)));
    const int steps = static_cast<int>(length / 0.05F) + 1;
    for (int k = 0; k <= steps; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(steps);
      const Vec3 p(a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t),
                   a.z + ((b.z - a.z) * t));
      Vec3 snapped{};
      // A point on an edge between columns may snap to the neighbour;
      // a millimetre is far below a cell.
      if (!query.nearest_point(p, &snapped) ||
          (std::fabs(snapped.x - p.x) > 1.0e-3F) ||
          (std::fabs(snapped.z - p.z) > 1.0e-3F)) {
        std::fprintf(stderr, "  off the mesh at (%.3f, %.3f, %.3f)\n", p.x, p.y,
                     p.z);
        return false;
      }
    }
  }
  return true;
}

void check_flat_floor() {
  Level level{};
  level.boxes.push_back(floor_box());
  nav::NavMesh mesh{};
  g_tests.check(bake(level, settings_for(5.0F, 5.0F), &mesh) && !mesh.empty() &&
                    (mesh.columns_x() == 40) && (mesh.columns_z() == 40),
                "a flat floor bakes a 40 by 40 grid");
  // Radius 0.4 at 0.25 m cells erodes two columns from every edge.
  g_tests.check((mesh.surface_count(0, 20) == 0U) &&
                    (mesh.surface_count(1, 20) == 0U) &&
                    (mesh.surface_count(2, 20) == 1U) &&
                    (mesh.surface_count(37, 20) == 1U) &&
                    (mesh.surface_count(38, 20) == 0U),
                "the floor is eroded by the agent radius");
  // 36 by 36 columns merge into rectangles of at most 32 a side.
  g_tests.check(mesh.rect_count() == 4U,
                "the floor merges into four rectangles");
  nav::NavQuery query{};
  Vec3 path[16] = {};
  std::size_t count = 0U;
  g_tests.check(
      query.init(mesh) &&
          (query.find_path(Vec3(-3.0F, 0.0F, -3.0F), Vec3(3.0F, 0.0F, 3.0F),
                           path, 16U, &count) == nav::NavPathResult::Found) &&
          (count == 2U) && (path[0].x == -3.0F) && (path[1].z == 3.0F) &&
          (path[0].y == 0.0F) && (path[1].y == 0.0F),
      "a path across an open floor is one straight segment");
  g_tests.check(
      (query.find_path(Vec3(-3.0F, 0.0F, -3.0F), Vec3(-2.9F, 0.0F, -3.0F), path,
                       16U, &count) == nav::NavPathResult::Found) &&
          (count == 2U),
      "a path within one rectangle is its two ends");
}

void check_wall() {
  Level level{};
  level.boxes.push_back(floor_box());
  level.boxes.push_back(Box{Vec3(-0.5F, 0.0F, -5.0F), Vec3(0.5F, 2.0F, 3.0F)});
  nav::NavMesh mesh{};
  nav::NavQuery query{};
  Vec3 path[32] = {};
  std::size_t count = 0U;
  const nav::NavPathResult result =
      (bake(level, settings_for(5.0F, 5.0F), &mesh) && query.init(mesh))
          ? query.find_path(Vec3(-3.0F, 0.0F, 0.0F), Vec3(3.0F, 0.0F, 0.0F),
                            path, 32U, &count)
          : nav::NavPathResult::OffMesh;
  g_tests.check(result == nav::NavPathResult::Found,
                "a path around the wall is found");
  // The route hugging the wall's end, (+-0.5, 3), is the floor on its
  // length; the one around the eroded corners, (+-1, 3.5), its ceiling.
  const float hugging = (2.0F * std::sqrt(6.25F + 9.0F)) + 1.0F;
  const float eroded = (2.0F * std::sqrt(4.0F + 12.25F)) + 2.0F;
  const float length = path_length(path, count);
  g_tests.check((length > hugging) && (length <= eroded + 0.01F),
                "the route around the wall is no longer than the one around "
                "the eroded corners");
  g_tests.check(path_on_mesh(query, path, count),
                "every point of the route is on the mesh");
  // Column (16, 32) touches the wall's end corner diagonally: a chamfer
  // erodes it as far as the straight sides, a straight-steps distance
  // would keep it.
  g_tests.check((mesh.surface_count(16, 32) == 0U) &&
                    (mesh.surface_count(16, 34) == 1U),
                "the wall's outside corner is eroded as far as its sides");
  // Erosion keeps whole columns, so a corner may sit up to half a column
  // inside the radius, never closer.
  bool clearsWall = true;
  for (std::size_t i = 0U; i < count; ++i) {
    const float dx = std::fmax(std::fabs(path[i].x) - 0.5F, 0.0F);
    const float dz = std::fmax(path[i].z - 3.0F, 0.0F);
    clearsWall = clearsWall && (std::sqrt((dx * dx) + (dz * dz)) >= 0.275F);
  }
  g_tests.check(
      clearsWall,
      "no corner is nearer the wall than the radius less half a column");
}

void check_steps() {
  for (const float rise : {0.3F, 0.5F}) {
    Level level{};
    level.boxes.push_back(
        Box{Vec3(-5.0F, -1.0F, -5.0F), Vec3(0.0F, 0.0F, 5.0F)});
    level.boxes.push_back(
        Box{Vec3(0.0F, -1.0F, -5.0F), Vec3(5.0F, rise, 5.0F)});
    nav::NavMesh mesh{};
    nav::NavQuery query{};
    Vec3 path[16] = {};
    std::size_t count = 0U;
    const nav::NavPathResult result =
        (bake(level, settings_for(5.0F, 5.0F), &mesh) && query.init(mesh))
            ? query.find_path(Vec3(-3.0F, 0.0F, 0.0F), Vec3(3.0F, rise, 0.0F),
                              path, 16U, &count)
            : nav::NavPathResult::OffMesh;
    if (rise < 0.4F) {
      g_tests.check((result == nav::NavPathResult::Found) &&
                        (path[count - 1U].y == rise),
                    "a 0.3 m step within the climb is walked up");
    } else {
      g_tests.check(result == nav::NavPathResult::Unreachable,
                    "a 0.5 m step past the climb is not");
    }
  }
}

void check_slopes() {
  // A 50 degree ramp rises 0.3 m a column, within the climb, so only the
  // slope limit can refuse it.
  for (const float degrees : {30.0F, 50.0F}) {
    Level level{};
    Ramp ramp{};
    ramp.x0 = -1.0F;
    ramp.x1 = 1.0F;
    ramp.z0 = -5.0F;
    ramp.z1 = 5.0F;
    ramp.slope = std::tan(degrees * 3.14159265F / 180.0F);
    nav::NavBakeSettings s = settings_for(5.0F, 5.0F);
    s.boundsMin.x = -1.0F;
    s.boundsMax.x = 1.0F;
    level.ramps.push_back(ramp);
    nav::NavMesh mesh{};
    const bool baked = bake(level, s, &mesh);
    if (degrees < 45.0F) {
      g_tests.check(baked && !mesh.empty(), "a 30 degree slope is walkable");
    } else {
      g_tests.check(
          baked && mesh.empty(),
          "a 50 degree slope, past the 45 degree limit, is not walkable");
    }
  }
}

void check_low_ceiling() {
  Level level{};
  level.boxes.push_back(floor_box());
  level.boxes.push_back(Box{Vec3(-1.0F, 1.0F, -1.0F), Vec3(1.0F, 2.0F, 1.0F)});
  nav::NavMesh mesh{};
  g_tests.check(bake(level, settings_for(5.0F, 5.0F), &mesh),
                "a floor under a low ceiling bakes");
  // Column 20 is x = 0.125: under the ceiling, 1 m of headroom. The
  // ceiling's own roof stays walkable; the floor under it does not.
  g_tests.check((mesh.surface_count(20, 20) == 1U) &&
                    (mesh.surface_height(20, 20, 0U) == 2.0F) &&
                    (mesh.surface_count(20, 30) == 1U) &&
                    (mesh.surface_height(20, 30, 0U) == 0.0F),
                "the floor under a ceiling lower than the agent is not "
                "walkable, and the open floor and the roof are");
}

void check_bridge() {
  Level level{};
  level.boxes.push_back(floor_box());
  level.boxes.push_back(Box{Vec3(-5.0F, 2.9F, -1.0F), Vec3(5.0F, 3.0F, 1.0F)});
  nav::NavMesh mesh{};
  nav::NavQuery query{};
  Vec3 path[16] = {};
  std::size_t count = 0U;
  g_tests.check(bake(level, settings_for(5.0F, 5.0F), &mesh) &&
                    query.init(mesh) && (mesh.surface_count(20, 20) == 2U) &&
                    (mesh.surface_height(20, 20, 0U) == 3.0F) &&
                    (mesh.surface_height(20, 20, 1U) == 0.0F),
                "a bridge over the floor keeps both surfaces, top first");
  bool onBridge =
      query.find_path(Vec3(-3.0F, 3.0F, 0.0F), Vec3(3.0F, 3.0F, 0.0F), path,
                      16U, &count) == nav::NavPathResult::Found;
  for (std::size_t i = 0U; i < count; ++i) {
    onBridge = onBridge && (path[i].y == 3.0F);
  }
  g_tests.check(onBridge && (count == 2U),
                "a path along the bridge stays on the bridge");
  bool underneath =
      query.find_path(Vec3(-3.0F, 0.0F, 0.0F), Vec3(3.0F, 0.0F, 0.0F), path,
                      16U, &count) == nav::NavPathResult::Found;
  for (std::size_t i = 0U; i < count; ++i) {
    underneath = underneath && (path[i].y == 0.0F);
  }
  g_tests.check(underneath, "a path under the bridge stays on the floor");
}

void check_failures() {
  Level level{};
  level.boxes.push_back(
      Box{Vec3(-5.0F, -1.0F, -5.0F), Vec3(-1.0F, 0.0F, 5.0F)});
  level.boxes.push_back(Box{Vec3(1.0F, -1.0F, -5.0F), Vec3(5.0F, 0.0F, 5.0F)});
  nav::NavMesh mesh{};
  nav::NavQuery query{};
  Vec3 path[16] = {};
  std::size_t count = 7U;
  g_tests.check(
      bake(level, settings_for(5.0F, 5.0F), &mesh) && query.init(mesh) &&
          (query.find_path(Vec3(-3.0F, 0.0F, 0.0F), Vec3(3.0F, 0.0F, 0.0F),
                           path, 16U,
                           &count) == nav::NavPathResult::Unreachable) &&
          (count == 0U),
      "islands with no link between them are unreachable");
  g_tests.check(query.find_path(Vec3(-3.0F, 0.0F, 0.0F),
                                Vec3(40.0F, 0.0F, 0.0F), path, 16U,
                                &count) == nav::NavPathResult::OffMesh,
                "a point far from the mesh is off it");
  g_tests.check(query.find_path(Vec3(-3.0F, 0.0F, 0.0F),
                                Vec3(-3.0F, 9.0F, 0.0F), path, 16U,
                                &count) == nav::NavPathResult::OffMesh,
                "a point far above the mesh is off it");

  Level walled{};
  walled.boxes.push_back(floor_box());
  walled.boxes.push_back(Box{Vec3(-0.5F, 0.0F, -5.0F), Vec3(0.5F, 2.0F, 3.0F)});
  nav::NavMesh walledMesh{};
  nav::NavQuery walledQuery{};
  g_tests.check(bake(walled, settings_for(5.0F, 5.0F), &walledMesh) &&
                    walledQuery.init(walledMesh) &&
                    (walledQuery.find_path(
                         Vec3(-3.0F, 0.0F, 0.0F), Vec3(3.0F, 0.0F, 0.0F), path,
                         2U, &count) == nav::NavPathResult::TooLong) &&
                    (count == 0U),
                "a path with more corners than the output is too long");

  const std::uint64_t before = mesh.content_hash();
  nav::NavBakeSettings bad = settings_for(5.0F, 5.0F);
  bad.cellSize = 0.0F;
  g_tests.check(!bake(level, bad, &mesh), "a zero cell size is refused");
  bad = settings_for(5.0F, 5.0F);
  bad.boundsMax.x = -6.0F;
  g_tests.check(!bake(level, bad, &mesh), "inverted bounds are refused");
  bad = settings_for(200.0F, 200.0F);
  g_tests.check(!bake(level, bad, &mesh),
                "bounds past 1048576 columns are refused");
  bad = settings_for(5.0F, 5.0F);
  bad.maxSlopeDegrees = 90.0F;
  g_tests.check(!bake(level, bad, &mesh), "a 90 degree slope is refused");
  g_tests.check(
      !nav::bake_nav_mesh(settings_for(5.0F, 5.0F), nullptr, nullptr, &mesh),
      "a bake with no sampler is refused");
  g_tests.check(mesh.content_hash() == before,
                "a refused bake leaves the mesh unchanged");
}

void check_determinism() {
  Level level{};
  level.boxes.push_back(floor_box());
  level.boxes.push_back(Box{Vec3(-0.5F, 0.0F, -5.0F), Vec3(0.5F, 2.0F, 3.0F)});
  level.boxes.push_back(Box{Vec3(2.0F, 0.0F, -4.0F), Vec3(4.0F, 0.3F, -2.0F)});
  nav::NavMesh a{};
  nav::NavMesh b{};
  g_tests.check(bake(level, settings_for(5.0F, 5.0F), &a) &&
                    bake(level, settings_for(5.0F, 5.0F), &b) &&
                    (a.content_hash() == b.content_hash()),
                "the same level bakes to the same mesh");
  nav::NavQuery qa{};
  nav::NavQuery qb{};
  Vec3 pa[32] = {};
  Vec3 pb[32] = {};
  std::size_t ca = 0U;
  std::size_t cb = 0U;
  g_tests.check(
      qa.init(a) && qb.init(b) &&
          (qa.find_path(Vec3(-3.0F, 0.0F, -3.0F), Vec3(3.0F, 0.3F, -3.0F), pa,
                        32U, &ca) == nav::NavPathResult::Found) &&
          (qb.find_path(Vec3(-3.0F, 0.0F, -3.0F), Vec3(3.0F, 0.3F, -3.0F), pb,
                        32U, &cb) == nav::NavPathResult::Found) &&
          (ca == cb) && (std::memcmp(pa, pb, sizeof(Vec3) * ca) == 0),
      "the same query gives the same path, bit for bit");
}

} // namespace

int main() {
  check_flat_floor();
  check_wall();
  check_steps();
  check_slopes();
  check_low_ceiling();
  check_bridge();
  check_failures();
  check_determinism();
  return g_tests.finish("navigation mesh tests");
}
