// Implements the narrow phase of a convex collider against a TriMesh: the
// triangles near the collider each give contacts (closed form for spheres
// and capsules, vertex-against-face plus a GJK/EPA fallback for boxes,
// hulls and sheared shapes), and those that share the deepest contact's
// face direction are reduced to one manifold of up to four points, so a
// box rests flat across two triangles and a capsule lies along a floor.
// A contact whose normal leaves that face, against a wall, resolves after
// it as a single point.

#include "narrow_phase.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "contact_clip.h"
#include "engine/math/aabb.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"
#include "engine/physics/tri_mesh.h"
#include "physics_internal.h"
#include "tri_mesh_world.h"

namespace engine::physics {

namespace {

/// Contacts one pair gathers before reduction.
constexpr std::size_t kMaxRawContacts = 64U;
/// Two contacts belong to one manifold when their normals are this close.
constexpr float kSameFaceCosine = 0.95F;
/// A contact's point within this distance of a kept one adds nothing.
constexpr float kDuplicatePointDistanceSq = 1.0e-6F;

/// One contact, its normal pointing from the mesh toward the collider.
struct MeshContact final {
  math::Vec3 point{};
  math::Vec3 normal{};
  float depth = 0.0F;
};

struct ContactList final {
  MeshContact items[kMaxRawContacts]{};
  std::size_t count = 0U;
};

void add_contact(ContactList &list, const math::Vec3 &point,
                 const math::Vec3 &normal, float depth) noexcept {
  if (!(depth > 0.0F) || !std::isfinite(depth)) {
    return;
  }
  for (std::size_t i = 0U; i < list.count; ++i) {
    // The same point on the same face, reached from a neighbouring
    // triangle, keeps the deeper reading.
    if ((math::length_sq(math::sub(list.items[i].point, point)) <=
         kDuplicatePointDistanceSq) &&
        (math::dot(list.items[i].normal, normal) >= kSameFaceCosine)) {
      list.items[i].depth = std::max(list.items[i].depth, depth);
      return;
    }
  }
  if (list.count < kMaxRawContacts) {
    list.items[list.count++] = MeshContact{point, normal, depth};
  }
}

/// Whether the collider's transform is a rotation and translation only,
/// so its sphere and capsule keep their round cross-section in the world.
bool rigid(const math::Mat4 &m) noexcept {
  constexpr float epsilon = 1.0e-4F;
  const math::Vec3 x(m.columns[0].x, m.columns[0].y, m.columns[0].z);
  const math::Vec3 y(m.columns[1].x, m.columns[1].y, m.columns[1].z);
  const math::Vec3 z(m.columns[2].x, m.columns[2].y, m.columns[2].z);
  return (std::fabs(math::length_sq(x) - 1.0F) < epsilon) &&
         (std::fabs(math::length_sq(y) - 1.0F) < epsilon) &&
         (std::fabs(math::length_sq(z) - 1.0F) < epsilon) &&
         (std::fabs(math::dot(x, y)) < epsilon) &&
         (std::fabs(math::dot(y, z)) < epsilon) &&
         (std::fabs(math::dot(x, z)) < epsilon);
}

math::Vec3 support_collider(const void *data, const math::Vec3 & /*center*/,
                            const math::Vec3 &direction) noexcept {
  return collider_support_point(
      *static_cast<const ColliderWorldGeometry *>(data), direction);
}

/// A sphere of `radius` at `center` against the triangle.
void sphere_contact(const WorldTriangle &triangle, const math::Vec3 &center,
                    float radius, ContactList &contacts) noexcept {
  const math::Vec3 closest = closest_point_on_triangle(
      center, triangle.v[0], triangle.v[1], triangle.v[2]);
  const math::Vec3 offset = math::sub(center, closest);
  const float distanceSq = math::length_sq(offset);
  if (distanceSq >= (radius * radius)) {
    return;
  }
  const float distance = std::sqrt(distanceSq);
  // A center on the triangle itself has no direction of its own: the face
  // normal, turned toward the collider, pushes it back out the side it
  // came from.
  const math::Vec3 normal = (distance > 1.0e-6F)
                                ? math::mul(offset, 1.0F / distance)
                                : triangle.normal;
  add_contact(contacts, closest, normal, radius - distance);
}

/// A capsule with core segment (p0, p1) against the triangle: each end and
/// the segment's nearest approach, so a capsule lying across the triangle
/// gets a contact at both ends.
void capsule_contact(const WorldTriangle &triangle, const math::Vec3 &p0,
                     const math::Vec3 &p1, float radius,
                     ContactList &contacts) noexcept {
  sphere_contact(triangle, p0, radius, contacts);
  sphere_contact(triangle, p1, radius, contacts);
  // The segment's nearest approach: through the face, or to an edge.
  math::Vec3 nearestOnSegment = p0;
  float bestSq = INFINITY;
  const math::Vec3 segment = math::sub(p1, p0);
  float t = 0.0F;
  if (ray_triangle_intersect(p0, segment, triangle.v[0], triangle.v[1],
                             triangle.v[2], 1.0F, &t)) {
    nearestOnSegment = math::add(p0, math::mul(segment, t));
    bestSq = 0.0F;
  }
  for (std::size_t e = 0U; (e < 3U) && (bestSq > 0.0F); ++e) {
    math::Vec3 onSegment{};
    math::Vec3 onEdge{};
    const float distanceSq = closest_point_segment_segment(
        p0, p1, triangle.v[e], triangle.v[(e + 1U) % 3U], onSegment, onEdge);
    if (distanceSq < bestSq) {
      bestSq = distanceSq;
      nearestOnSegment = onSegment;
    }
  }
  if (bestSq < (radius * radius)) {
    sphere_contact(triangle, nearestOnSegment, radius, contacts);
  }
}

/// A box, hull or sheared collider against the triangle: each of its
/// corners that has passed through the face, then GJK/EPA when no corner
/// has (an edge across an edge).
void convex_contact(const WorldTriangle &triangle,
                    const ColliderWorldGeometry &geometry,
                    ContactList &contacts) noexcept {
  const std::size_t before = contacts.count;
  const math::Vec3 &n = triangle.normal;
  const math::Vec3 extent = math::aabb_half_extents(geometry.worldAabb);
  const float maxDepth = 2.0F * std::max({extent.x, extent.y, extent.z});
  const auto try_corner = [&](const math::Vec3 &corner) noexcept {
    const float height = math::dot(math::sub(corner, triangle.v[0]), n);
    if (!(height < 0.0F) || !(height > -maxDepth)) {
      return;
    }
    const math::Vec3 onPlane = math::sub(corner, math::mul(n, height));
    const math::Vec3 nearest = closest_point_on_triangle(
        onPlane, triangle.v[0], triangle.v[1], triangle.v[2]);
    if (math::length_sq(math::sub(nearest, onPlane)) > 1.0e-8F) {
      return;
    }
    add_contact(contacts, corner, n, -height);
  };
  if (geometry.shape == math::ColliderShape::AABB) {
    for (int c = 0; c < 8; ++c) {
      const math::Vec3 local(
          (c & 1) ? geometry.halfExtents.x : -geometry.halfExtents.x,
          (c & 2) ? geometry.halfExtents.y : -geometry.halfExtents.y,
          (c & 4) ? geometry.halfExtents.z : -geometry.halfExtents.z);
      try_corner(math::transform_point(geometry.localToWorld, local));
    }
  } else if ((geometry.shape == math::ColliderShape::ConvexHull) &&
             (geometry.convexHull != nullptr)) {
    for (std::size_t v = 0U; v < geometry.convexHull->vertexCount; ++v) {
      try_corner(math::transform_point(geometry.localToWorld,
                                       geometry.convexHull->vertices[v]));
    }
  }
  if (contacts.count > before) {
    return;
  }
  const GjkResult gjk =
      gjk_epa(&triangle, triangle.center, &support_world_triangle, &geometry,
              geometry.center, &support_collider);
  if (!gjk.intersecting || !(gjk.depth > 1.0e-6F)) {
    return;
  }
  const float normalLengthSq = math::length_sq(gjk.normal);
  if (!std::isfinite(normalLengthSq) || (normalLengthSq < 0.81F) ||
      (normalLengthSq > 1.21F)) {
    return;
  }
  // EPA's normal runs from the triangle toward the collider; a depth past
  // the collider's own size is a degenerate polytope, not a contact.
  const math::Vec3 normal = math::normalize(gjk.normal);
  const float depth = std::min(gjk.depth, maxDepth);
  const math::Vec3 point =
      math::add(collider_support_point(geometry, math::mul(normal, -1.0F)),
                math::mul(normal, depth * 0.5F));
  add_contact(contacts, point, normal, depth);
}

/// Chooses up to four of the contacts in `members` that span the most
/// area: the deepest, the farthest from it, the farthest from the line
/// through both, and the farthest from those three's centroid.
void reduce(const ContactList &contacts, const std::size_t *members,
            std::size_t memberCount, std::size_t deepest,
            ClippedManifold *outManifold) noexcept {
  std::size_t chosen[ClippedManifold::kMaxPoints] = {deepest, 0U, 0U, 0U};
  std::size_t chosenCount = 1U;
  const auto taken = [&](std::size_t index) noexcept {
    for (std::size_t i = 0U; i < chosenCount; ++i) {
      if (chosen[i] == index) {
        return true;
      }
    }
    return false;
  };
  const auto pick = [&](auto score) noexcept {
    float bestScore = 1.0e-8F;
    std::size_t best = kMaxRawContacts;
    for (std::size_t m = 0U; m < memberCount; ++m) {
      const std::size_t index = members[m];
      if (taken(index)) {
        continue;
      }
      const float s = score(contacts.items[index].point);
      if (s > bestScore) {
        bestScore = s;
        best = index;
      }
    }
    if (best < kMaxRawContacts) {
      chosen[chosenCount++] = best;
    }
  };
  const math::Vec3 first = contacts.items[deepest].point;
  pick([&](const math::Vec3 &p) noexcept {
    return math::length_sq(math::sub(p, first));
  });
  if (chosenCount == 2U) {
    const math::Vec3 second = contacts.items[chosen[1]].point;
    const math::Vec3 axis = math::sub(second, first);
    pick([&](const math::Vec3 &p) noexcept {
      return math::length_sq(math::cross(math::sub(p, first), axis));
    });
  }
  if (chosenCount == 3U) {
    const math::Vec3 centroid =
        math::mul(math::add(math::add(first, contacts.items[chosen[1]].point),
                            contacts.items[chosen[2]].point),
                  1.0F / 3.0F);
    pick([&](const math::Vec3 &p) noexcept {
      return math::length_sq(math::sub(p, centroid));
    });
  }
  outManifold->count = chosenCount;
  for (std::size_t i = 0U; i < chosenCount; ++i) {
    outManifold->points[i] = contacts.items[chosen[i]].point;
    outManifold->penetrations[i] = contacts.items[chosen[i]].depth;
  }
}

} // namespace

void narrow_phase_tri_mesh(const PairContext &pair) noexcept {
  const bool meshIsA = pair.colliderA.shape == ColliderShape::TriMesh;
  const ColliderWorldGeometry &meshGeometry =
      meshIsA ? pair.geometryA : pair.geometryB;
  const ColliderWorldGeometry &object =
      meshIsA ? pair.geometryB : pair.geometryA;
  const TriMeshData *mesh = meshGeometry.triMesh;
  // A mesh is static geometry: one on a body that moves has no volume to
  // give it mass, so it collides with nothing (World::add_collider warns).
  const float meshInverseMass = meshIsA ? pair.invMassA : pair.invMassB;
  if ((mesh == nullptr) || (meshInverseMass > 0.0F) ||
      (object.shape == ColliderShape::TriMesh) ||
      (object.shape == ColliderShape::Heightfield)) {
    if ((mesh != nullptr) && (meshInverseMass > 0.0F)) {
      ++pair.physicsCtx.triMeshMovingPairsSkipped;
    }
    return;
  }

  std::uint32_t candidates[kMaxTriMeshCandidates] = {};
  const std::size_t found = collect_tri_mesh_triangles(
      *mesh, world_box_into_mesh(meshGeometry, object.worldAabb), candidates,
      kMaxTriMeshCandidates);
  if (found > kMaxTriMeshCandidates) {
    ++pair.physicsCtx.triMeshCandidateOverflows;
  }
  const std::size_t candidateCount = std::min(found, kMaxTriMeshCandidates);
  if (candidateCount == 0U) {
    return;
  }

  const bool roundShape =
      rigid(object.localToWorld) && ((object.shape == ColliderShape::Sphere) ||
                                     (object.shape == ColliderShape::Capsule));
  math::Vec3 capsule0{};
  math::Vec3 capsule1{};
  if (roundShape && (object.shape == ColliderShape::Capsule)) {
    capsule0 = math::transform_point(
        object.localToWorld, math::Vec3(0.0F, -object.halfExtents.y, 0.0F));
    capsule1 = math::transform_point(
        object.localToWorld, math::Vec3(0.0F, object.halfExtents.y, 0.0F));
  }
  ContactList contacts{};
  for (std::size_t c = 0U; c < candidateCount; ++c) {
    WorldTriangle triangle{};
    if (!world_triangle(meshGeometry, candidates[c], &triangle)) {
      continue;
    }
    // Facing the collider, so a contact pushes it back the way it came.
    if (math::dot(math::sub(object.center, triangle.v[0]), triangle.normal) <
        0.0F) {
      triangle.normal = math::mul(triangle.normal, -1.0F);
    }
    if (roundShape && (object.shape == ColliderShape::Sphere)) {
      sphere_contact(triangle, object.center, object.halfExtents.x, contacts);
    } else if (roundShape) {
      capsule_contact(triangle, capsule0, capsule1, object.halfExtents.x,
                      contacts);
    } else {
      convex_contact(triangle, object, contacts);
    }
  }
  if (contacts.count == 0U) {
    return;
  }
  if (!record_pair_and_wake(pair)) {
    return;
  }

  std::size_t deepest = 0U;
  for (std::size_t i = 1U; i < contacts.count; ++i) {
    if (contacts.items[i].depth > contacts.items[deepest].depth) {
      deepest = i;
    }
  }
  const math::Vec3 faceNormal = contacts.items[deepest].normal;
  std::size_t members[kMaxRawContacts] = {};
  std::size_t memberCount = 0U;
  std::size_t secondary = kMaxRawContacts;
  for (std::size_t i = 0U; i < contacts.count; ++i) {
    if (math::dot(contacts.items[i].normal, faceNormal) >= kSameFaceCosine) {
      members[memberCount++] = i;
    } else if ((secondary == kMaxRawContacts) ||
               (contacts.items[i].depth > contacts.items[secondary].depth)) {
      secondary = i;
    }
  }
  ClippedManifold manifold{};
  reduce(contacts, members, memberCount, deepest, &manifold);
  // resolve_pair_manifold's normal runs from A to B.
  const float toB = meshIsA ? 1.0F : -1.0F;
  resolve_pair_manifold(pair, math::mul(faceNormal, toB), manifold);

  if (secondary == kMaxRawContacts) {
    return;
  }
  // The manifold above already moved the collider out along its face; what
  // is left of the other contact is the part that move did not cover.
  const MeshContact &other = contacts.items[secondary];
  const float covered = contacts.items[deepest].depth *
                        std::max(0.0F, math::dot(faceNormal, other.normal));
  const float remaining = other.depth - covered;
  if (remaining > 0.0F) {
    resolve_pair_contact(pair, math::mul(other.normal, toB), remaining,
                         other.point);
  }
}

} // namespace engine::physics
