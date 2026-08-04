// Implements convex hull behavior for the Engine physics system.

#include "engine/physics/convex_hull.h"

#include "engine/math/vec3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::physics {
namespace {

// ------------- Quickhull implementation ------------------------------------
// Simplified incremental convex hull builder.  Sufficient for up to ~128
// input points with up to 64 output faces — the engine's collider budget.

// Working-set budgets during the build; output is clamped to ConvexHullData's
// tighter limits when surviving faces are collected.
static constexpr std::size_t kMaxFaces = 256U;
static constexpr std::size_t kMaxEdges = 512U;

// Build-time triangle face. Faces wind counter-clockwise seen from outside,
// so adjacent faces share each edge with reversed winding — horizon-edge
// detection relies on that to find the neighbor across an edge.
struct HullFace {
  std::uint16_t v[3]{};
  math::Vec3 normal{};
  // Plane distance from the origin.
  float dist = 0.0F;
  bool alive = true;
};

// Compute plane from 3 vertices.
void face_plane(HullFace &f, const math::Vec3 *verts) noexcept {
  const math::Vec3 ab = math::sub(verts[f.v[1]], verts[f.v[0]]);
  const math::Vec3 ac = math::sub(verts[f.v[2]], verts[f.v[0]]);
  f.normal = math::cross(ab, ac);
  const float len = math::length(f.normal);
  if (len > 1e-10F) {
    f.normal = math::mul(f.normal, 1.0F / len);
  }
  f.dist = math::dot(f.normal, verts[f.v[0]]);
}

// Signed distance of a point from the face plane (positive = outside).
float point_plane_distance(const math::Vec3 &point,
                           const HullFace &face) noexcept {
  return math::dot(face.normal, point) - face.dist;
}

} // namespace

