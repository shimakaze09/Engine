// Implements reference-face contact clipping for faceted colliders: the two
// touching faces are found from the contact normal, the incident face is
// clipped against the reference face's edge planes (Sutherland–Hodgman), and
// the surviving points reduce to a max-spread four-point manifold.

#include "contact_clip.h"

#include "engine/math/mat4.h"
#include "engine/math/vec4.h"

#include <cmath>
#include <cstddef>

namespace engine::physics {
namespace {

constexpr std::size_t kMaxFaceVertices = 16U;
constexpr std::size_t kMaxClipVertices = 32U;
constexpr float kFaceGatherEpsilon = 1.0e-3F;
constexpr float kKeepSeparationTolerance = 1.0e-3F;

/// World-space polygon of one collider face with its outward unit normal.
struct FacePolygon final {
  math::Vec3 vertices[kMaxFaceVertices]{};
  std::size_t count = 0U;
  math::Vec3 outwardNormal{};
};

/// Transforms a point by an affine matrix.
[[nodiscard]] math::Vec3 transform_point(const math::Mat4 &matrix,
                                         const math::Vec3 &point) noexcept {
  const math::Vec4 result =
      math::mul(matrix, math::Vec4(point.x, point.y, point.z, 1.0F));
  return math::Vec3(result.x, result.y, result.z);
}

/// Multiplies a direction by the transpose of the matrix's linear 3×3 block.
[[nodiscard]] math::Vec3 mul_transpose3(const math::Mat4 &matrix,
                                        const math::Vec3 &v) noexcept {
  return math::Vec3(v.x * matrix.columns[0].x + v.y * matrix.columns[0].y +
                        v.z * matrix.columns[0].z,
                    v.x * matrix.columns[1].x + v.y * matrix.columns[1].y +
                        v.z * matrix.columns[1].z,
                    v.x * matrix.columns[2].x + v.y * matrix.columns[2].y +
                        v.z * matrix.columns[2].z);
}

/// Returns the vector scaled to unit length, or false when degenerate.
[[nodiscard]] bool safe_normalize(math::Vec3 *v) noexcept {
  const float lengthSq = math::dot(*v, *v);
  if (!(lengthSq > 1.0e-12F) || !std::isfinite(lengthSq)) {
    return false;
  }
  *v = math::mul(*v, 1.0F / std::sqrt(lengthSq));
  return true;
}

/// Maps a local-space face normal to a world-space unit normal via the
/// inverse-transpose (worldToLocal transposed) of the collider transform.
[[nodiscard]] bool face_normal_world(const ColliderWorldGeometry &geometry,
                                     const math::Vec3 &localNormal,
                                     math::Vec3 *outNormal) noexcept {
  *outNormal = mul_transpose3(geometry.worldToLocal, localNormal);
  return safe_normalize(outNormal);
}

/// Orders the polygon's vertices by angle around its outward normal so edge
/// planes can be built from consecutive vertices.
void order_polygon(FacePolygon *face) noexcept {
  if (face->count < 3U) {
    return;
  }
  math::Vec3 centroid(0.0F, 0.0F, 0.0F);
  for (std::size_t i = 0U; i < face->count; ++i) {
    centroid = math::add(centroid, face->vertices[i]);
  }
  centroid = math::mul(centroid, 1.0F / static_cast<float>(face->count));

  math::Vec3 tangent = (std::fabs(face->outwardNormal.x) < 0.9F)
                           ? math::cross(face->outwardNormal,
                                         math::Vec3(1.0F, 0.0F, 0.0F))
                           : math::cross(face->outwardNormal,
                                         math::Vec3(0.0F, 1.0F, 0.0F));
  if (!safe_normalize(&tangent)) {
    return;
  }
  const math::Vec3 bitangent = math::cross(face->outwardNormal, tangent);

  float angles[kMaxFaceVertices]{};
  for (std::size_t i = 0U; i < face->count; ++i) {
    const math::Vec3 offset = math::sub(face->vertices[i], centroid);
    angles[i] =
        std::atan2(math::dot(offset, bitangent), math::dot(offset, tangent));
  }
  for (std::size_t i = 1U; i < face->count; ++i) {
    const float angle = angles[i];
    const math::Vec3 vertex = face->vertices[i];
    std::size_t j = i;
    while ((j > 0U) && (angles[j - 1U] > angle)) {
      angles[j] = angles[j - 1U];
      face->vertices[j] = face->vertices[j - 1U];
      --j;
    }
    angles[j] = angle;
    face->vertices[j] = vertex;
  }
}

/// Extracts the box face whose outward local axis best matches the local
/// query direction.
bool extract_box_face(const ColliderWorldGeometry &geometry,
                      const math::Vec3 &localDirection,
                      FacePolygon *outFace) noexcept {
  const math::Vec3 he = geometry.halfExtents;
  const float ax = std::fabs(localDirection.x);
  const float ay = std::fabs(localDirection.y);
  const float az = std::fabs(localDirection.z);

  std::size_t axis = 0U;
  if ((ay >= ax) && (ay >= az)) {
    axis = 1U;
  } else if ((az >= ax) && (az >= ay)) {
    axis = 2U;
  }
  const float axisValue = (axis == 0U)   ? localDirection.x
                          : (axis == 1U) ? localDirection.y
                                         : localDirection.z;
  const float sign = (axisValue >= 0.0F) ? 1.0F : -1.0F;

  math::Vec3 localNormal(0.0F, 0.0F, 0.0F);
  math::Vec3 corners[4]{};
  if (axis == 0U) {
    localNormal.x = sign;
    corners[0] = math::Vec3(sign * he.x, -he.y, -he.z);
    corners[1] = math::Vec3(sign * he.x, he.y, -he.z);
    corners[2] = math::Vec3(sign * he.x, he.y, he.z);
    corners[3] = math::Vec3(sign * he.x, -he.y, he.z);
  } else if (axis == 1U) {
    localNormal.y = sign;
    corners[0] = math::Vec3(-he.x, sign * he.y, -he.z);
    corners[1] = math::Vec3(he.x, sign * he.y, -he.z);
    corners[2] = math::Vec3(he.x, sign * he.y, he.z);
    corners[3] = math::Vec3(-he.x, sign * he.y, he.z);
  } else {
    localNormal.z = sign;
    corners[0] = math::Vec3(-he.x, -he.y, sign * he.z);
    corners[1] = math::Vec3(he.x, -he.y, sign * he.z);
    corners[2] = math::Vec3(he.x, he.y, sign * he.z);
    corners[3] = math::Vec3(-he.x, he.y, sign * he.z);
  }

  for (std::size_t i = 0U; i < 4U; ++i) {
    outFace->vertices[i] = transform_point(geometry.localToWorld, corners[i]);
  }
  outFace->count = 4U;
  return face_normal_world(geometry, localNormal, &outFace->outwardNormal);
}

/// Extracts the hull face whose plane normal best matches the local query
/// direction, gathering the vertices lying on that plane.
bool extract_hull_face(const ColliderWorldGeometry &geometry,
                       const math::Vec3 &localDirection,
                       FacePolygon *outFace) noexcept {
  const ConvexHullData *const hull = geometry.convexHull;
  if ((hull == nullptr) || (hull->planeCount == 0U) ||
      (hull->vertexCount == 0U)) {
    return false;
  }

  std::size_t bestPlane = 0U;
  float bestDot = math::dot(hull->planes[0].normal, localDirection);
  for (std::size_t i = 1U; i < hull->planeCount; ++i) {
    const float d = math::dot(hull->planes[i].normal, localDirection);
    if (d > bestDot) {
      bestDot = d;
      bestPlane = i;
    }
  }

  const ConvexHullData::Plane &plane = hull->planes[bestPlane];
  const float epsilon =
      kFaceGatherEpsilon * (1.0F + std::fabs(plane.distance));
  outFace->count = 0U;
  for (std::size_t i = 0U; i < hull->vertexCount; ++i) {
    if (outFace->count >= kMaxFaceVertices) {
      break;
    }
    const float distance =
        math::dot(plane.normal, hull->vertices[i]) - plane.distance;
    if (std::fabs(distance) <= epsilon) {
      outFace->vertices[outFace->count] =
          transform_point(geometry.localToWorld, hull->vertices[i]);
      ++outFace->count;
    }
  }
  if (outFace->count == 0U) {
    return false;
  }
  return face_normal_world(geometry, plane.normal, &outFace->outwardNormal);
}

/// Extracts the collider face facing the world query direction; false for
/// non-faceted shapes.
bool extract_face(const ColliderWorldGeometry &geometry,
                  const math::Vec3 &worldDirection,
                  FacePolygon *outFace) noexcept {
  const math::Vec3 localDirection =
      mul_transpose3(geometry.localToWorld, worldDirection);
  bool ok = false;
  switch (geometry.shape) {
  case math::ColliderShape::AABB:
    ok = extract_box_face(geometry, localDirection, outFace);
    break;
  case math::ColliderShape::ConvexHull:
    ok = extract_hull_face(geometry, localDirection, outFace);
    break;
  case math::ColliderShape::Sphere:
  case math::ColliderShape::Capsule:
  case math::ColliderShape::Heightfield:
    return false;
  }
  if (ok) {
    order_polygon(outFace);
  }
  return ok;
}

/// Clips a polygon against one plane (keeps the non-negative side).
std::size_t clip_polygon_by_plane(const math::Vec3 *inVerts,
                                  std::size_t inCount,
                                  const math::Vec3 &planeNormal,
                                  const math::Vec3 &planePoint,
                                  math::Vec3 *outVerts) noexcept {
  std::size_t outCount = 0U;
  for (std::size_t i = 0U; i < inCount; ++i) {
    const math::Vec3 &current = inVerts[i];
    const math::Vec3 &next = inVerts[(i + 1U) % inCount];
    const float currentSide =
        math::dot(math::sub(current, planePoint), planeNormal);
    const float nextSide = math::dot(math::sub(next, planePoint), planeNormal);
    if (currentSide >= 0.0F) {
      if (outCount < kMaxClipVertices) {
        outVerts[outCount] = current;
        ++outCount;
      }
    }
    if (((currentSide >= 0.0F) != (nextSide >= 0.0F)) &&
        (outCount < kMaxClipVertices)) {
      const float t = currentSide / (currentSide - nextSide);
      outVerts[outCount] =
          math::add(current, math::mul(math::sub(next, current), t));
      ++outCount;
    }
  }
  return outCount;
}

/// Reduces surviving points to the deepest plus the three maximizing spread.
void reduce_points(const math::Vec3 *points, const float *penetrations,
                   std::size_t count, ClippedManifold *out) noexcept {
  if (count <= ClippedManifold::kMaxPoints) {
    for (std::size_t i = 0U; i < count; ++i) {
      out->points[i] = points[i];
      out->penetrations[i] = penetrations[i];
    }
    out->count = count;
    return;
  }

  bool used[kMaxClipVertices] = {};
  std::size_t kept[ClippedManifold::kMaxPoints]{};

  std::size_t deepest = 0U;
  for (std::size_t i = 1U; i < count; ++i) {
    if (penetrations[i] > penetrations[deepest]) {
      deepest = i;
    }
  }
  kept[0] = deepest;
  used[deepest] = true;

  std::size_t farthest = 0U;
  float maxDistSq = -1.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 d = math::sub(points[i], points[kept[0]]);
    const float distSq = math::dot(d, d);
    if (distSq > maxDistSq) {
      maxDistSq = distSq;
      farthest = i;
    }
  }
  kept[1] = farthest;
  used[farthest] = true;

