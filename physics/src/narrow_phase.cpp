// Implements the physics narrow phase: per-shape-pair contact testers
// (analytic capsule/sphere/box paths, GJK/EPA for affine convex shapes,
// heightfield resolution) plus the pair recording and wake bookkeeping
// they share.

#include "narrow_phase.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/math/aabb.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"
#include "engine/physics/physics.h"
#include "contact_clip.h"
#include "contact_resolution.h"
#include "physics_internal.h"

namespace engine::physics {
namespace {

// Overlap of two 1D intervals; non-positive when separated.
float axis_overlap(float aMin, float aMax, float bMin, float bMax) noexcept {
  return std::min(aMax, bMax) - std::max(aMin, bMin);
}

std::uint64_t make_pair_key(std::uint32_t idxA, std::uint32_t idxB) noexcept {
  const std::uint32_t lo = std::min(idxA, idxB);
  const std::uint32_t hi = std::max(idxA, idxB);
  return (static_cast<std::uint64_t>(lo) << 32U) |
         static_cast<std::uint64_t>(hi);
}

bool insert_pair_key(PhysicsContext &ctx, std::uint64_t key) noexcept {
  const std::uint32_t generation = ctx.pairHashGeneration;
  const std::size_t bucketCount = ctx.pairHashStamps.size();
  std::size_t bucket =
      static_cast<std::size_t>((key * 11400714819323198485ULL) % bucketCount);

  for (std::size_t probe = 0U; probe < bucketCount; ++probe) {
    if (ctx.pairHashStamps[bucket] != generation) {
      ctx.pairHashStamps[bucket] = generation;
      ctx.pairHashKeys[bucket] = key;
      return true;
    }

    if (ctx.pairHashKeys[bucket] == key) {
      return false;
    }

    bucket = (bucket + 1U) % bucketCount;
  }

  return false;
}

void record_collision_pair(PhysicsWorldView &world, std::uint32_t idxA,
                           std::uint32_t idxB) noexcept {
  PhysicsContext &ctx = world.physics_context();
  if (ctx.collisionPairCount >= kMaxCollisionPairs) {
    return;
  }

  if (!insert_pair_key(ctx, make_pair_key(idxA, idxB))) {
    return;
  }

  ctx.collisionPairData[ctx.collisionPairCount * 2U] = idxA;
  ctx.collisionPairData[ctx.collisionPairCount * 2U + 1U] = idxB;
  ++ctx.collisionPairCount;
}

// ---------------------------------------------------------------------------
// Capsule geometry helpers
// ---------------------------------------------------------------------------

// Return the two endpoints of a capsule's internal segment (along local Y).
// The capsule is centered at `pos` with halfHeight = halfExtents.y,
// radius = halfExtents.x.
void capsule_segment(const engine::math::Vec3 &pos, const Collider &col,
                     engine::math::Vec3 &outA,
                     engine::math::Vec3 &outB) noexcept {
  const float hh = col.halfExtents.y;
  outA = engine::math::Vec3(pos.x, pos.y - hh, pos.z);
  outB = engine::math::Vec3(pos.x, pos.y + hh, pos.z);
}

// Closest point on line segment AB to point P.  Returns parameter t in [0,1].
float closest_point_on_segment(const engine::math::Vec3 &a,
                               const engine::math::Vec3 &b,
                               const engine::math::Vec3 &p,
                               engine::math::Vec3 &outClosest) noexcept {
  const engine::math::Vec3 ab = engine::math::sub(b, a);
  const float ab2 = engine::math::dot(ab, ab);
  if (ab2 < 1e-12F) {
    outClosest = a;
    return 0.0F;
  }
  float t = engine::math::dot(engine::math::sub(p, a), ab) / ab2;
  t = std::max(0.0F, std::min(1.0F, t));
  outClosest = engine::math::add(a, engine::math::mul(ab, t));
  return t;
}

// Closest points between two line segments (P0-P1 and Q0-Q1).
// Returns the two closest points and the squared distance between them.
float closest_point_segment_segment(const engine::math::Vec3 &p0,
                                    const engine::math::Vec3 &p1,
                                    const engine::math::Vec3 &q0,
                                    const engine::math::Vec3 &q1,
                                    engine::math::Vec3 &outClosestP,
                                    engine::math::Vec3 &outClosestQ) noexcept {
  const engine::math::Vec3 d1 = engine::math::sub(p1, p0);
  const engine::math::Vec3 d2 = engine::math::sub(q1, q0);
  const engine::math::Vec3 r = engine::math::sub(p0, q0);
  const float a = engine::math::dot(d1, d1);
  const float e = engine::math::dot(d2, d2);
  const float f = engine::math::dot(d2, r);

  float s = 0.0F;
  float t = 0.0F;

  if (a <= 1e-12F && e <= 1e-12F) {
    outClosestP = p0;
    outClosestQ = q0;
    const engine::math::Vec3 diff = engine::math::sub(outClosestP, outClosestQ);
    return engine::math::dot(diff, diff);
  }

  if (a <= 1e-12F) {
    s = 0.0F;
    t = std::max(0.0F, std::min(f / e, 1.0F));
  } else {
    const float c = engine::math::dot(d1, r);
    if (e <= 1e-12F) {
      t = 0.0F;
      s = std::max(0.0F, std::min(-c / a, 1.0F));
    } else {
      const float b = engine::math::dot(d1, d2);
      const float denom = a * e - b * b;

      if (denom > 1e-12F) {
        s = std::max(0.0F, std::min((b * f - c * e) / denom, 1.0F));
      } else {
        s = 0.0F;
      }

      t = (b * s + f) / e;
      if (t < 0.0F) {
        t = 0.0F;
        s = std::max(0.0F, std::min(-c / a, 1.0F));
      } else if (t > 1.0F) {
        t = 1.0F;
        s = std::max(0.0F, std::min((b - c) / a, 1.0F));
      }
    }
  }

  outClosestP = engine::math::add(p0, engine::math::mul(d1, s));
  outClosestQ = engine::math::add(q0, engine::math::mul(d2, t));
  const engine::math::Vec3 diff = engine::math::sub(outClosestP, outClosestQ);
  return engine::math::dot(diff, diff);
}

// Closest point on an AABB (defined by center + halfExtents) to a point.
engine::math::Vec3
closest_point_on_aabb(const engine::math::Vec3 &point,
                      const engine::math::Vec3 &center,
                      const engine::math::Vec3 &halfExt) noexcept {
  return engine::math::Vec3(
      std::max(center.x - halfExt.x, std::min(point.x, center.x + halfExt.x)),
      std::max(center.y - halfExt.y, std::min(point.y, center.y + halfExt.y)),
      std::max(center.z - halfExt.z, std::min(point.z, center.z + halfExt.z)));
}

// --------------------------------------------------------------------------
// Heightfield helpers
// --------------------------------------------------------------------------

// Closest point on triangle (a, b, c) to point p.
// Voronoi region projection (Christer Ericson, Real-Time Collision Detection).
engine::math::Vec3 closest_point_on_triangle(
    const engine::math::Vec3 &p, const engine::math::Vec3 &a,
    const engine::math::Vec3 &b, const engine::math::Vec3 &c) noexcept {
  const engine::math::Vec3 ab = engine::math::sub(b, a);
  const engine::math::Vec3 ac = engine::math::sub(c, a);
  const engine::math::Vec3 ap = engine::math::sub(p, a);
  const float d1 = engine::math::dot(ab, ap);
  const float d2 = engine::math::dot(ac, ap);
  if (d1 <= 0.0F && d2 <= 0.0F) {
    return a; // vertex A region
  }

  const engine::math::Vec3 bp = engine::math::sub(p, b);
  const float d3 = engine::math::dot(ab, bp);
  const float d4 = engine::math::dot(ac, bp);
  if (d3 >= 0.0F && d4 <= d3) {
    return b; // vertex B region
  }

  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
    const float v = d1 / (d1 - d3);
    return engine::math::add(a, engine::math::mul(ab, v)); // edge AB
  }