/// Quickhull: seeds a tetrahedron from extremal points (rejecting degenerate,
/// collinear, and coplanar input), incrementally adds the remaining points by
/// killing visible faces and re-facing their horizon edges, then compacts the
/// surviving outward-oriented faces and their vertices into the output.
bool build_convex_hull(const math::Vec3 *points, std::size_t pointCount,
                       ConvexHullData &outHull) noexcept {
  if ((points == nullptr) || (pointCount < 4U)) {
    return false;
  }

  const std::size_t maxPts = ConvexHullData::kMaxVertices;
  const std::size_t count = (pointCount > maxPts) ? maxPts : pointCount;

  std::array<math::Vec3, ConvexHullData::kMaxVertices> verts{};
  for (std::size_t i = 0U; i < count; ++i) {
    verts[i] = points[i];
  }

  std::size_t i0 = 0U;
  std::size_t i1 = 1U;
  float bestDist2 = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    for (std::size_t j = i + 1U; j < count; ++j) {
      const float d2 = math::length_sq(math::sub(verts[j], verts[i]));
      if (d2 > bestDist2) {
        bestDist2 = d2;
        i0 = i;
        i1 = j;
      }
    }
  }

  if (bestDist2 < 1e-12F) {
    return false;
  }

  const math::Vec3 lineDir = math::normalize(math::sub(verts[i1], verts[i0]));
  std::size_t i2 = 0U;
  float maxLineDist = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    if (i == i0 || i == i1) {
      continue;
    }
    const math::Vec3 diff = math::sub(verts[i], verts[i0]);
    const float proj = math::dot(diff, lineDir);
    const math::Vec3 onLine = math::add(verts[i0], math::mul(lineDir, proj));
    const float d2 = math::length_sq(math::sub(verts[i], onLine));
    if (d2 > maxLineDist) {
      maxLineDist = d2;
      i2 = i;
    }
  }

  if (maxLineDist < 1e-12F) {
    return false;
  }

  math::Vec3 triNormal = math::cross(math::sub(verts[i1], verts[i0]),
                                     math::sub(verts[i2], verts[i0]));
  triNormal = math::normalize(triNormal);

  std::size_t i3 = 0U;
  float maxPlaneDist = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    if (i == i0 || i == i1 || i == i2) {
      continue;
    }
    const float d =
        std::fabs(math::dot(math::sub(verts[i], verts[i0]), triNormal));
    if (d > maxPlaneDist) {
      maxPlaneDist = d;
      i3 = i;
    }
  }

  if (maxPlaneDist < 1e-12F) {
    return false;
  }

  if (math::dot(math::sub(verts[i3], verts[i0]), triNormal) > 0.0F) {
    const std::size_t tmp = i1;
    i1 = i2;
    i2 = tmp;
  }

  std::array<HullFace, kMaxFaces> faces{};
  std::size_t faceCount = 0U;

  auto add_face = [&](std::size_t a, std::size_t b,
                      std::size_t c) -> std::size_t {
    if (faceCount >= kMaxFaces) {
      return kMaxFaces;
    }
    HullFace &f = faces[faceCount];
    f.v[0] = static_cast<std::uint16_t>(a);
    f.v[1] = static_cast<std::uint16_t>(b);
    f.v[2] = static_cast<std::uint16_t>(c);
    f.alive = true;
    face_plane(f, verts.data());
    return faceCount++;
  };

  add_face(i0, i1, i2);
  add_face(i0, i2, i3);
  add_face(i0, i3, i1);
  add_face(i1, i3, i2);

  {
    const math::Vec3 centroid =
        math::mul(math::add(math::add(verts[i0], verts[i1]),
                            math::add(verts[i2], verts[i3])),
                  0.25F);
    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      const math::Vec3 facePt = verts[faces[fi].v[0]];
      if (math::dot(math::sub(facePt, centroid), faces[fi].normal) < 0.0F) {
        const auto tmp = faces[fi].v[0];
        faces[fi].v[0] = faces[fi].v[1];
        faces[fi].v[1] = tmp;
        face_plane(faces[fi], verts.data());
      }
    }
  }

  for (std::size_t pi = 0U; pi < count; ++pi) {
    if (pi == i0 || pi == i1 || pi == i2 || pi == i3) {
      continue;
    }

    bool anyVisible = false;
    std::array<bool, kMaxFaces> visible{};
    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (!faces[fi].alive) {
        continue;
      }
      if (point_plane_distance(verts[pi], faces[fi]) > 1e-6F) {
        visible[fi] = true;
        anyVisible = true;
      }
    }

    if (!anyVisible) {
      continue;
    }

    struct Edge {
      std::uint16_t a, b;
    };
    std::array<Edge, kMaxEdges> horizon{};
    std::size_t horizonCount = 0U;

    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (!visible[fi] || !faces[fi].alive) {
        continue;
      }
      for (int e = 0; e < 3; ++e) {
        const std::uint16_t ea = faces[fi].v[e];
        const std::uint16_t eb = faces[fi].v[(e + 1) % 3];
        bool neighborVisible = false;
        for (std::size_t fj = 0U; fj < faceCount; ++fj) {
          if (fj == fi || !faces[fj].alive) {
            continue;
          }
          for (int e2 = 0; e2 < 3; ++e2) {
            if ((faces[fj].v[e2] == eb) && (faces[fj].v[(e2 + 1) % 3] == ea)) {
              neighborVisible = visible[fj];
              goto found_neighbor;
            }
          }
        }
      found_neighbor:
        if (!neighborVisible && horizonCount < kMaxEdges) {
          horizon[horizonCount++] = {ea, eb};
        }
      }
    }

    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (visible[fi]) {
        faces[fi].alive = false;
      }
    }

    for (std::size_t ei = 0U; ei < horizonCount; ++ei) {
      add_face(horizon[ei].a, horizon[ei].b, static_cast<std::uint16_t>(pi));
    }
  }

  outHull.planeCount = 0U;
  outHull.vertexCount = 0U;

  std::array<std::int16_t, ConvexHullData::kMaxVertices> vertRemap{};
  for (auto &v : vertRemap) {
    v = -1;
  }

  for (std::size_t fi = 0U; fi < faceCount; ++fi) {
    if (!faces[fi].alive) {
      continue;
    }
    if (outHull.planeCount >= ConvexHullData::kMaxPlanes) {
      break;
    }
    ConvexHullData::Plane &plane = outHull.planes[outHull.planeCount++];
    plane.normal = faces[fi].normal;
    plane.distance = faces[fi].dist;

    for (int vi = 0; vi < 3; ++vi) {
      const auto idx = faces[fi].v[vi];
      if ((vertRemap[idx] < 0) &&
          (outHull.vertexCount < ConvexHullData::kMaxVertices)) {
        vertRemap[idx] = static_cast<std::int16_t>(outHull.vertexCount);
        outHull.vertices[outHull.vertexCount++] = verts[idx];
      }
    }
  }

  if (outHull.planeCount < 4U || outHull.vertexCount < 4U) {
    return false;
  }

  math::Vec3 minV = outHull.vertices[0];
  math::Vec3 maxV = outHull.vertices[0];
  for (std::size_t i = 1U; i < outHull.vertexCount; ++i) {
    const math::Vec3 &v = outHull.vertices[i];
    minV.x = std::min(minV.x, v.x);
    minV.y = std::min(minV.y, v.y);
    minV.z = std::min(minV.z, v.z);
    maxV.x = std::max(maxV.x, v.x);
    maxV.y = std::max(maxV.y, v.y);
    maxV.z = std::max(maxV.z, v.z);
  }
  outHull.localCenter = math::mul(math::add(minV, maxV), 0.5F);
  outHull.localHalfExtents = math::mul(math::sub(maxV, minV), 0.5F);

  return true;
}

// ------------- GJK / EPA implementation ------------------------------------

