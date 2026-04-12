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

static constexpr std::size_t kMaxFaces = 256U; // working faces during build
static constexpr std::size_t kMaxEdges = 512U;

struct HullFace {
  std::uint16_t v[3]{};
  math::Vec3 normal{};
  float dist = 0.0F; // plane distance from origin
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

float point_plane_distance(const math::Vec3 &point,
                           const HullFace &face) noexcept {
  return math::dot(face.normal, point) - face.dist;
}

} // namespace

bool build_convex_hull(const math::Vec3 *points, std::size_t pointCount,
                       ConvexHullData &outHull) noexcept {
  if ((points == nullptr) || (pointCount < 4U)) {
    return false;
  }

  // Clamp input.
  const std::size_t maxPts = ConvexHullData::kMaxVertices;
  const std::size_t count = (pointCount > maxPts) ? maxPts : pointCount;

  // Copy points to local workspace.
  std::array<math::Vec3, ConvexHullData::kMaxVertices> verts{};
  for (std::size_t i = 0U; i < count; ++i) {
    verts[i] = points[i];
  }

  // 1. Find initial tetrahedron: two most distant points, farthest from
  //    their line, farthest from their plane.
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
    return false; // degenerate
  }

  // Farthest from line i0→i1.
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
    return false; // collinear
  }

  // Triangle normal.
  math::Vec3 triNormal = math::cross(math::sub(verts[i1], verts[i0]),
                                     math::sub(verts[i2], verts[i0]));
  triNormal = math::normalize(triNormal);

  // Farthest from that plane.
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
    return false; // coplanar
  }

  // Orient tetrahedron so that face i0-i1-i2 has outward normal (away from i3).
  if (math::dot(math::sub(verts[i3], verts[i0]), triNormal) > 0.0F) {
    // Normal points toward i3 (inward) — swap i1 and i2 to flip.
    const std::size_t tmp = i1;
    i1 = i2;
    i2 = tmp;
  }

  // Build initial 4 faces.
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

  // 4 faces of the tetrahedron. Each face normal should point outward.
  add_face(i0, i1, i2);
  add_face(i0, i2, i3);
  add_face(i0, i3, i1);
  add_face(i1, i3, i2);

  // Validate: flip any face whose normal points toward the centroid.
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

  // 2. Incrementally add remaining points.
  for (std::size_t pi = 0U; pi < count; ++pi) {
    if (pi == i0 || pi == i1 || pi == i2 || pi == i3) {
      continue;
    }

    // Find faces visible from this point.
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
      continue; // inside the hull
    }

    // Find horizon edges (boundary of visible region).
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
        // Check if the neighboring face across this edge is NOT visible.
        bool neighborVisible = false;
        for (std::size_t fj = 0U; fj < faceCount; ++fj) {
          if (fj == fi || !faces[fj].alive) {
            continue;
          }
          // Check if fj shares edge (eb, ea) — reversed winding.
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

    // Kill visible faces.
    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (visible[fi]) {
        faces[fi].alive = false;
      }
    }

    // Create new faces from horizon edges to the new point.
    for (std::size_t ei = 0U; ei < horizonCount; ++ei) {
      add_face(horizon[ei].a, horizon[ei].b, static_cast<std::uint16_t>(pi));
    }
  }

  // 3. Collect surviving faces/vertices into output.
  outHull.planeCount = 0U;
  outHull.vertexCount = 0U;

  // Remap vertex indices: only include vertices used by surviving faces.
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

  // Compute local AABB.
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
static constexpr float kEpaTolerance = 1e-4F;

struct MinkowskiPoint {
  math::Vec3 v{}; // Minkowski difference point
  math::Vec3 a{}; // support point on A
  math::Vec3 b{}; // support point on B
};

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

// GJK simplex operations.
struct Simplex {
  std::array<MinkowskiPoint, 4> pts{};
  int size = 0;

  void push(const MinkowskiPoint &p) noexcept {
    // Push to front.
    for (int i = size; i > 0; --i) {
      pts[i] = pts[i - 1];
    }
    pts[0] = p;
    if (size < 4) {
      ++size;
    }
  }
};

bool do_simplex_line(Simplex &s, math::Vec3 &dir) noexcept {
  // A = s.pts[0], B = s.pts[1]
  const math::Vec3 ab = math::sub(s.pts[1].v, s.pts[0].v);
  const math::Vec3 ao = math::mul(s.pts[0].v, -1.0F);
  if (math::dot(ab, ao) > 0.0F) {
    // Origin is between A and B or beyond B.
    dir = math::cross(math::cross(ab, ao), ab);
  } else {
    s.size = 1;
    dir = ao;
  }
  return false;
}

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
        // Flip winding.
        const MinkowskiPoint tmp = s.pts[1];
        s.pts[1] = s.pts[2];
        s.pts[2] = tmp;
        dir = math::mul(abc, -1.0F);
      }
    }
  }
  return false;
}

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
  // Origin is inside the tetrahedron.
  return true;
}

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