  const engine::math::Vec3 cp2 = engine::math::sub(p, c);
  const float d5 = engine::math::dot(ab, cp2);
  const float d6 = engine::math::dot(ac, cp2);
  if (d6 >= 0.0F && d5 <= d6) {
    return c; // vertex C region
  }

  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
    const float w = d2 / (d2 - d6);
    return engine::math::add(a, engine::math::mul(ac, w)); // edge AC
  }

  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return engine::math::add(
        b, engine::math::mul(engine::math::sub(c, b), w)); // edge BC
  }

  const float denom = 1.0F / (va + vb + vc);
  const float v2 = vb * denom;
  const float w2 = vc * denom;
  return engine::math::add(
      a, engine::math::add(engine::math::mul(ab, v2),
                           engine::math::mul(ac, w2))); // interior
}

// Map world X/Z to fractional heightfield grid coordinates.
void heightfield_world_to_grid(const HeightfieldData &hf,
                               const engine::math::Vec3 &hfPos, float worldX,
                               float worldZ, float &outCol,
                               float &outRow) noexcept {
  const float totalW = static_cast<float>(hf.columns - 1U) * hf.spacingX;
  const float totalD = static_cast<float>(hf.rows - 1U) * hf.spacingZ;
  const float localX = worldX - (hfPos.x - totalW * 0.5F);
  const float localZ = worldZ - (hfPos.z - totalD * 0.5F);
  outCol = localX / hf.spacingX;
  outRow = localZ / hf.spacingZ;
}

// Convert grid coordinates to world-space vertex position.
engine::math::Vec3 heightfield_grid_to_world(const HeightfieldData &hf,
                                             const engine::math::Vec3 &hfPos,
                                             std::size_t col,
                                             std::size_t row) noexcept {
  const float totalW = static_cast<float>(hf.columns - 1U) * hf.spacingX;
  const float totalD = static_cast<float>(hf.rows - 1U) * hf.spacingZ;
  const float wx =
      (hfPos.x - totalW * 0.5F) + static_cast<float>(col) * hf.spacingX;
  const float wz =
      (hfPos.z - totalD * 0.5F) + static_cast<float>(row) * hf.spacingZ;
  const float wy = hfPos.y + hf.heights[row * hf.columns + col];
  return engine::math::Vec3(wx, wy, wz);
}


} // namespace


/// Resolves one collider contact against the rigid bodies that own the two
/// collider entities. Child colliders therefore move their compound root.
void resolve_pair_contact(const PairContext &pair,
                          const engine::math::Vec3 &normal, float overlap,
                          const engine::math::Vec3 &contactPoint) noexcept {
  resolve_contact(pair.world, pair.simToken, pair.bodyEntityA, pair.bodyEntityB,
                  pair.bodyCenterA, pair.bodyCenterB, pair.bodyA, pair.bodyB,
                  pair.invMassA, pair.invMassB, pair.invMassSum, normal,
                  overlap, contactPoint, pair.colliderA, pair.colliderB);
}

/// Resolves one clipped manifold against the rigid bodies that own the two
/// collider entities, mirroring resolve_pair_contact's ownership mapping.
void resolve_pair_manifold(const PairContext &pair,
                           const engine::math::Vec3 &normal,
                           const ClippedManifold &manifold) noexcept {
  resolve_manifold_contact(pair.world, pair.simToken, pair.bodyEntityA,
                           pair.bodyEntityB, pair.bodyCenterA, pair.bodyCenterB,
                           pair.bodyA, pair.bodyB, pair.invMassA, pair.invMassB,
                           pair.invMassSum, normal, manifold, pair.colliderA,
                           pair.colliderB);
}