namespace {

static constexpr std::size_t kGjkMaxIter = 64U;
static constexpr std::size_t kEpaMaxIter = 64U;
static constexpr std::size_t kEpaMaxFaces = 128U;
static constexpr std::size_t kEpaMaxVertices = 256U;
static constexpr std::size_t kEpaMaxHorizon = 256U;
static constexpr float kEpaTolerance = 1e-4F;

// World-space length (metres) below which two Minkowski samples are the same
// point and a residual counts as zero. Engine geometry is metre-scale, so a
// micrometre is far below any contact the solver acts on.
static constexpr float kDegenerateDistance = 1e-6F;

// The origin must clear every seed face by this much before EPA runs: EPA
// orients each face by the sign of its origin distance, so a face plane
// through the origin would wind against its neighbours and break the horizon
// walk. Contacts shallower than this are discarded by the narrow phase's
// 1e-6 depth gate anyway.
static constexpr float kSeedInteriorMargin = 1e-7F;

// Exact cos/sin of 120 degrees for the three search directions spaced around
// a degenerate simplex's axis. Fixed constants keep the expansion
// bit-identical across runs and platforms — no perturbation, no randomness.
static constexpr float kCos120 = -0.5F;
static constexpr float kSin120 = 0.86602540378443865F;

// Minkowski difference sample: v = a − b, keeping the originating support
// points on each shape for contact reconstruction.
struct MinkowskiPoint {
  math::Vec3 v{};
  math::Vec3 a{};
  math::Vec3 b{};
};

// Samples the Minkowski difference support in the given direction.
MinkowskiPoint support(const void *shapeA, const math::Vec3 &centerA,
                       SupportFn supA, const void *shapeB,
                       const math::Vec3 &centerB, SupportFn supB,
                       const math::Vec3 &dir) noexcept {
  MinkowskiPoint mp;
  mp.a = supA(shapeA, centerA, dir);
  mp.b = supB(shapeB, centerB, math::mul(dir, -1.0F));
  mp.v = math::sub(mp.a, mp.b);
  return mp;
}

// GJK simplex: newest point always sits at the front.
struct Simplex {
  std::array<MinkowskiPoint, 4> pts{};
  int size = 0;