  std::size_t third = 0U;
  float maxArea = -1.0F;
  const math::Vec3 edge01 = math::sub(points[kept[1]], points[kept[0]]);
  for (std::size_t i = 0U; i < count; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 edge0i = math::sub(points[i], points[kept[0]]);
    const math::Vec3 crossVec = math::cross(edge01, edge0i);
    const float area = math::dot(crossVec, crossVec);
    if (area > maxArea) {
      maxArea = area;
      third = i;
    }
  }
  kept[2] = third;
  used[third] = true;

  std::size_t fourth = 0U;
  float maxArea2 = -1.0F;
  for (std::size_t i = 0U; i < count; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 e0 = math::sub(points[i], points[kept[0]]);
    const math::Vec3 e1 = math::sub(points[i], points[kept[1]]);
    const math::Vec3 crossVec = math::cross(e0, e1);
    const float area = math::dot(crossVec, crossVec);
    if (area > maxArea2) {
      maxArea2 = area;
      fourth = i;
    }
  }
  kept[3] = fourth;

  for (std::size_t i = 0U; i < ClippedManifold::kMaxPoints; ++i) {
    out->points[i] = points[kept[i]];
    out->penetrations[i] = penetrations[kept[i]];
  }
  out->count = ClippedManifold::kMaxPoints;
}

} // namespace