/// Records the colliding pair, wakes sleeping bodies, and returns whether a
/// positional/impulse response is required (false for static-static pairs).
bool record_pair_and_wake(const PairContext &pair) noexcept {
  record_collision_pair(pair.world, pair.entityA.index, pair.entityB.index);
  const float vA2 = (pair.bodyA != nullptr)
                        ? engine::math::length_sq(pair.bodyA->velocity)
                        : 0.0F;
  const float vB2 = (pair.bodyB != nullptr)
                        ? engine::math::length_sq(pair.bodyB->velocity)
                        : 0.0F;
  maybe_wake_pair(pair.bodyA, pair.bodyB, vA2, vB2);
  return pair.invMassSum > 0.0F;
}

/// Reports whether the collider's world-space linear transform requires the
/// affine support-mapped narrow phase instead of an axis-aligned fast path.
bool has_non_identity_linear_transform(
    const ColliderWorldGeometry &geometry) noexcept {
  constexpr float epsilon = 1.0e-6F;
  const auto &m = geometry.localToWorld.columns;
  return (std::fabs(m[0].x - 1.0F) > epsilon) ||
         (std::fabs(m[0].y) > epsilon) || (std::fabs(m[0].z) > epsilon) ||
         (std::fabs(m[1].x) > epsilon) ||
         (std::fabs(m[1].y - 1.0F) > epsilon) ||
         (std::fabs(m[1].z) > epsilon) || (std::fabs(m[2].x) > epsilon) ||
         (std::fabs(m[2].y) > epsilon) || (std::fabs(m[2].z - 1.0F) > epsilon);
}

engine::math::Vec3
affine_transform_point(const engine::math::Mat4 &matrix,
                       const engine::math::Vec3 &point) noexcept {
  const engine::math::Vec4 transformed = engine::math::mul(
      matrix, engine::math::Vec4(point.x, point.y, point.z, 1.0F));
  return engine::math::Vec3(transformed.x, transformed.y, transformed.z);
}

engine::math::Vec3
heightfield_vertex_world(const ColliderWorldGeometry &geometry,
                         const HeightfieldData &heightfield, std::size_t column,
                         std::size_t row) noexcept {
  const float width =
      static_cast<float>(heightfield.columns - 1U) * heightfield.spacingX;
  const float depth =
      static_cast<float>(heightfield.rows - 1U) * heightfield.spacingZ;
  const engine::math::Vec3 local(
      -width * 0.5F + static_cast<float>(column) * heightfield.spacingX,
      heightfield.heights[row * heightfield.columns + column],
      -depth * 0.5F + static_cast<float>(row) * heightfield.spacingZ);
  return affine_transform_point(geometry.localToWorld, local);
}

/// Resolves an arbitrary affine convex collider against transformed terrain.
void narrow_phase_affine_heightfield(
    const PairContext &pair, bool aIsHeightfield,
    const HeightfieldData &heightfield) noexcept {
  const ColliderWorldGeometry &heightfieldGeometry =
      aIsHeightfield ? pair.geometryA : pair.geometryB;
  const ColliderWorldGeometry &objectGeometry =
      aIsHeightfield ? pair.geometryB : pair.geometryA;
  if (objectGeometry.shape == ColliderShape::Heightfield) {
    return;
  }

  engine::math::Vec3 localMinimum(FLT_MAX, FLT_MAX, FLT_MAX);
  engine::math::Vec3 localMaximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
  const engine::math::AABB &worldBounds = objectGeometry.worldAabb;
  for (std::size_t corner = 0U; corner < 8U; ++corner) {
    const engine::math::Vec3 worldPoint(
        (corner & 1U) != 0U ? worldBounds.max.x : worldBounds.min.x,
        (corner & 2U) != 0U ? worldBounds.max.y : worldBounds.min.y,
        (corner & 4U) != 0U ? worldBounds.max.z : worldBounds.min.z);
    const engine::math::Vec3 localPoint =
        affine_transform_point(heightfieldGeometry.worldToLocal, worldPoint);
    localMinimum.x = std::min(localMinimum.x, localPoint.x);
    localMinimum.y = std::min(localMinimum.y, localPoint.y);
    localMinimum.z = std::min(localMinimum.z, localPoint.z);
    localMaximum.x = std::max(localMaximum.x, localPoint.x);
    localMaximum.y = std::max(localMaximum.y, localPoint.y);
    localMaximum.z = std::max(localMaximum.z, localPoint.z);
  }

  const float width =
      static_cast<float>(heightfield.columns - 1U) * heightfield.spacingX;
  const float depth =
      static_cast<float>(heightfield.rows - 1U) * heightfield.spacingZ;
  const float gridColumnMinimum =
      (localMinimum.x + width * 0.5F) / heightfield.spacingX;
  const float gridColumnMaximum =
      (localMaximum.x + width * 0.5F) / heightfield.spacingX;
  const float gridRowMinimum =
      (localMinimum.z + depth * 0.5F) / heightfield.spacingZ;
  const float gridRowMaximum =
      (localMaximum.z + depth * 0.5F) / heightfield.spacingZ;
  if ((gridColumnMaximum < 0.0F) || (gridRowMaximum < 0.0F) ||
      (gridColumnMinimum > static_cast<float>(heightfield.columns - 1U)) ||
      (gridRowMinimum > static_cast<float>(heightfield.rows - 1U))) {
    return;
  }

  const std::size_t columnMinimum = static_cast<std::size_t>(
      std::max(0.0F, std::min(static_cast<float>(heightfield.columns - 2U),
                              std::floor(gridColumnMinimum))));
  const std::size_t columnMaximum = static_cast<std::size_t>(
      std::max(0.0F, std::min(static_cast<float>(heightfield.columns - 2U),
                              std::floor(gridColumnMaximum))));
  const std::size_t rowMinimum = static_cast<std::size_t>(
      std::max(0.0F, std::min(static_cast<float>(heightfield.rows - 2U),
                              std::floor(gridRowMinimum))));
  const std::size_t rowMaximum = static_cast<std::size_t>(
      std::max(0.0F, std::min(static_cast<float>(heightfield.rows - 2U),
                              std::floor(gridRowMaximum))));

  const engine::math::Mat4 normalMatrix =
      engine::math::transpose(heightfieldGeometry.worldToLocal);
  const engine::math::Vec4 transformedUp4 = engine::math::mul(
      normalMatrix, engine::math::Vec4(0.0F, 1.0F, 0.0F, 0.0F));
  engine::math::Vec3 transformedUp(transformedUp4.x, transformedUp4.y,
                                   transformedUp4.z);
  const float transformedUpLength = engine::math::length(transformedUp);
  if (transformedUpLength <= 1.0e-10F) {
    return;
  }
  transformedUp = engine::math::mul(transformedUp, 1.0F / transformedUpLength);

  float bestOverlap = 0.0F;
  engine::math::Vec3 bestNormal = transformedUp;
  engine::math::Vec3 bestContact = objectGeometry.center;
  bool anyContact = false;
  for (std::size_t row = rowMinimum; row <= rowMaximum; ++row) {
    for (std::size_t column = columnMinimum; column <= columnMaximum;
         ++column) {
      const engine::math::Vec3 v00 = heightfield_vertex_world(
          heightfieldGeometry, heightfield, column, row);
      const engine::math::Vec3 v10 = heightfield_vertex_world(
          heightfieldGeometry, heightfield, column + 1U, row);
      const engine::math::Vec3 v01 = heightfield_vertex_world(
          heightfieldGeometry, heightfield, column, row + 1U);
      const engine::math::Vec3 v11 = heightfield_vertex_world(
          heightfieldGeometry, heightfield, column + 1U, row + 1U);
      const engine::math::Vec3 triangles[2][3] = {{v00, v10, v01},
                                                  {v10, v11, v01}};

      for (std::size_t triangle = 0U; triangle < 2U; ++triangle) {
        const engine::math::Vec3 edgeA =
            engine::math::sub(triangles[triangle][1], triangles[triangle][0]);
        const engine::math::Vec3 edgeB =
            engine::math::sub(triangles[triangle][2], triangles[triangle][0]);
        engine::math::Vec3 faceNormal = engine::math::cross(edgeA, edgeB);
        const float faceNormalLength = engine::math::length(faceNormal);
        if (faceNormalLength <= 1.0e-10F) {
          continue;
        }
        faceNormal = engine::math::mul(faceNormal, 1.0F / faceNormalLength);
        if (engine::math::dot(faceNormal, transformedUp) < 0.0F) {
          faceNormal = engine::math::mul(faceNormal, -1.0F);
        }

        const engine::math::Vec3 lowestPoint = collider_support_point(
            objectGeometry, engine::math::mul(faceNormal, -1.0F));
        const float signedDistance = engine::math::dot(
            engine::math::sub(lowestPoint, triangles[triangle][0]), faceNormal);
        if (signedDistance >= 0.0F) {
          continue;
        }
        const float overlap = -signedDistance;
        if (overlap > bestOverlap) {
          bestOverlap = overlap;
          bestNormal = faceNormal;
          bestContact = closest_point_on_triangle(
              lowestPoint, triangles[triangle][0], triangles[triangle][1],
              triangles[triangle][2]);
          anyContact = true;
        }
      }
    }
  }

  if (!anyContact || !record_pair_and_wake(pair)) {
    return;
  }
  const engine::math::Vec3 resolveNormal =
      aIsHeightfield ? bestNormal : engine::math::mul(bestNormal, -1.0F);
  resolve_pair_contact(pair, resolveNormal, bestOverlap, bestContact);
}