  // Inserts at the front, shifting existing points back.
  void push(const MinkowskiPoint &p) noexcept {
    for (int i = size; i > 0; --i) {
      pts[i] = pts[i - 1];
    }
    pts[0] = p;
    if (size < 4) {
      ++size;
    }
  }
};

// Line case: keeps the closest feature and aims dir back at the origin.
bool do_simplex_line(Simplex &s, math::Vec3 &dir) noexcept {
  const math::Vec3 ab = math::sub(s.pts[1].v, s.pts[0].v);
  const math::Vec3 ao = math::mul(s.pts[0].v, -1.0F);
  if (math::dot(ab, ao) > 0.0F) {
    dir = math::cross(math::cross(ab, ao), ab);
  } else {
    s.size = 1;
    dir = ao;
  }
  return false;
}

// Triangle case: reduces to the edge/face region containing the origin.
bool do_simplex_triangle(Simplex &s, math::Vec3 &dir) noexcept {
  const math::Vec3 &a = s.pts[0].v;
  const math::Vec3 &b = s.pts[1].v;
  const math::Vec3 &c = s.pts[2].v;
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ac = math::sub(c, a);
  const math::Vec3 ao = math::mul(a, -1.0F);
  const math::Vec3 abc = math::cross(ab, ac);

  if (math::dot(math::cross(abc, ac), ao) > 0.0F) {
    if (math::dot(ac, ao) > 0.0F) {
      s.pts[1] = s.pts[2];
      s.size = 2;
      dir = math::cross(math::cross(ac, ao), ac);
    } else {
      s.size = 2;
      return do_simplex_line(s, dir);
    }
  } else {
    if (math::dot(math::cross(ab, abc), ao) > 0.0F) {
      s.size = 2;
      return do_simplex_line(s, dir);
    } else {
      if (math::dot(abc, ao) > 0.0F) {
        dir = abc;
      } else {
        const MinkowskiPoint tmp = s.pts[1];
        s.pts[1] = s.pts[2];
        s.pts[2] = tmp;
        dir = math::mul(abc, -1.0F);
      }
    }
  }
  return false;
}

// Tetrahedron case: true when the origin is enclosed, otherwise reduces to
// the face whose outside contains the origin.
bool do_simplex_tetrahedron(Simplex &s, math::Vec3 &dir) noexcept {
  const math::Vec3 &a = s.pts[0].v;
  const math::Vec3 &b = s.pts[1].v;
  const math::Vec3 &c = s.pts[2].v;
  const math::Vec3 &d = s.pts[3].v;
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ac = math::sub(c, a);
  const math::Vec3 ad = math::sub(d, a);
  const math::Vec3 ao = math::mul(a, -1.0F);

  const math::Vec3 abc = math::cross(ab, ac);
  const math::Vec3 acd = math::cross(ac, ad);
  const math::Vec3 adb = math::cross(ad, ab);

  if (math::dot(abc, ao) > 0.0F) {
    s.size = 3;
    return do_simplex_triangle(s, dir);
  }
  if (math::dot(acd, ao) > 0.0F) {
    s.pts[1] = s.pts[2];
    s.pts[2] = s.pts[3];
    s.size = 3;
    return do_simplex_triangle(s, dir);
  }
  if (math::dot(adb, ao) > 0.0F) {
    s.pts[2] = s.pts[1];
    s.pts[1] = s.pts[3];
    s.size = 3;
    return do_simplex_triangle(s, dir);
  }
  return true;
}

// Dispatches to the case handler for the current simplex dimension.
bool do_simplex(Simplex &s, math::Vec3 &dir) noexcept {
  switch (s.size) {
  case 2:
    return do_simplex_line(s, dir);
  case 3:
    return do_simplex_triangle(s, dir);
  case 4:
    return do_simplex_tetrahedron(s, dir);
  default:
    return false;
  }
}

// EPA (Expanding Polytope Algorithm) — computes penetration depth and normal
// from the GJK simplex (which must be a tetrahedron enclosing the origin).

struct EpaFace {
  std::uint16_t v[3]{};
  math::Vec3 normal{};
  float dist = 0.0F;
  bool alive = true;
};

// Plane from the face vertices with the normal oriented away from the origin.
// Reports false for a zero-area face: its cross product cannot be normalized,
// so both the normal and the plane distance would be meaningless and would
// poison the closest-face search (issue #72, finding 2).
[[nodiscard]] bool
epa_face_plane(EpaFace &f,
               const std::array<MinkowskiPoint, kEpaMaxVertices> &verts,
               std::size_t vertCount) noexcept {
  if ((f.v[0] >= vertCount) || (f.v[1] >= vertCount) ||
      (f.v[2] >= vertCount)) {
    return false;
  }
  const math::Vec3 ab = math::sub(verts[f.v[1]].v, verts[f.v[0]].v);
  const math::Vec3 ac = math::sub(verts[f.v[2]].v, verts[f.v[0]].v);
  f.normal = math::cross(ab, ac);
  const float len = math::length(f.normal);
  if (!(len > 1e-10F) || !std::isfinite(len)) {
    return false;
  }
  f.normal = math::mul(f.normal, 1.0F / len);
  f.dist = math::dot(f.normal, verts[f.v[0]].v);
  if (!std::isfinite(f.dist)) {
    return false;
  }
  if (f.dist < 0.0F) {
    f.dist = -f.dist;
    f.normal = math::mul(f.normal, -1.0F);
    const auto tmp = f.v[0];
    f.v[0] = f.v[1];
    f.v[1] = tmp;
  }
  return true;
}

// Origin-enclosing polytope handed to EPA: either the tetrahedron GJK
// terminated on or the bipyramid built from a degenerate simplex. Winding is
// left to epa_face_plane's away-from-origin orientation, which is the
// polytope's outward orientation exactly when the origin is strictly inside.
struct EpaSeed final {
  std::array<MinkowskiPoint, 5> verts{};
  std::size_t vertCount = 0U;
  std::array<std::array<std::uint16_t, 3>, 6> faces{};
  std::size_t faceCount = 0U;
};

/// Appends one seed face.
void seed_add_face(EpaSeed &seed, std::uint16_t a, std::uint16_t b,
                   std::uint16_t c) noexcept {
  if (seed.faceCount >= seed.faces.size()) {
    return;
  }
  seed.faces[seed.faceCount] = {a, b, c};
  ++seed.faceCount;
}

/// Closes a five-vertex bipyramid: two apexes over an equator triangle.
void seed_bipyramid(EpaSeed &seed, std::uint16_t apex0, std::uint16_t apex1,
                    std::uint16_t e0, std::uint16_t e1,
                    std::uint16_t e2) noexcept {
  seed_add_face(seed, apex0, e0, e1);
  seed_add_face(seed, apex0, e1, e2);
  seed_add_face(seed, apex0, e2, e0);
  seed_add_face(seed, apex1, e0, e1);
  seed_add_face(seed, apex1, e1, e2);
  seed_add_face(seed, apex1, e2, e0);
}

// Origin containment. The vertex centroid of a full-dimensional convex
// polytope is interior, so the origin is enclosed exactly when no face plane
// separates it from the centroid; a face plane through the origin is allowed
// because EPA expands such a face out to the real boundary. Seeds whose
// centroid lies on a face plane are flat and are rejected, as are seeds with
// a zero-area face, since neither can be oriented or expanded.
[[nodiscard]] bool seed_encloses_origin(const EpaSeed &seed) noexcept {
  if ((seed.vertCount < 4U) || (seed.faceCount < 4U)) {
    return false;
  }

  math::Vec3 centroid(0.0F, 0.0F, 0.0F);
  for (std::size_t i = 0U; i < seed.vertCount; ++i) {
    centroid = math::add(centroid, seed.verts[i].v);
  }
  centroid = math::mul(centroid, 1.0F / static_cast<float>(seed.vertCount));

  for (std::size_t fi = 0U; fi < seed.faceCount; ++fi) {
    const auto &face = seed.faces[fi];
    if ((face[0] >= seed.vertCount) || (face[1] >= seed.vertCount) ||
        (face[2] >= seed.vertCount)) {
      return false;
    }
    const math::Vec3 &v0 = seed.verts[face[0]].v;
    math::Vec3 normal = math::cross(math::sub(seed.verts[face[1]].v, v0),
                                    math::sub(seed.verts[face[2]].v, v0));
    const float lengthSq = math::dot(normal, normal);
    if (!(lengthSq > 0.0F) || !std::isfinite(lengthSq)) {
      return false;
    }
    normal = math::mul(normal, 1.0F / std::sqrt(lengthSq));

    const float originSide = math::dot(normal, math::mul(v0, -1.0F));
    const float centroidSide = math::dot(normal, math::sub(centroid, v0));
    if (!(std::fabs(centroidSide) > kSeedInteriorMargin)) {
      return false;
    }
    const bool separated =
        ((originSide > kSeedInteriorMargin) && (centroidSide < 0.0F)) ||
        ((originSide < -kSeedInteriorMargin) && (centroidSide > 0.0F));
    if (separated) {
      return false;
    }
  }
  return true;
}

// Finds the live face nearest the origin. Reports false when the polytope
// holds no live face with a finite distance, which means expansion corrupted
// it and no depth may be trusted (issue #72, finding 2 — the old code
// returned its 1e30 sentinel as the penetration depth).
[[nodiscard]] bool
epa_closest_face(const std::array<EpaFace, kEpaMaxFaces> &faces,
                 std::size_t faceCount, std::size_t *outIndex) noexcept {
  bool found = false;
  float closestDist = 0.0F;
  for (std::size_t fi = 0U; fi < faceCount; ++fi) {
    if (!faces[fi].alive || !std::isfinite(faces[fi].dist)) {
      continue;
    }
    if (!found || (faces[fi].dist < closestDist)) {
      closestDist = faces[fi].dist;
      *outIndex = fi;
      found = true;
    }
  }
  return found;
}

/// Fills the penetration result from one face; the contact point is
/// approximated as the centroid of that face's A-side support points.
void epa_fill_result(const EpaFace &face,
                     const std::array<MinkowskiPoint, kEpaMaxVertices> &verts,
                     GjkResult *result) noexcept {
  result->normal = face.normal;
  result->depth = face.dist;
  result->contactPoint = math::mul(
      math::add(verts[face.v[0]].a,
                math::add(verts[face.v[1]].a, verts[face.v[2]].a)),
      1.0F / 3.0F);
}

// Expands the seeded polytope toward the closest boundary face until the
// support distance converges (or the face/vertex budget or iteration cap is
// hit, in which case the current closest face is the answer — a lower bound
// on the true depth, which is the conservative direction). Every budget
// overflow now stops expansion instead of silently dropping faces or horizon
// edges, which used to leave an open mesh behind.
GjkResult epa(const EpaSeed &seed, const void *shapeA,
              const math::Vec3 &centerA, SupportFn supA, const void *shapeB,
              const math::Vec3 &centerB, SupportFn supB) noexcept {
  GjkResult result;
  result.intersecting = true;

  std::array<MinkowskiPoint, kEpaMaxVertices> verts{};
  std::size_t vertCount = seed.vertCount;
  for (std::size_t i = 0U; i < seed.vertCount; ++i) {
    verts[i] = seed.verts[i];
  }

  std::array<EpaFace, kEpaMaxFaces> faces{};
  std::size_t faceCount = 0U;

  auto add_epa_face = [&](std::uint16_t a, std::uint16_t b,
                          std::uint16_t c) noexcept -> bool {
    if (faceCount >= kEpaMaxFaces) {
      return false;
    }
    EpaFace &f = faces[faceCount];
    f.v[0] = a;
    f.v[1] = b;
    f.v[2] = c;
    f.alive = true;
    if (!epa_face_plane(f, verts, vertCount)) {
      return false;
    }
    ++faceCount;
    return true;
  };

  for (std::size_t fi = 0U; fi < seed.faceCount; ++fi) {
    static_cast<void>(add_epa_face(seed.faces[fi][0], seed.faces[fi][1],
                                   seed.faces[fi][2]));
  }
  if (faceCount != seed.faceCount) {
    return result;
  }

  std::size_t closestIdx = 0U;
  for (std::size_t iter = 0U; iter < kEpaMaxIter; ++iter) {
    if (!epa_closest_face(faces, faceCount, &closestIdx)) {
      return result;
    }

    const math::Vec3 searchDir = faces[closestIdx].normal;
    const float closestDist = faces[closestIdx].dist;

    const MinkowskiPoint newPt =
        support(shapeA, centerA, supA, shapeB, centerB, supB, searchDir);
    const float newDist = math::dot(newPt.v, searchDir);

    if (!std::isfinite(newDist) || ((newDist - closestDist) < kEpaTolerance) ||
        (vertCount >= kEpaMaxVertices)) {
      epa_fill_result(faces[closestIdx], verts, &result);
      return result;
    }

    const auto newIdx = static_cast<std::uint16_t>(vertCount);
    verts[vertCount] = newPt;
    ++vertCount;

    struct Edge {
      std::uint16_t a, b;
    };
    std::array<Edge, kEpaMaxHorizon> horizon{};
    std::size_t horizonCount = 0U;
    bool horizonOverflow = false;

    std::array<bool, kEpaMaxFaces> visible{};
    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (!faces[fi].alive) {
        continue;
      }
      if (math::dot(faces[fi].normal,
                    math::sub(newPt.v, verts[faces[fi].v[0]].v)) > 1e-6F) {
        visible[fi] = true;
      }
    }

    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (!visible[fi] || !faces[fi].alive) {
        continue;
      }
      for (int e = 0; e < 3; ++e) {
        const std::uint16_t ea = faces[fi].v[e];
        const std::uint16_t eb = faces[fi].v[(e + 1) % 3];
        bool neighborVisible = false;
        for (std::size_t fj = 0U; fj < faceCount; ++fj) {
          if (fj == fi || !faces[fj].alive) {
            continue;
          }
          for (int e2 = 0; e2 < 3; ++e2) {
            if ((faces[fj].v[e2] == eb) && (faces[fj].v[(e2 + 1) % 3] == ea)) {
              neighborVisible = visible[fj];
              goto epa_found_neighbor;
            }
          }
        }
      epa_found_neighbor:
        if (!neighborVisible) {
          if (horizonCount >= kEpaMaxHorizon) {
            horizonOverflow = true;
          } else {
            horizon[horizonCount] = {ea, eb};
            ++horizonCount;
          }
        }
      }
    }