void epa_face_plane(EpaFace &f,
                    const std::array<MinkowskiPoint, 256> &verts) noexcept {
  const math::Vec3 ab = math::sub(verts[f.v[1]].v, verts[f.v[0]].v);
  const math::Vec3 ac = math::sub(verts[f.v[2]].v, verts[f.v[0]].v);
  f.normal = math::cross(ab, ac);
  const float len = math::length(f.normal);
  if (len > 1e-10F) {
    f.normal = math::mul(f.normal, 1.0F / len);
  }
  f.dist = math::dot(f.normal, verts[f.v[0]].v);
  // Ensure normal faces outward (away from origin).
  if (f.dist < 0.0F) {
    f.dist = -f.dist;
    f.normal = math::mul(f.normal, -1.0F);
    const auto tmp = f.v[0];
    f.v[0] = f.v[1];
    f.v[1] = tmp;
  }
}

GjkResult epa(Simplex &simplex, const void *shapeA, const math::Vec3 &centerA,
              SupportFn supA, const void *shapeB, const math::Vec3 &centerB,
              SupportFn supB) noexcept {
  GjkResult result;
  result.intersecting = true;

  std::array<MinkowskiPoint, 256> verts{};
  std::size_t vertCount = 4U;
  for (int i = 0; i < 4; ++i) {
    verts[i] = simplex.pts[i];
  }

  std::array<EpaFace, kEpaMaxFaces> faces{};
  std::size_t faceCount = 0U;

  auto add_epa_face = [&](std::uint16_t a, std::uint16_t b, std::uint16_t c) {
    if (faceCount >= kEpaMaxFaces) {
      return;
    }
    EpaFace &f = faces[faceCount];
    f.v[0] = a;
    f.v[1] = b;
    f.v[2] = c;
    f.alive = true;
    epa_face_plane(f, verts);
    ++faceCount;
  };

  // Build initial tetrahedron faces.
  add_epa_face(0, 1, 2);
  add_epa_face(0, 2, 3);
  add_epa_face(0, 3, 1);
  add_epa_face(1, 3, 2);

  for (std::size_t iter = 0U; iter < kEpaMaxIter; ++iter) {
    // Find closest face to origin.
    std::size_t closestIdx = 0U;
    float closestDist = 1e30F;
    for (std::size_t fi = 0U; fi < faceCount; ++fi) {
      if (!faces[fi].alive) {
        continue;
      }
      if (faces[fi].dist < closestDist) {
        closestDist = faces[fi].dist;
        closestIdx = fi;
      }
    }

    const EpaFace &closest = faces[closestIdx];
    const math::Vec3 searchDir = closest.normal;

    // Get new support point.
    const MinkowskiPoint newPt =
        support(shapeA, centerA, supA, shapeB, centerB, supB, searchDir);
    const float newDist = math::dot(newPt.v, searchDir);

    if ((newDist - closestDist) < kEpaTolerance) {
      // Converged.
      result.normal = closest.normal;
      result.depth = closestDist;
      // Approximate contact point: interpolate A-side support points on
      // the closest face using barycentric coordinates of the origin
      // projection.
      result.contactPoint = math::mul(
          math::add(verts[closest.v[0]].a,
                    math::add(verts[closest.v[1]].a, verts[closest.v[2]].a)),
          1.0F / 3.0F);
      return result;
    }

    if (vertCount >= 256U) {
      // Out of vertex budget; return best so far.
      result.normal = closest.normal;
      result.depth = closestDist;
      result.contactPoint = math::mul(
          math::add(verts[closest.v[0]].a,
                    math::add(verts[closest.v[1]].a, verts[closest.v[2]].a)),
          1.0F / 3.0F);
      return result;
    }

    const auto newIdx = static_cast<std::uint16_t>(vertCount);
    verts[vertCount++] = newPt;

    // Remove faces visible from the new point and collect horizon edges.
    struct Edge {
      std::uint16_t a, b;
    };
    std::array<Edge, 256> horizon{};
    std::size_t horizonCount = 0U;

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
        if (!neighborVisible && horizonCount < 256U) {
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
      add_epa_face(horizon[ei].a, horizon[ei].b, newIdx);
    }
  }

  // Max iterations — return best estimate.
  float closestDist = 1e30F;
  std::size_t closestIdx = 0U;
  for (std::size_t fi = 0U; fi < faceCount; ++fi) {
    if (!faces[fi].alive) {
      continue;
    }
    if (faces[fi].dist < closestDist) {
      closestDist = faces[fi].dist;
      closestIdx = fi;
    }
  }
  result.normal = faces[closestIdx].normal;
  result.depth = closestDist;
  result.contactPoint =
      math::mul(math::add(verts[faces[closestIdx].v[0]].a,
                          math::add(verts[faces[closestIdx].v[1]].a,
                                    verts[faces[closestIdx].v[2]].a)),
                1.0F / 3.0F);
  return result;
}

} // namespace

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
      // Origin is inside the Minkowski difference — shapes intersect.
      // Run EPA for penetration info.
      return epa(simplex, shapeA, centerA, supportA, shapeB, centerB, supportB);
    }

    // Safety: direction must not degenerate.
    if (math::length_sq(dir) < 1e-20F) {
      result.intersecting = false;
      return result;
    }
  }

  result.intersecting = false;
  return result;
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
  // data points to float[2]: {radius, halfHeight}
  const auto *params = static_cast<const float *>(data);
  const float radius = params[0];
  const float halfHeight = params[1];

  // Capsule is Y-axis aligned.  Find the endpoint that is farthest in
  // the given direction.
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
