// Verifies the collision triangle mesh: what a build refuses and drops, that
// the BVH's box and ray queries answer exactly what a scan of every
// triangle answers, that two builds of one input lay out identically, and
// that a mesh lives exactly as long as its last reference.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "engine/core/mem_tracker.h"
#include "engine/math/vec3.h"
#include "engine/physics/tri_mesh.h"

#include "../test_harness.h"

namespace {

namespace math = engine::math;
namespace physics = engine::physics;

engine::tests::TestContext g_tests{};

/// A wavy terrain of `n` x `n` quads, two triangles each, over [0, n].
struct Grid final {
  std::vector<math::Vec3> vertices;
  std::vector<std::uint32_t> indices;
};

Grid make_grid(std::uint32_t n) {
  Grid grid{};
  for (std::uint32_t z = 0U; z <= n; ++z) {
    for (std::uint32_t x = 0U; x <= n; ++x) {
      const float fx = static_cast<float>(x);
      const float fz = static_cast<float>(z);
      grid.vertices.emplace_back(fx, 0.25F * std::sin(fx * 0.7F + fz * 0.3F),
                                 fz);
    }
  }
  for (std::uint32_t z = 0U; z < n; ++z) {
    for (std::uint32_t x = 0U; x < n; ++x) {
      const std::uint32_t a = (z * (n + 1U)) + x;
      const std::uint32_t b = a + 1U;
      const std::uint32_t c = a + n + 1U;
      const std::uint32_t d = c + 1U;
      grid.indices.insert(grid.indices.end(), {a, c, b, b, c, d});
    }
  }
  return grid;
}

physics::TriMeshRef build(const Grid &grid,
                          physics::TriMeshBuildResult *result = nullptr) {
  physics::TriMeshRef mesh{};
  const physics::TriMeshBuildResult r =
      physics::build_tri_mesh(grid.vertices.data(), grid.vertices.size(),
                              grid.indices.data(), grid.indices.size(), &mesh);
  if (result != nullptr) {
    *result = r;
  }
  return mesh;
}

/// A small deterministic generator, so every run casts the same rays.
std::uint32_t next(std::uint32_t &state) {
  state = (state * 1664525U) + 1013904223U;
  return state >> 8U;
}

float unit(std::uint32_t &state) {
  return static_cast<float>(next(state) & 0xFFFFU) / 65535.0F;
}

bool boxes_meet(const math::AABB &a, const math::AABB &b) {
  return (a.min.x <= b.max.x) && (a.max.x >= b.min.x) && (a.min.y <= b.max.y) &&
         (a.max.y >= b.min.y) && (a.min.z <= b.max.z) && (a.max.z >= b.min.z);
}

math::AABB triangle_box(const physics::TriMeshData &mesh, std::uint32_t t) {
  math::AABB box{mesh.corner(t, 0U), mesh.corner(t, 0U)};
  for (std::size_t c = 1U; c < 3U; ++c) {
    const math::Vec3 &p = mesh.corner(t, c);
    box.min = math::Vec3(std::fmin(box.min.x, p.x), std::fmin(box.min.y, p.y),
                         std::fmin(box.min.z, p.z));
    box.max = math::Vec3(std::fmax(box.max.x, p.x), std::fmax(box.max.y, p.y),
                         std::fmax(box.max.z, p.z));
  }
  return box;
}

void check_refusals() {
  const math::Vec3 v[4] = {
      math::Vec3(0.0F, 0.0F, 0.0F), math::Vec3(1.0F, 0.0F, 0.0F),
      math::Vec3(0.0F, 0.0F, 1.0F), math::Vec3(2.0F, 0.0F, 0.0F)};
  physics::TriMeshRef mesh{};
  const std::uint32_t one[3] = {0U, 1U, 2U};
  g_tests.check(physics::build_tri_mesh(v, 4U, one, 0U, &mesh) ==
                    physics::TriMeshBuildResult::Empty,
                "no triangles is refused");
  g_tests.check(physics::build_tri_mesh(v, 4U, one, 2U, &mesh) ==
                    physics::TriMeshBuildResult::Empty,
                "an index count that is not a multiple of three is refused");
  const std::uint32_t outOfRange[3] = {0U, 1U, 4U};
  g_tests.check(physics::build_tri_mesh(v, 4U, outOfRange, 3U, &mesh) ==
                    physics::TriMeshBuildResult::IndexOutOfRange,
                "an index past the last vertex is refused");
  math::Vec3 bad[3] = {v[0], v[1], math::Vec3(NAN, 0.0F, 0.0F)};
  g_tests.check(physics::build_tri_mesh(bad, 3U, one, 3U, &mesh) ==
                    physics::TriMeshBuildResult::NonFinite,
                "a non-finite vertex is refused");
  const std::uint32_t collinear[3] = {0U, 1U, 3U};
  g_tests.check(physics::build_tri_mesh(v, 4U, collinear, 3U, &mesh) ==
                    physics::TriMeshBuildResult::AllDegenerate,
                "a mesh of only zero-area triangles is refused");
  // The count is checked before any index is read.
  g_tests.check(physics::build_tri_mesh(
                    v, 4U, one, (physics::TriMeshData::kMaxTriangles + 1U) * 3U,
                    &mesh) == physics::TriMeshBuildResult::TooManyTriangles,
                "one triangle past the capacity is refused");
  g_tests.check(!mesh, "a refused build leaves the reference empty");

  const std::uint32_t mixed[9] = {0U, 1U, 2U, 0U, 1U, 3U, 1U, 1U, 2U};
  g_tests.check(physics::build_tri_mesh(v, 4U, mixed, 9U, &mesh) ==
                        physics::TriMeshBuildResult::Ok &&
                    mesh && (mesh.get()->triangle_count() == 1U) &&
                    (mesh.get()->dropped_degenerate() == 2U),
                "zero-area triangles are dropped and counted");
}

void check_box_query_matches_scan() {
  const Grid grid = make_grid(48U);
  physics::TriMeshBuildResult result{};
  const physics::TriMeshRef ref = build(grid, &result);
  g_tests.check((result == physics::TriMeshBuildResult::Ok) && ref &&
                    (ref.get()->triangle_count() == 48U * 48U * 2U),
                "a 48x48 grid builds all 4608 triangles");
  if (!ref) {
    return;
  }
  const physics::TriMeshData &mesh = *ref.get();
  std::uint32_t seed = 7U;
  bool allMatch = true;
  bool ascending = true;
  std::vector<std::uint32_t> found(mesh.triangle_count());
  for (int q = 0; q < 200; ++q) {
    const math::Vec3 center(unit(seed) * 50.0F - 1.0F, unit(seed) - 0.5F,
                            unit(seed) * 50.0F - 1.0F);
    const math::Vec3 half(unit(seed) * 3.0F, unit(seed) * 0.5F,
                          unit(seed) * 3.0F);
    const math::AABB box{math::sub(center, half), math::add(center, half)};
    const std::size_t count = physics::collect_tri_mesh_triangles(
        mesh, box, found.data(), found.size());
    std::vector<std::uint32_t> expected{};
    for (std::uint32_t t = 0U; t < mesh.triangle_count(); ++t) {
      if (boxes_meet(triangle_box(mesh, t), box)) {
        expected.push_back(t);
      }
    }
    allMatch = allMatch && (count == expected.size()) &&
               std::equal(expected.begin(), expected.end(), found.begin());
    for (std::size_t i = 1U; i < count; ++i) {
      ascending = ascending && (found[i - 1U] < found[i]);
    }
  }
  g_tests.check(allMatch, "200 box queries find exactly the triangles a "
                          "scan of every triangle finds");
  g_tests.check(ascending, "a box query lists triangles in ascending order");

  const math::AABB everything{math::Vec3(-1.0F, -1.0F, -1.0F),
                              math::Vec3(49.0F, 1.0F, 49.0F)};
  std::uint32_t few[8] = {};
  g_tests.check(physics::collect_tri_mesh_triangles(
                    mesh, everything, few, 8U) == mesh.triangle_count() &&
                    (few[0] == 0U) && (few[7] == 7U),
                "a query past its buffer counts every match and keeps the "
                "first ones");
  const math::AABB away{math::Vec3(100.0F, 0.0F, 100.0F),
                        math::Vec3(101.0F, 1.0F, 101.0F)};
  g_tests.check(physics::collect_tri_mesh_triangles(mesh, away, few, 8U) == 0U,
                "a box beside the mesh meets nothing");
}

void check_raycast_matches_scan() {
  const Grid grid = make_grid(32U);
  const physics::TriMeshRef ref = build(grid);
  if (!ref) {
    g_tests.fail("the 32x32 grid builds");
    return;
  }
  const physics::TriMeshData &mesh = *ref.get();
  std::uint32_t seed = 11U;
  bool allMatch = true;
  int hits = 0;
  for (int r = 0; r < 400; ++r) {
    const math::Vec3 origin(unit(seed) * 34.0F - 1.0F, 2.0F + unit(seed),
                            unit(seed) * 34.0F - 1.0F);
    const math::Vec3 direction(unit(seed) - 0.5F, -1.0F, unit(seed) - 0.5F);
    physics::TriMeshRayHit hit{};
    const bool got =
        physics::raycast_tri_mesh(mesh, origin, direction, 10.0F, &hit);
    bool want = false;
    float bestT = 10.0F;
    std::uint32_t bestTriangle = 0U;
    for (std::uint32_t t = 0U; t < mesh.triangle_count(); ++t) {
      float candidate = 0.0F;
      if (physics::ray_triangle_intersect(
              origin, direction, mesh.corner(t, 0U), mesh.corner(t, 1U),
              mesh.corner(t, 2U), bestT, &candidate) &&
          (!want || (candidate < bestT))) {
        want = true;
        bestT = candidate;
        bestTriangle = t;
      }
    }
    // The same intersection routine runs in both, so the distance agrees
    // exactly and the triangle agrees by the lowest-index tie rule.
    allMatch = allMatch && (got == want) &&
               (!got || ((hit.t == bestT) && (hit.triangle == bestTriangle) &&
                         (math::dot(hit.normal, direction) <= 0.0F)));
    hits += got ? 1 : 0;
  }
  g_tests.check(allMatch, "400 rays hit exactly the nearest triangle a scan "
                          "of every triangle finds, normal facing the ray");
  g_tests.check(hits > 300, "most of the rays aimed at the mesh hit it");

  physics::TriMeshRayHit hit{};
  g_tests.check(!physics::raycast_tri_mesh(mesh, math::Vec3(5.0F, 2.0F, 5.0F),
                                           math::Vec3(0.0F, 1.0F, 0.0F), 10.0F,
                                           &hit),
                "a ray pointing away from the mesh misses it");
  g_tests.check(physics::raycast_tri_mesh(mesh, math::Vec3(5.5F, -2.0F, 5.5F),
                                          math::Vec3(0.0F, 1.0F, 0.0F), 10.0F,
                                          &hit) &&
                    (hit.normal.y < 0.0F),
                "a ray from below hits the mesh with the normal turned down");
  g_tests.check(!physics::raycast_tri_mesh(mesh, math::Vec3(5.5F, 2.0F, 5.5F),
                                           math::Vec3(0.0F, -1.0F, 0.0F), 1.0F,
                                           &hit),
                "a ray too short to reach the mesh misses it");
}

void check_closest_point() {
  const math::Vec3 a(0.0F, 0.0F, 0.0F);
  const math::Vec3 b(2.0F, 0.0F, 0.0F);
  const math::Vec3 c(0.0F, 0.0F, 2.0F);
  const math::Vec3 inside =
      physics::closest_point_on_triangle(math::Vec3(0.5F, 3.0F, 0.5F), a, b, c);
  const math::Vec3 vertex = physics::closest_point_on_triangle(
      math::Vec3(-1.0F, 1.0F, -1.0F), a, b, c);
  const math::Vec3 edge = physics::closest_point_on_triangle(
      math::Vec3(1.0F, 0.0F, -3.0F), a, b, c);
  g_tests.check((inside.x == 0.5F) && (inside.y == 0.0F) && (inside.z == 0.5F),
                "a point above the face projects onto it");
  g_tests.check((vertex.x == 0.0F) && (vertex.z == 0.0F),
                "a point beyond a corner is nearest that corner");
  g_tests.check((edge.x == 1.0F) && (edge.z == 0.0F),
                "a point beyond an edge is nearest that edge");
}

void check_identical_builds() {
  const Grid grid = make_grid(40U);
  const physics::TriMeshRef first = build(grid);
  const physics::TriMeshRef second = build(grid);
  if (!first || !second) {
    g_tests.fail("both builds of the grid succeed");
    return;
  }
  const physics::TriMeshData &a = *first.get();
  const physics::TriMeshData &b = *second.get();
  g_tests.check(
      (a.node_count() == b.node_count()) &&
          (std::memcmp(a.nodes(), b.nodes(),
                       a.node_count() * sizeof(physics::TriMeshNode)) == 0) &&
          (std::memcmp(a.triangles(), b.triangles(),
                       a.triangle_count() * sizeof(physics::TriMeshTriangle)) ==
           0),
      "two builds of one input lay out byte-identical BVHs");
  bool leavesSmall = true;
  for (std::size_t n = 0U; n < a.node_count(); ++n) {
    leavesSmall = leavesSmall &&
                  (a.nodes()[n].count <= physics::TriMeshData::kLeafTriangles);
  }
  g_tests.check(leavesSmall && (a.node_count() < 2U * a.triangle_count()),
                "no leaf holds more than four triangles");
}

void check_reference_lifetime() {
  const std::int64_t before =
      engine::core::mem_tracker_current_bytes(engine::core::MemTag::Physics);
  {
    const Grid grid = make_grid(8U);
    physics::TriMeshRef owner = build(grid);
    const std::int64_t built =
        engine::core::mem_tracker_current_bytes(engine::core::MemTag::Physics);
    g_tests.check(built > before, "a built mesh is tracked as physics memory");
    physics::TriMeshRef copy = owner;
    physics::TriMeshRef moved = std::move(copy);
    owner.reset();
    g_tests.check(!owner && !copy && moved &&
                      (moved.get()->triangle_count() == 128U),
                  "a mesh outlives the references dropped before its last");
    physics::TriMeshRef assigned{};
    assigned = moved;
    const physics::TriMeshRef &alias = assigned;
    assigned = alias;
    moved.reset();
    g_tests.check(assigned && (assigned.get()->triangle_count() == 128U),
                  "assigning a reference to itself keeps the mesh");
  }
  g_tests.check(engine::core::mem_tracker_current_bytes(
                    engine::core::MemTag::Physics) == before,
                "the last reference frees every byte the mesh held");
}

} // namespace

/// Runs the triangle mesh suite.
int main() {
  engine::core::mem_tracker_init();
  check_refusals();
  check_box_query_matches_scan();
  check_raycast_matches_scan();
  check_closest_point();
  check_identical_builds();
  check_reference_lifetime();
  return g_tests.finish("tri mesh");
}