    if (horizonOverflow) {
      epa_fill_result(faces[closestIdx], verts, &result);
      return result;
    }

    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (visible[fi]) {
        faces[fi].alive = false;
      }
    }

    bool expansionComplete = true;
    for (std::size_t ei = 0U; ei < horizonCount; ++ei) {
      if (!add_epa_face(horizon[ei].a, horizon[ei].b, newIdx)) {
        expansionComplete = false;
        break;
      }
    }
    if (!expansionComplete) {
      break;
    }
  }

  if (epa_closest_face(faces, faceCount, &closestIdx)) {
    epa_fill_result(faces[closestIdx], verts, &result);
  }
  return result;
}

/// Seeds EPA from the tetrahedron GJK terminated on.
EpaSeed tetrahedron_seed(const Simplex &simplex) noexcept {
  EpaSeed seed;
  for (std::size_t i = 0U; i < 4U; ++i) {
    seed.verts[i] = simplex.pts[i];
  }
  seed.vertCount = 4U;
  seed_add_face(seed, 0, 1, 2);
  seed_add_face(seed, 0, 2, 3);
  seed_add_face(seed, 0, 3, 1);
  seed_add_face(seed, 1, 3, 2);
  return seed;
}

// Deterministic unit vector orthogonal to a unit axis: crossing with the
// axis's smallest-magnitude basis direction keeps the cross product far from
// zero. The comparison chain is on magnitudes only, so it cannot flip with
// the floating-point mode.
math::Vec3 orthogonal_unit(const math::Vec3 &axis) noexcept {
  const float ax = std::fabs(axis.x);
  const float ay = std::fabs(axis.y);
  const float az = std::fabs(axis.z);
  math::Vec3 basis(0.0F, 0.0F, 1.0F);
  if ((ax <= ay) && (ax <= az)) {
    basis = math::Vec3(1.0F, 0.0F, 0.0F);
  } else if (ay <= az) {
    basis = math::Vec3(0.0F, 1.0F, 0.0F);
  }
  const math::Vec3 result = math::cross(axis, basis);
  const float lengthSq = math::dot(result, result);
  if (!(lengthSq > 0.0F) || !std::isfinite(lengthSq)) {
    return math::Vec3(1.0F, 0.0F, 0.0F);
  }
  return math::mul(result, 1.0F / std::sqrt(lengthSq));
}