bool clip_contact_manifold(const ColliderWorldGeometry &geometryA,
                           const ColliderWorldGeometry &geometryB,
                           const math::Vec3 &normal,
                           ClippedManifold *outManifold) noexcept {
  if (outManifold == nullptr) {
    return false;
  }
  outManifold->count = 0U;

  FacePolygon faceA{};
  FacePolygon faceB{};
  if (!extract_face(geometryA, normal, &faceA) ||
      !extract_face(geometryB, math::mul(normal, -1.0F), &faceB)) {
    return false;
  }

  const float alignA = math::dot(faceA.outwardNormal, normal);
  const float alignB =
      math::dot(faceB.outwardNormal, math::mul(normal, -1.0F));
  bool referenceIsA = alignA >= alignB;
  if (referenceIsA && (faceA.count < 3U) && (faceB.count >= 3U)) {
    referenceIsA = false;
  } else if (!referenceIsA && (faceB.count < 3U) && (faceA.count >= 3U)) {
    referenceIsA = true;
  }
  const FacePolygon &reference = referenceIsA ? faceA : faceB;
  const FacePolygon &incident = referenceIsA ? faceB : faceA;
  if ((reference.count < 3U) || (incident.count == 0U)) {
    return false;
  }

  math::Vec3 centroid(0.0F, 0.0F, 0.0F);
  for (std::size_t i = 0U; i < reference.count; ++i) {
    centroid = math::add(centroid, reference.vertices[i]);
  }
  centroid = math::mul(centroid, 1.0F / static_cast<float>(reference.count));

  math::Vec3 bufferA[kMaxClipVertices]{};
  math::Vec3 bufferB[kMaxClipVertices]{};
  for (std::size_t i = 0U; i < incident.count; ++i) {
    bufferA[i] = incident.vertices[i];
  }
  math::Vec3 *input = bufferA;
  math::Vec3 *output = bufferB;
  std::size_t inputCount = incident.count;

  for (std::size_t edge = 0U; edge < reference.count; ++edge) {
    const math::Vec3 &v0 = reference.vertices[edge];
    const math::Vec3 &v1 = reference.vertices[(edge + 1U) % reference.count];
    math::Vec3 edgeNormal =
        math::cross(reference.outwardNormal, math::sub(v1, v0));
    if (!safe_normalize(&edgeNormal)) {
      continue;
    }
    if (math::dot(math::sub(centroid, v0), edgeNormal) < 0.0F) {
      edgeNormal = math::mul(edgeNormal, -1.0F);
    }
    inputCount = clip_polygon_by_plane(input, inputCount, edgeNormal, v0,
                                       output);
    math::Vec3 *const swapped = input;
    input = output;
    output = swapped;
    if (inputCount == 0U) {
      return false;
    }
  }

  math::Vec3 keptPoints[kMaxClipVertices]{};
  float keptPenetrations[kMaxClipVertices]{};
  std::size_t keptCount = 0U;
  for (std::size_t i = 0U; i < inputCount; ++i) {
    const float separation = math::dot(
        reference.outwardNormal, math::sub(input[i], reference.vertices[0]));
    const float penetration = -separation;
    if (penetration >= -kKeepSeparationTolerance) {
      keptPoints[keptCount] = input[i];
      keptPenetrations[keptCount] = std::fmax(penetration, 0.0F);
      ++keptCount;
    }
  }
  if (keptCount == 0U) {
    return false;
  }

  reduce_points(keptPoints, keptPenetrations, keptCount, outManifold);
  return true;
}

} // namespace engine::physics