/// Heightfield vs Sphere/AABB/Capsule: sweeps the terrain triangles under the
/// object footprint and resolves against the deepest penetration.
void narrow_phase_heightfield(const PairContext &pair) noexcept {
  const bool aIsHF = (pair.colliderA.shape == ColliderShape::Heightfield);
  const engine::math::Vec3 hfPos = aIsHF ? pair.posA : pair.posB;
  const engine::math::Vec3 objPos = aIsHF ? pair.posB : pair.posA;
  const Entity hfEnt = aIsHF ? pair.entityA : pair.entityB;
  const Collider &objCol = aIsHF ? pair.colliderB : pair.colliderA;

  const HeightfieldData *hf = find_heightfield_data(pair.physicsCtx, hfEnt);
  if (hf == nullptr) {
    return;
  }
  if (pair.requiresAffineNarrowPhase) {
    narrow_phase_affine_heightfield(pair, aIsHF, *hf);
    return;
  }

  float objRadius = 0.0F;
  if (objCol.shape == ColliderShape::Sphere) {
    objRadius = objCol.halfExtents.x;
  } else if (objCol.shape == ColliderShape::Capsule) {
    objRadius = objCol.halfExtents.y + objCol.halfExtents.x;
  } else {
    objRadius = engine::math::length(objCol.halfExtents);
  }

  float gColMin = 0.0F;
  float gRowMin = 0.0F;
  float gColMax = 0.0F;
  float gRowMax = 0.0F;
  heightfield_world_to_grid(*hf, hfPos, objPos.x - objRadius,
                            objPos.z - objRadius, gColMin, gRowMin);
  heightfield_world_to_grid(*hf, hfPos, objPos.x + objRadius,
                            objPos.z + objRadius, gColMax, gRowMax);

  const auto cMin =
      static_cast<std::size_t>(std::max(0.0F, std::floor(gColMin)));
  const auto rMin =
      static_cast<std::size_t>(std::max(0.0F, std::floor(gRowMin)));
  const auto cMax = static_cast<std::size_t>(
      std::min(static_cast<float>(hf->columns - 2U), std::floor(gColMax)));
  const auto rMax = static_cast<std::size_t>(
      std::min(static_cast<float>(hf->rows - 2U), std::floor(gRowMax)));

  float bestOverlap = 0.0F;
  engine::math::Vec3 bestNormal(0.0F, 1.0F, 0.0F);
  engine::math::Vec3 bestContact = objPos;
  bool anyContact = false;

  for (std::size_t r = rMin; r <= rMax; ++r) {
    for (std::size_t c = cMin; c <= cMax; ++c) {
      const engine::math::Vec3 v00 =
          heightfield_grid_to_world(*hf, hfPos, c, r);
      const engine::math::Vec3 v10 =
          heightfield_grid_to_world(*hf, hfPos, c + 1U, r);
      const engine::math::Vec3 v01 =
          heightfield_grid_to_world(*hf, hfPos, c, r + 1U);
      const engine::math::Vec3 v11 =
          heightfield_grid_to_world(*hf, hfPos, c + 1U, r + 1U);

      engine::math::Vec3 triVerts[2][3] = {{v00, v10, v01}, {v10, v11, v01}};

      for (int ti = 0; ti < 2; ++ti) {
        const engine::math::Vec3 e1 =
            engine::math::sub(triVerts[ti][1], triVerts[ti][0]);
        const engine::math::Vec3 e2 =
            engine::math::sub(triVerts[ti][2], triVerts[ti][0]);
        engine::math::Vec3 faceN = engine::math::cross(e1, e2);
        const float faceLen = engine::math::length(faceN);
        if (faceLen < 1e-10F) {
          continue;
        }
        faceN = engine::math::mul(faceN, 1.0F / faceLen);
        if (faceN.y < 0.0F) {
          faceN = engine::math::mul(faceN, -1.0F);
        }

        float tOverlap = 0.0F;

        if (objCol.shape == ColliderShape::Sphere) {
          const engine::math::Vec3 cp = closest_point_on_triangle(
              objPos, triVerts[ti][0], triVerts[ti][1], triVerts[ti][2]);
          const engine::math::Vec3 diff = engine::math::sub(objPos, cp);
          if (engine::math::dot(diff, diff) >=
              objCol.halfExtents.x * objCol.halfExtents.x) {
            continue;
          }
          const float signedDist = engine::math::dot(
              engine::math::sub(objPos, triVerts[ti][0]), faceN);
          tOverlap = objCol.halfExtents.x - signedDist;
          if (tOverlap <= 0.0F) {
            continue;
          }
        } else if (objCol.shape == ColliderShape::Capsule) {
          const float halfH = objCol.halfExtents.y;
          const float capR = objCol.halfExtents.x;
          const engine::math::Vec3 top(objPos.x, objPos.y + halfH, objPos.z);
          const engine::math::Vec3 bot(objPos.x, objPos.y - halfH, objPos.z);
          engine::math::Vec3 cpTri = closest_point_on_triangle(
              objPos, triVerts[ti][0], triVerts[ti][1], triVerts[ti][2]);
          engine::math::Vec3 cpSeg{};
          closest_point_on_segment(bot, top, cpTri, cpSeg);
          const engine::math::Vec3 cpTri2 = closest_point_on_triangle(
              cpSeg, triVerts[ti][0], triVerts[ti][1], triVerts[ti][2]);
          const engine::math::Vec3 diff2 = engine::math::sub(cpSeg, cpTri2);
          if (engine::math::dot(diff2, diff2) >= capR * capR) {
            continue;
          }
          const float signedDist = engine::math::dot(
              engine::math::sub(cpSeg, triVerts[ti][0]), faceN);
          tOverlap = capR - signedDist;
          if (tOverlap <= 0.0F) {
            continue;
          }
        } else {
          const float signedDist = engine::math::dot(
              engine::math::sub(objPos, triVerts[ti][0]), faceN);
          const float effR = std::abs(faceN.x) * objCol.halfExtents.x +
                             std::abs(faceN.y) * objCol.halfExtents.y +
                             std::abs(faceN.z) * objCol.halfExtents.z;
          tOverlap = effR - signedDist;
          if (tOverlap <= 0.0F) {
            continue;
          }
        }

        if (tOverlap > bestOverlap) {
          bestOverlap = tOverlap;
          bestNormal = faceN;
          bestContact = closest_point_on_triangle(
              objPos, triVerts[ti][0], triVerts[ti][1], triVerts[ti][2]);
          anyContact = true;
        }
      }
    }
  }

  if (!anyContact) {
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  engine::math::Vec3 resolveNormal = bestNormal;
  if (!aIsHF) {
    resolveNormal = engine::math::mul(bestNormal, -1.0F);
  }

  resolve_pair_contact(pair, resolveNormal, bestOverlap, bestContact);
}

/// Adapts authoritative affine collider geometry to the existing GJK API.
engine::math::Vec3
support_affine_collider(const void *shapeData,
                        const engine::math::Vec3 & /*center*/,
                        const engine::math::Vec3 &direction) noexcept {
  const auto *geometry = static_cast<const ColliderWorldGeometry *>(shapeData);
  return (geometry != nullptr) ? collider_support_point(*geometry, direction)
                               : engine::math::Vec3(0.0F, 0.0F, 0.0F);
}

/// Generic GJK/EPA path for convex shapes carrying any affine hierarchy TRS.
/// Faceted pairs (box/hull) resolve through a clipped multi-point manifold so
/// resting contacts get face support; other shapes keep the EPA point.
void narrow_phase_convex_gjk(const PairContext &pair) noexcept {
  const GjkResult gjk =
      gjk_epa(&pair.geometryA, pair.posA, &support_affine_collider,
              &pair.geometryB, pair.posB, &support_affine_collider);

  if (!gjk.intersecting || gjk.depth < 1e-6F) {
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  ClippedManifold manifold{};
  if (clip_contact_manifold(pair.geometryA, pair.geometryB, gjk.normal,
                            &manifold)) {
    resolve_pair_manifold(pair, gjk.normal, manifold);
    return;
  }
  resolve_pair_contact(pair, gjk.normal, gjk.depth, gjk.contactPoint);
}

/// Capsule vs Capsule: closest points between the two core segments.
void narrow_phase_capsule_capsule(const PairContext &pair) noexcept {
  engine::math::Vec3 segAa, segAb, segBa, segBb;
  capsule_segment(pair.posA, pair.colliderA, segAa, segAb);
  capsule_segment(pair.posB, pair.colliderB, segBa, segBb);
  engine::math::Vec3 closestA, closestB;
  const float dist2 = closest_point_segment_segment(segAa, segAb, segBa, segBb,
                                                    closestA, closestB);
  const float rA = pair.colliderA.halfExtents.x;
  const float rB = pair.colliderB.halfExtents.x;
  const float sumR = rA + rB;
  if (dist2 >= sumR * sumR) {
    return;
  }
  if (!record_pair_and_wake(pair)) {
    return;
  }
  const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
  const engine::math::Vec3 diff = engine::math::sub(closestB, closestA);
  const engine::math::Vec3 normal = (dist2 > 0.0F)
                                        ? engine::math::mul(diff, 1.0F / dist)
                                        : engine::math::Vec3(0.0F, 1.0F, 0.0F);
  const float overlap = sumR - dist;
  const engine::math::Vec3 contactPt =
      engine::math::mul(engine::math::add(closestA, closestB), 0.5F);
  resolve_pair_contact(pair, normal, overlap, contactPt);
}

/// Capsule vs Sphere (either ordering): sphere against the capsule segment.
void narrow_phase_capsule_sphere(const PairContext &pair) noexcept {
  const bool aIsCap = (pair.colliderA.shape == ColliderShape::Capsule);
  const engine::math::Vec3 capPos = aIsCap ? pair.posA : pair.posB;
  const engine::math::Vec3 sphPos = aIsCap ? pair.posB : pair.posA;
  const Collider &capCol = aIsCap ? pair.colliderA : pair.colliderB;
  const float capR = capCol.halfExtents.x;
  const float sphR = (aIsCap ? pair.colliderB : pair.colliderA).halfExtents.x;
  engine::math::Vec3 segA, segB;
  capsule_segment(capPos, capCol, segA, segB);
  engine::math::Vec3 closest;
  closest_point_on_segment(segA, segB, sphPos, closest);
  const engine::math::Vec3 diff = engine::math::sub(sphPos, closest);
  const float dist2 = engine::math::dot(diff, diff);
  const float sumR = capR + sphR;
  if (dist2 >= sumR * sumR) {
    return;
  }
  if (!record_pair_and_wake(pair)) {
    return;
  }
  const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
  engine::math::Vec3 normal = (dist2 > 0.0F)
                                  ? engine::math::mul(diff, 1.0F / dist)
                                  : engine::math::Vec3(0.0F, 1.0F, 0.0F);
  if (!aIsCap) {
    normal = engine::math::mul(normal, -1.0F);
  }
  const float overlap = sumR - dist;
  const engine::math::Vec3 contactPt =
      engine::math::mul(engine::math::add(closest, sphPos), 0.5F);
  resolve_pair_contact(pair, normal, overlap, contactPt);
}

/// Capsule vs AABB (either ordering): closest segment/box point pair search.
void narrow_phase_capsule_aabb(const PairContext &pair) noexcept {
  const bool aIsCap = (pair.colliderA.shape == ColliderShape::Capsule);
  const engine::math::Vec3 capPos = aIsCap ? pair.posA : pair.posB;
  const engine::math::Vec3 boxPos = aIsCap ? pair.posB : pair.posA;
  const Collider &capCol = aIsCap ? pair.colliderA : pair.colliderB;
  const Collider &boxCol = aIsCap ? pair.colliderB : pair.colliderA;
  const float capR = capCol.halfExtents.x;

  engine::math::Vec3 segA, segB;
  capsule_segment(capPos, capCol, segA, segB);

  const engine::math::Vec3 cpA =
      closest_point_on_aabb(segA, boxPos, boxCol.halfExtents);
  const engine::math::Vec3 cpB =
      closest_point_on_aabb(segB, boxPos, boxCol.halfExtents);

  engine::math::Vec3 segClosest;
  closest_point_on_segment(segA, segB, boxPos, segClosest);
  const engine::math::Vec3 cpC =
      closest_point_on_aabb(segClosest, boxPos, boxCol.halfExtents);

  auto seg_dist2 = [](const engine::math::Vec3 &segPt,
                      const engine::math::Vec3 &aabbPt) {
    const engine::math::Vec3 d = engine::math::sub(segPt, aabbPt);
    return engine::math::dot(d, d);
  };
  const float d2A = seg_dist2(segA, cpA);
  const float d2B = seg_dist2(segB, cpB);
  const float d2C = seg_dist2(segClosest, cpC);

  engine::math::Vec3 bestSeg = segA;
  engine::math::Vec3 bestBox = cpA;
  float bestDist2 = d2A;
  if (d2B < bestDist2) {
    bestSeg = segB;
    bestBox = cpB;
    bestDist2 = d2B;
  }
  if (d2C < bestDist2) {
    bestSeg = segClosest;
    bestBox = cpC;
    bestDist2 = d2C;
  }

  engine::math::Vec3 finalSeg;
  closest_point_on_segment(segA, segB, bestBox, finalSeg);
  const engine::math::Vec3 finalBox =
      closest_point_on_aabb(finalSeg, boxPos, boxCol.halfExtents);
  const engine::math::Vec3 diff = engine::math::sub(finalSeg, finalBox);
  const float dist2 = engine::math::dot(diff, diff);

  if (dist2 >= capR * capR) {
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
  engine::math::Vec3 normal;
  if (dist2 > 0.0F) {
    normal = engine::math::mul(diff, 1.0F / dist);
  } else {
    // Degenerate deep contact (axis touching the box): separate along the
    // horizontal center offset instead of an arbitrary vertical pop.
    const engine::math::Vec3 horizontal(capPos.x - boxPos.x, 0.0F,
                                        capPos.z - boxPos.z);
    const float horizontal2 = engine::math::dot(horizontal, horizontal);
    normal = (horizontal2 > 1e-12F)
                 ? engine::math::mul(horizontal,
                                     1.0F / std::sqrt(horizontal2))
                 : engine::math::Vec3(0.0F, 1.0F, 0.0F);
  }
  if (aIsCap) {
    normal = engine::math::mul(normal, -1.0F);
  }
  const float overlap = capR - dist;
  const engine::math::Vec3 contactPt =
      engine::math::mul(engine::math::add(finalSeg, finalBox), 0.5F);
  resolve_pair_contact(pair, normal, overlap, contactPt);
}

/// Sphere vs Sphere: distance test with a speculative contact for near misses.
void narrow_phase_sphere_sphere(const PairContext &pair) noexcept {
  const float rA = pair.colliderA.halfExtents.x;
  const float rB = pair.colliderB.halfExtents.x;
  const float dx = pair.posB.x - pair.posA.x;
  const float dy = pair.posB.y - pair.posA.y;
  const float dz = pair.posB.z - pair.posA.z;
  const float dist2 = dx * dx + dy * dy + dz * dz;
  const float sumR = rA + rB;
  if (dist2 >= sumR * sumR) {
    const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
    const float gap = dist - sumR;
    if ((gap > 0.0F) && (gap < pair.speculativeDt * 300.0F)) {
      const engine::math::Vec3 specN =
          (dist2 > 0.0F) ? engine::math::Vec3(dx / dist, dy / dist, dz / dist)
                         : engine::math::Vec3(0.0F, 1.0F, 0.0F);
      resolve_speculative_contact(pair.bodyA, pair.bodyB, specN, pair.invMassA,
                                  pair.invMassB, pair.invMassSum, gap,
                                  pair.speculativeDt);
    }
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
  const float nx = dx / dist;
  const float ny = dy / dist;
  const float nz = dz / dist;
  const float overlap = sumR - dist;
  const float moveA = overlap * (pair.invMassA / pair.invMassSum);
  const float moveB = overlap * (pair.invMassB / pair.invMassSum);

  Transform *mutableA =
      pair.world.get_transform_write_ptr(pair.entityA, pair.simToken);
  Transform *mutableB =
      pair.world.get_transform_write_ptr(pair.entityB, pair.simToken);
  if ((mutableA == nullptr) || (mutableB == nullptr)) {
    return;
  }
  mutableA->position.x -= nx * moveA;
  mutableA->position.y -= ny * moveA;
  mutableA->position.z -= nz * moveA;
  mutableB->position.x += nx * moveB;
  mutableB->position.y += ny * moveB;
  mutableB->position.z += nz * moveB;

  const engine::math::Vec3 contactNormal(nx, ny, nz);
  const engine::math::Vec3 contactPt = engine::math::mul(
      engine::math::add(mutableA->position, mutableB->position), 0.5F);
  const float combinedRest =
      std::max(pair.colliderA.restitution, pair.colliderB.restitution);
  const float combinedStaticFric =
      std::sqrt(pair.colliderA.staticFriction * pair.colliderB.staticFriction);
  const float combinedDynFric = std::sqrt(pair.colliderA.dynamicFriction *
                                          pair.colliderB.dynamicFriction);
  apply_velocity_impulse(pair.bodyA, pair.bodyB, contactNormal, pair.invMassA,
                         pair.invMassB, pair.invMassSum,
                         engine::math::sub(contactPt, mutableA->position),
                         engine::math::sub(contactPt, mutableB->position),
                         combinedRest, combinedStaticFric, combinedDynFric);
}

/// AABB vs Sphere (either ordering): clamped closest point on the box, with a
/// speculative contact for near misses.
void narrow_phase_aabb_sphere(const PairContext &pair) noexcept {
  const bool aIsBox = (pair.colliderA.shape == ColliderShape::AABB);
  const float boxX = aIsBox ? pair.posA.x : pair.posB.x;
  const float boxY = aIsBox ? pair.posA.y : pair.posB.y;
  const float boxZ = aIsBox ? pair.posA.z : pair.posB.z;
  const float sphX = aIsBox ? pair.posB.x : pair.posA.x;
  const float sphY = aIsBox ? pair.posB.y : pair.posA.y;
  const float sphZ = aIsBox ? pair.posB.z : pair.posA.z;
  const Collider &boxCol = aIsBox ? pair.colliderA : pair.colliderB;
  const float radius = (aIsBox ? pair.colliderB : pair.colliderA).halfExtents.x;

  const float cpx = std::max(boxX - boxCol.halfExtents.x,
                             std::min(sphX, boxX + boxCol.halfExtents.x));
  const float cpy = std::max(boxY - boxCol.halfExtents.y,
                             std::min(sphY, boxY + boxCol.halfExtents.y));
  const float cpz = std::max(boxZ - boxCol.halfExtents.z,
                             std::min(sphZ, boxZ + boxCol.halfExtents.z));

  const float dx = sphX - cpx;
  const float dy = sphY - cpy;
  const float dz = sphZ - cpz;
  const float dist2 = dx * dx + dy * dy + dz * dz;
  if (dist2 >= radius * radius) {
    const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
    const float gap = dist - radius;
    if ((gap > 0.0F) && (gap < pair.speculativeDt * 300.0F)) {
      engine::math::Vec3 specN =
          (dist2 > 0.0F) ? engine::math::Vec3(dx / dist, dy / dist, dz / dist)
                         : engine::math::Vec3(0.0F, 1.0F, 0.0F);
      if (!aIsBox) {
        specN = engine::math::mul(specN, -1.0F);
      }
      resolve_speculative_contact(pair.bodyA, pair.bodyB, specN, pair.invMassA,
                                  pair.invMassB, pair.invMassSum, gap,
                                  pair.speculativeDt);
    }
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
  float nx = (dist2 > 0.0F) ? (dx / dist) : 0.0F;
  float ny = (dist2 > 0.0F) ? (dy / dist) : 1.0F;
  float nz = (dist2 > 0.0F) ? (dz / dist) : 0.0F;
  const float overlap = radius - dist;

  if (!aIsBox) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  }

  const float moveA = overlap * (pair.invMassA / pair.invMassSum);
  const float moveB = overlap * (pair.invMassB / pair.invMassSum);

  Transform *mutableA =
      pair.world.get_transform_write_ptr(pair.entityA, pair.simToken);
  Transform *mutableB =
      pair.world.get_transform_write_ptr(pair.entityB, pair.simToken);
  if ((mutableA == nullptr) || (mutableB == nullptr)) {
    return;
  }
  mutableA->position.x -= nx * moveA;
  mutableA->position.y -= ny * moveA;
  mutableA->position.z -= nz * moveA;
  mutableB->position.x += nx * moveB;
  mutableB->position.y += ny * moveB;
  mutableB->position.z += nz * moveB;

  const engine::math::Vec3 aabbSphNormal(nx, ny, nz);
  const engine::math::Vec3 closestPt(cpx, cpy, cpz);
  const float combinedRest =
      std::max(pair.colliderA.restitution, pair.colliderB.restitution);
  const float combinedStaticFric =
      std::sqrt(pair.colliderA.staticFriction * pair.colliderB.staticFriction);
  const float combinedDynFric = std::sqrt(pair.colliderA.dynamicFriction *
                                          pair.colliderB.dynamicFriction);
  apply_velocity_impulse(pair.bodyA, pair.bodyB, aabbSphNormal, pair.invMassA,
                         pair.invMassB, pair.invMassSum,
                         engine::math::sub(closestPt, mutableA->position),
                         engine::math::sub(closestPt, mutableB->position),
                         combinedRest, combinedStaticFric, combinedDynFric);
}

/// AABB vs AABB: axis-overlap test, smallest-axis push-out, and a speculative
/// contact when the pair is separated but closing fast.
void narrow_phase_aabb_aabb(const PairContext &pair) noexcept {
  const float ax = pair.posA.x;
  const float ay = pair.posA.y;
  const float az = pair.posA.z;
  const float bx = pair.posB.x;
  const float by = pair.posB.y;
  const float bz = pair.posB.z;
  const Collider &colliderA = pair.colliderA;
  const Collider &colliderB = pair.colliderB;

  const float overlapX =
      axis_overlap(ax - colliderA.halfExtents.x, ax + colliderA.halfExtents.x,
                   bx - colliderB.halfExtents.x, bx + colliderB.halfExtents.x);
  const float overlapY =
      axis_overlap(ay - colliderA.halfExtents.y, ay + colliderA.halfExtents.y,
                   by - colliderB.halfExtents.y, by + colliderB.halfExtents.y);
  const float overlapZ =
      axis_overlap(az - colliderA.halfExtents.z, az + colliderA.halfExtents.z,
                   bz - colliderB.halfExtents.z, bz + colliderB.halfExtents.z);

  const bool hasOverlap =
      (overlapX > 0.0F) && (overlapY > 0.0F) && (overlapZ > 0.0F);

  if (!hasOverlap) {
    const float minOverlap = std::min({overlapX, overlapY, overlapZ});
    const float gap = -minOverlap;

    if ((gap > 0.0F) && (gap < pair.speculativeDt * 300.0F)) {
      float specNx = 0.0F;
      float specNy = 0.0F;
      float specNz = 0.0F;
      if (overlapX <= overlapY && overlapX <= overlapZ) {
        specNx = sign_or_positive(bx - ax);
      } else if (overlapY <= overlapZ) {
        specNy = sign_or_positive(by - ay);
      } else {
        specNz = sign_or_positive(bz - az);
      }
      const engine::math::Vec3 specNormal(specNx, specNy, specNz);
      resolve_speculative_contact(pair.bodyA, pair.bodyB, specNormal,
                                  pair.invMassA, pair.invMassB, pair.invMassSum,
                                  gap, pair.speculativeDt);
    }
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  float pushAmount = overlapX;
  float pushX = sign_or_positive(bx - ax);
  float pushY = 0.0F;
  float pushZ = 0.0F;

  if (overlapY < pushAmount) {
    pushAmount = overlapY;
    pushX = 0.0F;
    pushY = sign_or_positive(by - ay);
    pushZ = 0.0F;
  }

  if (overlapZ < pushAmount) {
    pushAmount = overlapZ;
    pushX = 0.0F;
    pushY = 0.0F;
    pushZ = sign_or_positive(bz - az);
  }

  const engine::math::Vec3 aabbNormal(pushX, pushY, pushZ);
  ClippedManifold manifold{};
  if (clip_contact_manifold(pair.geometryA, pair.geometryB, aabbNormal,
                            &manifold)) {
    resolve_pair_manifold(pair, aabbNormal, manifold);
    return;
  }

  const float moveA = pushAmount * (pair.invMassA / pair.invMassSum);
  const float moveB = pushAmount * (pair.invMassB / pair.invMassSum);

  Transform *mutableA =
      pair.world.get_transform_write_ptr(pair.entityA, pair.simToken);
  Transform *mutableB =
      pair.world.get_transform_write_ptr(pair.entityB, pair.simToken);
  if ((mutableA == nullptr) || (mutableB == nullptr)) {
    return;
  }

  mutableA->position.x -= pushX * moveA;
  mutableA->position.y -= pushY * moveA;
  mutableA->position.z -= pushZ * moveA;
  mutableB->position.x += pushX * moveB;
  mutableB->position.y += pushY * moveB;
  mutableB->position.z += pushZ * moveB;

  const engine::math::Vec3 midPt = engine::math::mul(
      engine::math::add(mutableA->position, mutableB->position), 0.5F);
  const float combinedRest =
      std::max(colliderA.restitution, colliderB.restitution);
  const float combinedStaticFric =
      std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
  const float combinedDynFric =
      std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
  apply_velocity_impulse(pair.bodyA, pair.bodyB, aabbNormal, pair.invMassA,
                         pair.invMassB, pair.invMassSum,
                         engine::math::sub(midPt, mutableA->position),
                         engine::math::sub(midPt, mutableB->position),
                         combinedRest, combinedStaticFric, combinedDynFric);
}


} // namespace engine::physics