/// Rotates a vector already perpendicular to a unit axis about that axis
/// (Rodrigues without the axis-parallel term, which is zero here).
math::Vec3 rotate_about_axis(const math::Vec3 &v, const math::Vec3 &axis,
                             float cosAngle, float sinAngle) noexcept {
  return math::add(math::mul(v, cosAngle),
                   math::mul(math::cross(axis, v), sinAngle));
}

/// Collects the simplex's distinct points, newest first.
std::size_t distinct_simplex_points(
    const Simplex &simplex,
    std::array<MinkowskiPoint, 4> *outPoints) noexcept {
  const float duplicateSq = kDegenerateDistance * kDegenerateDistance;
  std::size_t count = 0U;
  for (int i = 0; i < simplex.size; ++i) {
    bool duplicate = false;
    for (std::size_t j = 0U; j < count; ++j) {
      if (math::length_sq(math::sub(simplex.pts[static_cast<std::size_t>(i)].v,
                                    (*outPoints)[j].v)) <= duplicateSq) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      (*outPoints)[count] = simplex.pts[static_cast<std::size_t>(i)];
      ++count;
    }
  }
  return count;
}

// Grows the bipyramid around a segment that carries the origin: three
// supports spaced 120 degrees around the segment axis surround it, so the
// origin — strictly between the endpoints — lands inside the hull.
void seed_from_segment(EpaSeed &seed, const MinkowskiPoint &a,
                       const MinkowskiPoint &b, const math::Vec3 &axis,
                       const void *shapeA, const math::Vec3 &centerA,
                       SupportFn supA, const void *shapeB,
                       const math::Vec3 &centerB, SupportFn supB) noexcept {
  const math::Vec3 first = orthogonal_unit(axis);
  const math::Vec3 second = rotate_about_axis(first, axis, kCos120, kSin120);
  const math::Vec3 third = rotate_about_axis(first, axis, kCos120, -kSin120);

  seed.verts[0] = a;
  seed.verts[1] = b;
  seed.verts[2] =
      support(shapeA, centerA, supA, shapeB, centerB, supB, first);
  seed.verts[3] =
      support(shapeA, centerA, supA, shapeB, centerB, supB, second);
  seed.verts[4] =
      support(shapeA, centerA, supA, shapeB, centerB, supB, third);
  seed.vertCount = 5U;
  seed_bipyramid(seed, 0, 1, 2, 3, 4);
}

// Grows the bipyramid around a coplanar triangle that carries the origin:
// the supports along both face normals lift the polytope off the plane.
void seed_from_triangle(EpaSeed &seed, const MinkowskiPoint &a,
                        const MinkowskiPoint &b, const MinkowskiPoint &c,
                        const math::Vec3 &normal, const void *shapeA,
                        const math::Vec3 &centerA, SupportFn supA,
                        const void *shapeB, const math::Vec3 &centerB,
                        SupportFn supB) noexcept {
  seed.verts[0] = a;
  seed.verts[1] = b;
  seed.verts[2] = c;
  seed.verts[3] =
      support(shapeA, centerA, supA, shapeB, centerB, supB, normal);
  seed.verts[4] = support(shapeA, centerA, supA, shapeB, centerB, supB,
                          math::mul(normal, -1.0F));
  seed.vertCount = 5U;
  seed_bipyramid(seed, 3, 4, 0, 1, 2);
}

// Recovers a contact when GJK stops without a tetrahedron. Exactly
// axis-aligned pairs are the reachable case (issue #72): both support
// functions resolve a zero direction component to the same corner, so the
// component cancels in every Minkowski sample, the samples stay collinear
// or coplanar, and no tetrahedron can form even though the origin is
// enclosed. Finding the origin on the simplex's segment or triangle PROVES
// intersection — those points lie in the Minkowski difference, which is
// convex — and the polytope EPA needs is then grown from fixed support
// directions. Nothing is perturbed, so the outcome is bit-identical across
// runs, thread counts, and platforms.
GjkResult resolve_degenerate_simplex(const Simplex &simplex,
                                     const void *shapeA,
                                     const math::Vec3 &centerA, SupportFn supA,
                                     const void *shapeB,
                                     const math::Vec3 &centerB,
                                     SupportFn supB) noexcept {
  GjkResult result;

  std::array<MinkowskiPoint, 4> points{};
  const std::size_t count = distinct_simplex_points(simplex, &points);
  if (count == 0U) {
    return result;
  }

  const float toleranceSq = kDegenerateDistance * kDegenerateDistance;
  if (count == 1U) {
    result.intersecting = math::length_sq(points[0].v) <= toleranceSq;
    return result;
  }

  std::size_t farA = 0U;
  std::size_t farB = 1U;
  float longestSq = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    for (std::size_t j = i + 1U; j < count; ++j) {
      const float lengthSq =
          math::length_sq(math::sub(points[j].v, points[i].v));
      if (lengthSq > longestSq) {
        longestSq = lengthSq;
        farA = i;
        farB = j;
      }
    }
  }
  if (longestSq <= toleranceSq) {
    result.intersecting = math::length_sq(points[0].v) <= toleranceSq;
    return result;
  }

  const MinkowskiPoint &segmentStart = points[farA];
  const MinkowskiPoint &segmentEnd = points[farB];
  const float segmentLength = std::sqrt(longestSq);
  const math::Vec3 axis =
      math::mul(math::sub(segmentEnd.v, segmentStart.v), 1.0F / segmentLength);

  std::size_t offLine = count;
  float farthestOffLine = 0.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    if ((i == farA) || (i == farB)) {
      continue;
    }
    const math::Vec3 relative = math::sub(points[i].v, segmentStart.v);
    const math::Vec3 perpendicular =
        math::sub(relative, math::mul(axis, math::dot(relative, axis)));
    const float distance = math::length(perpendicular);
    if (distance > farthestOffLine) {
      farthestOffLine = distance;
      offLine = i;
    }
  }

  EpaSeed seed;
  if ((offLine < count) && (farthestOffLine > kDegenerateDistance)) {
    const MinkowskiPoint &third = points[offLine];
    math::Vec3 normal =
        math::cross(math::sub(segmentEnd.v, segmentStart.v),
                    math::sub(third.v, segmentStart.v));
    const float normalLengthSq = math::dot(normal, normal);
    if (!(normalLengthSq > 0.0F) || !std::isfinite(normalLengthSq)) {
      return result;
    }
    normal = math::mul(normal, 1.0F / std::sqrt(normalLengthSq));
    if (std::fabs(math::dot(normal, segmentStart.v)) > kDegenerateDistance) {
      return result;
    }

    const math::Vec3 corners[3] = {segmentStart.v, segmentEnd.v, third.v};
    for (std::size_t i = 0U; i < 3U; ++i) {
      const math::Vec3 &from = corners[i];
      const math::Vec3 edge = math::sub(corners[(i + 1U) % 3U], from);
      const float edgeLength = math::length(edge);
      const float side =
          math::dot(math::cross(edge, math::mul(from, -1.0F)), normal);
      if (side < -(kDegenerateDistance * edgeLength)) {
        return result;
      }
    }
    seed_from_triangle(seed, segmentStart, segmentEnd, third, normal, shapeA,
                       centerA, supA, shapeB, centerB, supB);
  } else {
    const math::Vec3 toOrigin = math::mul(segmentStart.v, -1.0F);
    const float along = math::dot(toOrigin, axis);
    if ((along < -kDegenerateDistance) ||
        (along > segmentLength + kDegenerateDistance)) {
      return result;
    }
    const math::Vec3 perpendicular =
        math::sub(toOrigin, math::mul(axis, along));
    if (math::length_sq(perpendicular) > toleranceSq) {
      return result;
    }
    seed_from_segment(seed, segmentStart, segmentEnd, axis, shapeA, centerA,
                      supA, shapeB, centerB, supB);
  }

  result.intersecting = true;
  if (!seed_encloses_origin(seed)) {
    return result;
  }
  return epa(seed, shapeA, centerA, supA, shapeB, centerB, supB);
}

} // namespace

/// GJK intersection walk that hands enclosing simplexes to EPA; a simplex
/// that degenerates or stalls goes to resolve_degenerate_simplex, which
/// either proves the origin is enclosed and grows a polytope for EPA or
/// reports non-intersection. A simplex that do_simplex already accepted
/// stays intersecting even when it is too flat to measure a depth from.
GjkResult gjk_epa(const void *shapeA, const math::Vec3 &centerA,
                  SupportFn supportA, const void *shapeB,
                  const math::Vec3 &centerB, SupportFn supportB) noexcept {
  GjkResult result;

  math::Vec3 dir = math::sub(centerB, centerA);
  if (math::length_sq(dir) < 1e-12F) {
    dir = math::Vec3(1.0F, 0.0F, 0.0F);
  }

  Simplex simplex;
  MinkowskiPoint sp =
      support(shapeA, centerA, supportA, shapeB, centerB, supportB, dir);
  simplex.push(sp);
  dir = math::mul(sp.v, -1.0F);

  for (std::size_t iter = 0U; iter < kGjkMaxIter; ++iter) {
    sp = support(shapeA, centerA, supportA, shapeB, centerB, supportB, dir);
    if (math::dot(sp.v, dir) < 0.0F) {
      result.intersecting = false;
      return result;
    }
    simplex.push(sp);

    if (do_simplex(simplex, dir)) {
      const EpaSeed seed = tetrahedron_seed(simplex);
      if (seed_encloses_origin(seed)) {
        return epa(seed, shapeA, centerA, supportA, shapeB, centerB, supportB);
      }
      result = resolve_degenerate_simplex(simplex, shapeA, centerA, supportA,
                                          shapeB, centerB, supportB);
      result.intersecting = true;
      return result;
    }

    if (math::length_sq(dir) < 1e-20F) {
      return resolve_degenerate_simplex(simplex, shapeA, centerA, supportA,
                                        shapeB, centerB, supportB);
    }
  }

  return resolve_degenerate_simplex(simplex, shapeA, centerA, supportA, shapeB,
                                    centerB, supportB);
}

// ------------- Support functions -------------------------------------------

math::Vec3 support_convex_hull(const void *data, const math::Vec3 &center,
                               const math::Vec3 &dir) noexcept {
  const auto *hull = static_cast<const ConvexHullData *>(data);
  if ((hull == nullptr) || (hull->vertexCount == 0U)) {
    return center;
  }

  float bestDot = -1e30F;
  std::size_t bestIdx = 0U;
  for (std::size_t i = 0U; i < hull->vertexCount; ++i) {
    const math::Vec3 worldVert = math::add(center, hull->vertices[i]);
    const float d = math::dot(worldVert, dir);
    if (d > bestDot) {
      bestDot = d;
      bestIdx = i;
    }
  }
  return math::add(center, hull->vertices[bestIdx]);
}

math::Vec3 support_sphere(const void *data, const math::Vec3 &center,
                          const math::Vec3 &dir) noexcept {
  const float radius = *static_cast<const float *>(data);
  const float len = math::length(dir);
  if (len < 1e-12F) {
    return center;
  }
  return math::add(center, math::mul(dir, radius / len));
}

math::Vec3 support_capsule(const void *data, const math::Vec3 &center,
                           const math::Vec3 &dir) noexcept {
  const auto *params = static_cast<const float *>(data);
  const float radius = params[0];
  const float halfHeight = params[1];

  const math::Vec3 top = math::add(center, math::Vec3(0.0F, halfHeight, 0.0F));
  const math::Vec3 bot = math::add(center, math::Vec3(0.0F, -halfHeight, 0.0F));
  const math::Vec3 base =
      (math::dot(top, dir) >= math::dot(bot, dir)) ? top : bot;
  const float len = math::length(dir);
  if (len < 1e-12F) {
    return base;
  }
  return math::add(base, math::mul(dir, radius / len));
}

math::Vec3 support_aabb(const void *data, const math::Vec3 &center,
                        const math::Vec3 &dir) noexcept {
  const auto *he = static_cast<const math::Vec3 *>(data);
  return math::Vec3(center.x + ((dir.x >= 0.0F) ? he->x : -he->x),
                    center.y + ((dir.y >= 0.0F) ? he->y : -he->y),
                    center.z + ((dir.z >= 0.0F) ? he->z : -he->z));
}

} // namespace engine::physics
