#include "engine/physics/physics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/math/aabb.h"
#include "engine/math/quat.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/vec3.h"
#include "engine/physics/ccd.h"
#include "engine/physics/collider.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/convex_hull.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace engine::physics {

// Storage for convex hull data.  Limited to 256 active hulls.
// Maps entity index → hull slot.
static constexpr std::size_t kMaxConvexHulls = 256U;
static std::array<ConvexHullData, kMaxConvexHulls> g_convexHullData{};
static std::array<std::uint32_t, kMaxConvexHulls> g_convexHullEntityIndex{};
static std::size_t g_convexHullCount = 0U;

ConvexHullData *find_hull_data(std::uint32_t entityIndex) noexcept {
  for (std::size_t i = 0U; i < g_convexHullCount; ++i) {
    if (g_convexHullEntityIndex[i] == entityIndex) {
      return &g_convexHullData[i];
    }
  }
  return nullptr;
}

ConvexHullData *allocate_hull_data(std::uint32_t entityIndex) noexcept {
  ConvexHullData *existing = find_hull_data(entityIndex);
  if (existing != nullptr) {
    return existing;
  }
  if (g_convexHullCount >= kMaxConvexHulls) {
    return nullptr;
  }
  g_convexHullEntityIndex[g_convexHullCount] = entityIndex;
  g_convexHullData[g_convexHullCount] = ConvexHullData{};
  return &g_convexHullData[g_convexHullCount++];
}

// Storage for heightfield data.  Limited to 16 active heightfields.
static constexpr std::size_t kMaxHeightfields = 16U;
static std::array<HeightfieldData, kMaxHeightfields> g_heightfieldData{};
static std::array<std::uint32_t, kMaxHeightfields> g_heightfieldEntityIndex{};
static std::size_t g_heightfieldCount = 0U;

HeightfieldData *find_heightfield_data(std::uint32_t entityIndex) noexcept {
  for (std::size_t i = 0U; i < g_heightfieldCount; ++i) {
    if (g_heightfieldEntityIndex[i] == entityIndex) {
      return &g_heightfieldData[i];
    }
  }
  return nullptr;
}

HeightfieldData *allocate_heightfield_data(std::uint32_t entityIndex) noexcept {
  HeightfieldData *existing = find_heightfield_data(entityIndex);
  if (existing != nullptr) {
    return existing;
  }
  if (g_heightfieldCount >= kMaxHeightfields) {
    return nullptr;
  }
  g_heightfieldEntityIndex[g_heightfieldCount] = entityIndex;
  g_heightfieldData[g_heightfieldCount] = HeightfieldData{};
  return &g_heightfieldData[g_heightfieldCount++];
}

// Public accessors used by the runtime bridge.
bool set_convex_hull_data_impl(std::uint32_t entityIndex,
                               const ConvexHullData &hull) noexcept {
  ConvexHullData *slot = allocate_hull_data(entityIndex);
  if (slot == nullptr) {
    return false;
  }
  *slot = hull;
  return true;
}

const ConvexHullData *
get_convex_hull_data_impl(std::uint32_t entityIndex) noexcept {
  return find_hull_data(entityIndex);
}

bool set_heightfield_data_impl(std::uint32_t entityIndex,
                               const HeightfieldData &hf) noexcept {
  HeightfieldData *slot = allocate_heightfield_data(entityIndex);
  if (slot == nullptr) {
    return false;
  }
  *slot = hf;
  return true;
}

const HeightfieldData *
get_heightfield_data_impl(std::uint32_t entityIndex) noexcept {
  return find_heightfield_data(entityIndex);
}

namespace {

constexpr float kStaticInverseMass = 0.0F;
constexpr float kDefaultCellSize = 4.0F;
constexpr std::size_t kSpatialHashBuckets = 4096U;
constexpr std::uint32_t kSpatialHashEmpty = 0xFFFFFFFFU;

constexpr float kSleepThreshold = 0.01F;
constexpr std::uint8_t kSleepFramesRequired = 60U;
constexpr float kAngularDampingPerSecond = 1.8F;
constexpr float kMaxAngularSpeed = 3.0F;

float axis_overlap(float aMin, float aMax, float bMin, float bMax) noexcept {
  const float left = std::max(aMin, bMin);
  const float right = std::min(aMax, bMax);
  return right - left;
}

float sign_or_positive(float value) noexcept {
  return (value < 0.0F) ? -1.0F : 1.0F;
}

void begin_generation(std::uint32_t *generation, std::uint32_t *stamps,
                      std::size_t stampCount) noexcept {
  if ((generation == nullptr) || (stamps == nullptr)) {
    return;
  }

  ++(*generation);
  if (*generation != 0U) {
    return;
  }

  for (std::size_t i = 0U; i < stampCount; ++i) {
    stamps[i] = 0U;
  }
  *generation = 1U;
}

std::uint64_t make_pair_key(std::uint32_t idxA, std::uint32_t idxB) noexcept {
  const std::uint32_t lo = std::min(idxA, idxB);
  const std::uint32_t hi = std::max(idxA, idxB);
  return (static_cast<std::uint64_t>(lo) << 32U) |
         static_cast<std::uint64_t>(hi);
}

bool insert_pair_key(runtime::World::PhysicsContext &ctx,
                     std::uint64_t key) noexcept {
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

void record_collision_pair(runtime::World &world, std::uint32_t idxA,
                           std::uint32_t idxB) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (ctx.collisionPairCount >= runtime::World::kMaxCollisionPairs) {
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
void capsule_segment(const engine::math::Vec3 &pos,
                     const runtime::Collider &col, engine::math::Vec3 &outA,
                     engine::math::Vec3 &outB) noexcept {
  const float hh = col.halfExtents.y; // halfHeight
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
    // Both segments degenerate to points.
    outClosestP = p0;
    outClosestQ = q0;
    const engine::math::Vec3 diff = engine::math::sub(outClosestP, outClosestQ);
    return engine::math::dot(diff, diff);
  }

  if (a <= 1e-12F) {
    // First segment degenerates.
    s = 0.0F;
    t = std::max(0.0F, std::min(f / e, 1.0F));
  } else {
    const float c = engine::math::dot(d1, r);
    if (e <= 1e-12F) {
      // Second segment degenerates.
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

// Compute the AABB halfExtents for a capsule (for broadphase insertion).
engine::math::Vec3
capsule_aabb_half_extents(const runtime::Collider &col) noexcept {
  const float r = col.halfExtents.x;
  const float hh = col.halfExtents.y;
  return engine::math::Vec3(r, hh + r, r);
}

// --------------------------------------------------------------------------
// Heightfield helpers
// --------------------------------------------------------------------------

// Sample height at fractional grid (col, row) with bilinear interpolation.
float heightfield_sample(const HeightfieldData &hf, float col,
                         float row) noexcept {
  const auto maxCol = static_cast<float>(hf.columns - 1U);
  const auto maxRow = static_cast<float>(hf.rows - 1U);
  col = std::max(0.0F, std::min(col, maxCol));
  row = std::max(0.0F, std::min(row, maxRow));

  const auto c0 = static_cast<std::size_t>(col);
  const auto r0 = static_cast<std::size_t>(row);
  const std::size_t c1 = (c0 + 1U < hf.columns) ? c0 + 1U : c0;
  const std::size_t r1 = (r0 + 1U < hf.rows) ? r0 + 1U : r0;

  const float fc = col - static_cast<float>(c0);
  const float fr = row - static_cast<float>(r0);

  const float h00 = hf.heights[r0 * hf.columns + c0];
  const float h10 = hf.heights[r0 * hf.columns + c1];
  const float h01 = hf.heights[r1 * hf.columns + c0];
  const float h11 = hf.heights[r1 * hf.columns + c1];

  const float h0 = h00 + (h10 - h00) * fc;
  const float h1 = h01 + (h11 - h01) * fc;
  return h0 + (h1 - h0) * fr;
}

// Get height at world X/Z relative to the heightfield's transform position.
// Returns the world-space Y height.  The heightfield origin is at
// position - (totalWidth/2, 0, totalDepth/2).
float heightfield_get_height(const HeightfieldData &hf,
                             const engine::math::Vec3 &hfPos, float worldX,
                             float worldZ) noexcept {
  const float totalW = static_cast<float>(hf.columns - 1U) * hf.spacingX;
  const float totalD = static_cast<float>(hf.rows - 1U) * hf.spacingZ;
  const float localX = worldX - (hfPos.x - totalW * 0.5F);
  const float localZ = worldZ - (hfPos.z - totalD * 0.5F);
  const float gridCol = localX / hf.spacingX;
  const float gridRow = localZ / hf.spacingZ;
  return hfPos.y + heightfield_sample(hf, gridCol, gridRow);
}

// Compute the heightfield surface normal at a world X/Z via finite
// differences.
engine::math::Vec3 heightfield_get_normal(const HeightfieldData &hf,
                                          const engine::math::Vec3 &hfPos,
                                          float worldX, float worldZ) noexcept {
  const float eps = hf.spacingX * 0.5F;
  const float hL = heightfield_get_height(hf, hfPos, worldX - eps, worldZ);
  const float hR = heightfield_get_height(hf, hfPos, worldX + eps, worldZ);
  const float hD = heightfield_get_height(hf, hfPos, worldX, worldZ - eps);
  const float hU = heightfield_get_height(hf, hfPos, worldX, worldZ + eps);
  engine::math::Vec3 n(hL - hR, 2.0F * eps, hD - hU);
  const float len = engine::math::length(n);
  if (len > 1e-10F) {
    n = engine::math::mul(n, 1.0F / len);
  }
  return n;
}

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


// Ray-vs-heightfield intersection by grid marching.
bool ray_intersects_heightfield(const engine::math::Ray &ray,
                                const engine::math::Vec3 &hfPos,
                                const HeightfieldData &hf, float maxDist,
                                float &outT,
                                engine::math::Vec3 &outNormal) noexcept {
  // Step along the ray in small increments and check height.
  constexpr std::size_t kMaxSteps = 256U;
  const float stepSize = std::min(hf.spacingX, hf.spacingZ) * 0.5F;
  const float totalW = static_cast<float>(hf.columns - 1U) * hf.spacingX;
  const float totalD = static_cast<float>(hf.rows - 1U) * hf.spacingZ;
  const float hfMinX = hfPos.x - totalW * 0.5F;
  const float hfMaxX = hfPos.x + totalW * 0.5F;
  const float hfMinZ = hfPos.z - totalD * 0.5F;
  const float hfMaxZ = hfPos.z + totalD * 0.5F;

  float prevT = 0.0F;
  float prevY = ray.origin.y;
  float prevH = heightfield_get_height(hf, hfPos, ray.origin.x, ray.origin.z);

  for (std::size_t step = 1U; step <= kMaxSteps; ++step) {
    const float t = static_cast<float>(step) * stepSize;
    if (t > maxDist) {
      break;
    }
    const engine::math::Vec3 p =
        engine::math::add(ray.origin, engine::math::mul(ray.direction, t));

    // Skip if outside heightfield XZ bounds.
    if (p.x < hfMinX || p.x > hfMaxX || p.z < hfMinZ || p.z > hfMaxZ) {
      prevT = t;
      prevY = p.y;
      prevH = hfPos.y + hf.minY; // approximate
      continue;
    }

    const float h = heightfield_get_height(hf, hfPos, p.x, p.z);
    if (p.y <= h && prevY > prevH) {
      // Crossed from above to below: interpolate hit t.
      const float dPrev = prevY - prevH;
      const float dCurr = h - p.y;
      const float ratio = dPrev / (dPrev + dCurr);
      outT = prevT + (t - prevT) * ratio;
      const engine::math::Vec3 hitPt =
          engine::math::add(ray.origin, engine::math::mul(ray.direction, outT));
      outNormal = heightfield_get_normal(hf, hfPos, hitPt.x, hitPt.z);
      return true;
    }
    prevT = t;
    prevY = p.y;
    prevH = h;
  }
  return false;
}

// Compute the AABB half-extents for broadphase purposes regardless of shape.
engine::math::Vec3
broadphase_half_extents(const runtime::Collider &col) noexcept {
  if (col.shape == runtime::ColliderShape::Capsule) {
    return capsule_aabb_half_extents(col);
  }
  // For heightfield, halfExtents is set externally from the heightfield data.
  return col.halfExtents;
}

// Wake a sleeping body if the other body has velocity above threshold.
void maybe_wake_pair(runtime::RigidBody *bodyA, runtime::RigidBody *bodyB,
                     float vA2, float vB2) noexcept {
  if ((bodyA != nullptr) && bodyA->sleeping && (vB2 > kSleepThreshold)) {
    bodyA->sleeping = false;
    bodyA->sleepFrameCount = 0U;
  }
  if ((bodyB != nullptr) && bodyB->sleeping && (vA2 > kSleepThreshold)) {
    bodyB->sleeping = false;
    bodyB->sleepFrameCount = 0U;
  }
}

// Forward declaration.
void apply_velocity_impulse(runtime::RigidBody *bodyA,
                            runtime::RigidBody *bodyB,
                            const engine::math::Vec3 &normal, float invMassA,
                            float invMassB, float invMassSum,
                            const engine::math::Vec3 &contactOffsetA,
                            const engine::math::Vec3 &contactOffsetB,
                            float restitution, float staticFric,
                            float dynamicFric) noexcept;

// Resolve a collision between two shapes given contact normal, overlap, and
// the contact point.  Applies positional correction and velocity impulse.
void resolve_contact(runtime::World &world,
                     const runtime::World::SimulationAccessToken &simToken,
                     runtime::Entity entityA, runtime::Entity entityB,
                     runtime::RigidBody *bodyA, runtime::RigidBody *bodyB,
                     float invMassA, float invMassB, float invMassSum,
                     const engine::math::Vec3 &normal, float overlap,
                     const engine::math::Vec3 &contactPt,
                     const runtime::Collider &colliderA,
                     const runtime::Collider &colliderB) noexcept {
  const float moveA = overlap * (invMassA / invMassSum);
  const float moveB = overlap * (invMassB / invMassSum);

  runtime::Transform *mutableA =
      world.get_transform_write_ptr(entityA, simToken);
  runtime::Transform *mutableB =
      world.get_transform_write_ptr(entityB, simToken);
  if ((mutableA == nullptr) || (mutableB == nullptr)) {
    return;
  }

  mutableA->position =
      engine::math::sub(mutableA->position, engine::math::mul(normal, moveA));
  mutableB->position =
      engine::math::add(mutableB->position, engine::math::mul(normal, moveB));

  const float combinedRest =
      std::max(colliderA.restitution, colliderB.restitution);
  const float combinedStaticFric =
      std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
  const float combinedDynFric =
      std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
  apply_velocity_impulse(bodyA, bodyB, normal, invMassA, invMassB, invMassSum,
                         engine::math::sub(contactPt, mutableA->position),
                         engine::math::sub(contactPt, mutableB->position),
                         combinedRest, combinedStaticFric, combinedDynFric);
}

// Resolve a speculative contact (E2a/E2b).
// Bodies are NOT yet overlapping but are approaching. Apply a clamped velocity
// impulse to prevent penetration in the next frame — no positional correction,
// no restitution, and the impulse is clamped to zero minimum (can only push
// apart, never pull together).
void resolve_speculative_contact(runtime::RigidBody *bodyA,
                                 runtime::RigidBody *bodyB,
                                 const engine::math::Vec3 &normal,
                                 float invMassA, float invMassB,
                                 float invMassSum, float gap,
                                 float deltaSeconds) noexcept {
  if (invMassSum <= 0.0F) {
    return;
  }
  if (deltaSeconds <= 0.0F) {
    return;
  }

  const engine::math::Vec3 velA = (bodyA != nullptr)
                                      ? bodyA->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 velB = (bodyB != nullptr)
                                      ? bodyB->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 relVel = engine::math::sub(velB, velA);
  const float relVelAlongNormal = engine::math::dot(relVel, normal);

  // Only act if bodies are approaching (relative velocity along normal < 0
  // means B is moving toward A along the contact normal).
  if (relVelAlongNormal >= 0.0F) {
    return;
  }

  // The velocity that would exactly close the gap in one frame.
  // We only need to cancel the EXCESS approach velocity beyond this.
  const float closeVel = gap / deltaSeconds;

  // Impulse to reduce approach velocity to at most what would close the gap.
  // Clamp: never apply more impulse than needed (prevents ghost collisions).
  const float excessVel = -relVelAlongNormal - closeVel;
  if (excessVel <= 0.0F) {
    return; // Approach velocity won't cause penetration.
  }

  // Clamped impulse magnitude — no restitution (speculative = conservative).
  const float impulseMagnitude = excessVel / invMassSum;

  if ((bodyA != nullptr) && (invMassA > 0.0F)) {
    bodyA->velocity = engine::math::sub(
        bodyA->velocity,
        engine::math::mul(normal, impulseMagnitude * invMassA));
  }
  if ((bodyB != nullptr) && (invMassB > 0.0F)) {
    bodyB->velocity = engine::math::add(
        bodyB->velocity,
        engine::math::mul(normal, impulseMagnitude * invMassB));
  }
}

void apply_velocity_impulse(runtime::RigidBody *bodyA,
                            runtime::RigidBody *bodyB,
                            const engine::math::Vec3 &normal, float invMassA,
                            float invMassB, float invMassSum,
                            const engine::math::Vec3 &contactOffsetA,
                            const engine::math::Vec3 &contactOffsetB,
                            float restitution, float staticFric,
                            float dynamicFric) noexcept {
  const engine::math::Vec3 velA = (bodyA != nullptr)
                                      ? bodyA->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 velB = (bodyB != nullptr)
                                      ? bodyB->velocity
                                      : engine::math::Vec3(0.0F, 0.0F, 0.0F);
  const engine::math::Vec3 relVel = engine::math::sub(velB, velA);
  const float relVelAlongNormal = engine::math::dot(relVel, normal);
  if (relVelAlongNormal < 0.0F) {
    const float impulseMagnitude =
        -(1.0F + restitution) * relVelAlongNormal / invMassSum;
    const engine::math::Vec3 impulseVec =
        engine::math::mul(normal, impulseMagnitude);
    if ((bodyA != nullptr) && (invMassA > 0.0F)) {
      bodyA->velocity = engine::math::sub(
          bodyA->velocity,
          engine::math::mul(normal, impulseMagnitude * invMassA));
      // Keep static-environment contacts stable: only transfer angular
      // impulse when both bodies are dynamic.
      if ((bodyA->inverseInertia > 0.0F) && (invMassB > 0.0F)) {
        const engine::math::Vec3 angImpulse =
            engine::math::mul(engine::math::cross(contactOffsetA, impulseVec),
                              bodyA->inverseInertia);
        bodyA->angularVelocity =
            engine::math::sub(bodyA->angularVelocity, angImpulse);
        const float angSpeedSq =
            engine::math::length_sq(bodyA->angularVelocity);
        if (angSpeedSq > (kMaxAngularSpeed * kMaxAngularSpeed)) {
          const float angSpeed = std::sqrt(angSpeedSq);
          bodyA->angularVelocity = engine::math::mul(
              bodyA->angularVelocity, kMaxAngularSpeed / angSpeed);
        }
      }
    }
    if ((bodyB != nullptr) && (invMassB > 0.0F)) {
      bodyB->velocity = engine::math::add(
          bodyB->velocity,
          engine::math::mul(normal, impulseMagnitude * invMassB));
      if ((bodyB->inverseInertia > 0.0F) && (invMassA > 0.0F)) {
        const engine::math::Vec3 angImpulse =
            engine::math::mul(engine::math::cross(contactOffsetB, impulseVec),
                              bodyB->inverseInertia);
        bodyB->angularVelocity =
            engine::math::add(bodyB->angularVelocity, angImpulse);
        const float angSpeedSq =
            engine::math::length_sq(bodyB->angularVelocity);
        if (angSpeedSq > (kMaxAngularSpeed * kMaxAngularSpeed)) {
          const float angSpeed = std::sqrt(angSpeedSq);
          bodyB->angularVelocity = engine::math::mul(
              bodyB->angularVelocity, kMaxAngularSpeed / angSpeed);
        }
      }
    }

    // Tangential friction impulse.
    const engine::math::Vec3 tangentVel =
        engine::math::sub(relVel, engine::math::mul(normal, relVelAlongNormal));
    const float tangentSpeedSq = engine::math::length_sq(tangentVel);
    if (tangentSpeedSq > 1e-12F) {
      const float tangentSpeed = std::sqrt(tangentSpeedSq);
      const engine::math::Vec3 tangent =
          engine::math::div(tangentVel, tangentSpeed);
      float frictionImpulse = -tangentSpeed / invMassSum;
      if (std::fabs(frictionImpulse) < impulseMagnitude * staticFric) {
        // Static friction: apply exact counter-impulse.
      } else {
        frictionImpulse =
            sign_or_positive(frictionImpulse) * impulseMagnitude * dynamicFric;
      }
      if ((bodyA != nullptr) && (invMassA > 0.0F)) {
        bodyA->velocity = engine::math::sub(
            bodyA->velocity,
            engine::math::mul(tangent, frictionImpulse * invMassA));
      }
      if ((bodyB != nullptr) && (invMassB > 0.0F)) {
        bodyB->velocity = engine::math::add(
            bodyB->velocity,
            engine::math::mul(tangent, frictionImpulse * invMassB));
      }
    }
  }
}

} // namespace

const ConvexHullData *get_hull_data_ptr(std::uint32_t entityIndex) noexcept {
  return find_hull_data(entityIndex);
}

bool step_physics(runtime::World &world, float deltaSeconds) noexcept {
  return step_physics_range(world, 0U, world.transform_count(), deltaSeconds);
}

bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept {
  const runtime::World::PhysicsContext &physicsCtx = world.physics_context();
  const runtime::Entity *entities = nullptr;
  const runtime::Transform *readTransforms = nullptr;
  runtime::Transform *writeTransforms = nullptr;

  if (!world.get_transform_update_range(startIndex, count, &entities,
                                        &readTransforms, &writeTransforms)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = entities[i];
    if (world.movement_authority(entity) ==
        runtime::MovementAuthority::Script) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    runtime::RigidBody *body = world.get_rigid_body_ptr(entity);
    runtime::Transform updated = readTransforms[i];
    const runtime::Collider *col = world.get_collider_ptr(entity);
    const bool lockRotation =
        (col != nullptr) && (col->shape == runtime::ColliderShape::AABB);

    // Capsules also lock rotation for now (upright-only capsule physics).
    const bool isCapsule =
        (col != nullptr) && (col->shape == runtime::ColliderShape::Capsule);

    // Skip sleeping bodies.
    if ((body != nullptr) && body->sleeping) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    if ((body != nullptr) && (body->inverseMass > 0.0F)) {
      const engine::math::Vec3 totalAccel =
          engine::math::add(body->acceleration, physicsCtx.gravity);
      body->velocity = engine::math::add(
          body->velocity, engine::math::mul(totalAccel, deltaSeconds));

      const engine::math::Vec3 displacement =
          engine::math::mul(body->velocity, deltaSeconds);

      // CCD: Bilateral advancement (Erwin Coumans GDC style).
      // If the body has a collider and is moving fast enough, sweep it
      // forward to find the earliest time-of-impact and clamp position.
      bool clamped = false;
      if (col != nullptr) {
        const CcdSweepResult ccdResult = bilateral_advance_ccd(
            world, entity, *body, *col, readTransforms[i], deltaSeconds);
        if (ccdResult.hit) {
          // Move to the safe position at time-of-impact.
          const float safeToi = std::max(0.0F, ccdResult.timeOfImpact - 0.01F);
          updated.position =
              engine::math::add(readTransforms[i].position,
                                engine::math::mul(displacement, safeToi));

          // Reflect velocity off the contact normal.
          const float vn =
              engine::math::dot(body->velocity, ccdResult.contactNormal);
          if (vn < 0.0F) {
            body->velocity = engine::math::sub(
                body->velocity,
                engine::math::mul(ccdResult.contactNormal, 2.0F * vn));
          }
          clamped = true;
        }
      }

      if (!clamped) {
        updated.position = engine::math::add(updated.position, displacement);
      }

      // Light angular damping keeps contact jitter from integrating into
      // runaway spins on resting contacts.
      const float angularDamping =
          std::exp(-kAngularDampingPerSecond * deltaSeconds);
      body->angularVelocity =
          engine::math::mul(body->angularVelocity, angularDamping);
      const float angSpeedSq = engine::math::length_sq(body->angularVelocity);
      if (angSpeedSq < 1e-6F) {
        body->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
      }

      // AABB and capsule colliders are axis-aligned in this physics backend.
      // Keep visual mesh rotation fixed so render and collision stay in sync.
      if (lockRotation || isCapsule) {
        body->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
      }
    }

    // Angular velocity integration (independent of linear mass).
    if ((body != nullptr) && (body->inverseInertia > 0.0F) && !lockRotation &&
        !isCapsule) {
      const float angSpeedSq = engine::math::length_sq(body->angularVelocity);
      if (angSpeedSq > 1e-12F) {
        const float angSpeed = std::sqrt(angSpeedSq);
        const float angle = angSpeed * deltaSeconds;
        const engine::math::Vec3 axis =
            engine::math::div(body->angularVelocity, angSpeed);
        const engine::math::Quat deltaRot =
            engine::math::from_axis_angle(axis, angle);
        updated.rotation = engine::math::normalize(
            engine::math::mul(deltaRot, updated.rotation));
      }
    }

    writeTransforms[i] = updated;
  }

  return true;
}

bool resolve_collisions(runtime::World &world) noexcept {
  const auto simToken = world.simulation_access_token();
  runtime::World::PhysicsContext &physicsCtx = world.physics_context();
  physicsCtx.collisionPairCount = 0U;
  begin_generation(&physicsCtx.pairHashGeneration,
                   physicsCtx.pairHashStamps.data(),
                   physicsCtx.pairHashStamps.size());

  const std::size_t colliderCount = world.collider_count();

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if ((colliderCount > 0U) &&
      !world.get_collider_range(0U, colliderCount, &entities, &colliders)) {
    return false;
  }

  if (colliderCount >= 2U) {

    // ---- Broadphase: spatial hash grid
    // ----------------------------------------

    // Per-collider cached position.
    thread_local static std::array<float, runtime::World::kMaxColliders> posX{},
        posY{}, posZ{};

    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const runtime::Transform *t =
          world.get_transform_write_ptr(entities[i], simToken);
      if (t != nullptr) {
        posX[i] = t->position.x;
        posY[i] = t->position.y;
        posZ[i] = t->position.z;
      } else {
        posX[i] = 0.0F;
        posY[i] = 0.0F;
        posZ[i] = 0.0F;
      }
    }

    // Compute cell size: max of kDefaultCellSize and 2× largest half-extent.
    float cellSize = kDefaultCellSize;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const engine::math::Vec3 he = broadphase_half_extents(colliders[i]);
      const float maxHe = std::max({he.x, he.y, he.z});
      if (maxHe * 2.0F > cellSize) {
        cellSize = maxHe * 2.0F;
      }
    }
    const float invCellSize = 1.0F / cellSize;

    // Spatial hash: bucket heads + linked-list nodes.
    struct SpatialNode {
      std::uint32_t colliderIdx;
      std::uint32_t next;
    };

    // Max entries: each collider may touch up to 8 cells (corners of its AABB).
    constexpr std::size_t kMaxNodes = runtime::World::kMaxColliders * 8U;
    thread_local static std::array<std::uint32_t, kSpatialHashBuckets>
        buckets{};
    thread_local static std::array<SpatialNode, kMaxNodes> nodes{};
    std::size_t nodeCount = 0U;

    for (std::size_t b = 0U; b < kSpatialHashBuckets; ++b) {
      buckets[b] = kSpatialHashEmpty;
    }

    auto cell_coord = [invCellSize](float v) noexcept -> std::int32_t {
      return static_cast<std::int32_t>(std::floor(v * invCellSize));
    };

    auto hash_cell = [](std::int32_t cx, std::int32_t cy,
                        std::int32_t cz) noexcept -> std::uint32_t {
      auto u = static_cast<std::uint32_t>(cx) * 73856093U ^
               static_cast<std::uint32_t>(cy) * 19349663U ^
               static_cast<std::uint32_t>(cz) * 83492791U;
      return u % static_cast<std::uint32_t>(kSpatialHashBuckets);
    };

    auto insert_node = [&](std::uint32_t bucket,
                           std::uint32_t colIdx) noexcept {
      if (nodeCount >= kMaxNodes) {
        return;
      }
      nodes[nodeCount] = {colIdx, buckets[bucket]};
      buckets[bucket] = static_cast<std::uint32_t>(nodeCount);
      ++nodeCount;
    };

    // Insert each collider into all cells its AABB overlaps.
    // For speculative contacts: expand AABB by velocity * dt (1/60s assumed).
    constexpr float kSpeculativeDt = 1.0F / 60.0F;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const engine::math::Vec3 he = broadphase_half_extents(colliders[i]);

      // Expand AABB by velocity to detect speculative contacts.
      const runtime::RigidBody *bodyI = world.get_rigid_body_ptr(entities[i]);
      float expandX = 0.0F;
      float expandY = 0.0F;
      float expandZ = 0.0F;
      if ((bodyI != nullptr) && (bodyI->inverseMass > 0.0F)) {
        expandX = std::fabs(bodyI->velocity.x) * kSpeculativeDt;
        expandY = std::fabs(bodyI->velocity.y) * kSpeculativeDt;
        expandZ = std::fabs(bodyI->velocity.z) * kSpeculativeDt;
      }

      const std::int32_t minCX = cell_coord(posX[i] - he.x - expandX);
      const std::int32_t maxCX = cell_coord(posX[i] + he.x + expandX);
      const std::int32_t minCY = cell_coord(posY[i] - he.y - expandY);
      const std::int32_t maxCY = cell_coord(posY[i] + he.y + expandY);
      const std::int32_t minCZ = cell_coord(posZ[i] - he.z - expandZ);
      const std::int32_t maxCZ = cell_coord(posZ[i] + he.z + expandZ);
      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            insert_node(hash_cell(cx, cy, cz), static_cast<std::uint32_t>(i));
          }
        }
      }
    }

    for (std::size_t i = 0U; i < colliderCount; ++i) {
      const runtime::Entity entityA = entities[i];
      if (world.movement_authority(entityA) ==
          runtime::MovementAuthority::Script) {
        continue;
      }

      const runtime::Transform *transformA =
          world.get_transform_write_ptr(entityA, simToken);
      if (transformA == nullptr) {
        continue;
      }

      const float ax = posX[i];
      const float ay = posY[i];
      const float az = posZ[i];

      begin_generation(&physicsCtx.testedGeneration,
                       physicsCtx.testedStamps.data(),
                       physicsCtx.testedStamps.size());
      physicsCtx.testedStamps[i] = physicsCtx.testedGeneration;

      const engine::math::Vec3 heA = broadphase_half_extents(colliders[i]);
      const std::int32_t minCX = cell_coord(ax - heA.x);
      const std::int32_t maxCX = cell_coord(ax + heA.x);
      const std::int32_t minCY = cell_coord(ay - heA.y);
      const std::int32_t maxCY = cell_coord(ay + heA.y);
      const std::int32_t minCZ = cell_coord(az - heA.z);
      const std::int32_t maxCZ = cell_coord(az + heA.z);

      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            const std::uint32_t bucket = hash_cell(cx, cy, cz);
            std::uint32_t nodeIdx = buckets[bucket];
            while (nodeIdx != kSpatialHashEmpty) {
              const std::uint32_t j = nodes[nodeIdx].colliderIdx;
              nodeIdx = nodes[nodeIdx].next;

              if (physicsCtx.testedStamps[j] == physicsCtx.testedGeneration) {
                continue;
              }
              physicsCtx.testedStamps[j] = physicsCtx.testedGeneration;

              // Only process pair (i, j) where i < j to avoid
              // double-processing.
              if (j <= i) {
                continue;
              }

              const runtime::Entity entityB = entities[j];
              if (world.movement_authority(entityB) ==
                  runtime::MovementAuthority::Script) {
                continue;
              }

              const runtime::Transform *transformB =
                  world.get_transform_write_ptr(entityB, simToken);
              if (transformB == nullptr) {
                continue;
              }

              const float bx = posX[j];
              const float by = posY[j];
              const float bz = posZ[j];

              const runtime::Collider &colliderA = colliders[i];
              const runtime::Collider &colliderB = colliders[j];

              // Collision layer/mask filtering (P1-M3-C2b).
              if (((colliderA.collisionLayer & colliderB.collisionMask) ==
                   0U) ||
                  ((colliderB.collisionLayer & colliderA.collisionMask) ==
                   0U)) {
                continue;
              }

              runtime::RigidBody *bodyA = world.get_rigid_body_ptr(entityA);
              runtime::RigidBody *bodyB = world.get_rigid_body_ptr(entityB);
              const float invMassA =
                  (bodyA != nullptr) ? bodyA->inverseMass : kStaticInverseMass;
              const float invMassB =
                  (bodyB != nullptr) ? bodyB->inverseMass : kStaticInverseMass;
              const float invMassSum = invMassA + invMassB;

              const auto shapeA = colliderA.shape;
              const auto shapeB = colliderB.shape;
              const bool aIsAABB = (shapeA == runtime::ColliderShape::AABB);
              const bool bIsAABB = (shapeB == runtime::ColliderShape::AABB);
              const bool aIsCapsule =
                  (shapeA == runtime::ColliderShape::Capsule);
              const bool bIsCapsule =
                  (shapeB == runtime::ColliderShape::Capsule);
              const bool aIsSphere = (shapeA == runtime::ColliderShape::Sphere);
              const bool bIsSphere = (shapeB == runtime::ColliderShape::Sphere);
              const bool aIsConvex =
                  (shapeA == runtime::ColliderShape::ConvexHull);
              const bool bIsConvex =
                  (shapeB == runtime::ColliderShape::ConvexHull);
              const bool aIsHeightfield =
                  (shapeA == runtime::ColliderShape::Heightfield);
              const bool bIsHeightfield =
                  (shapeB == runtime::ColliderShape::Heightfield);

              // -----------------------------------------------------------------------
              // Heightfield vs Sphere/AABB/Capsule
              // -----------------------------------------------------------------------
              if (aIsHeightfield || bIsHeightfield) {
                const bool aIsHF = aIsHeightfield;
                const engine::math::Vec3 hfPos =
                    aIsHF ? engine::math::Vec3(ax, ay, az)
                          : engine::math::Vec3(bx, by, bz);
                const engine::math::Vec3 objPos =
                    aIsHF ? engine::math::Vec3(bx, by, bz)
                          : engine::math::Vec3(ax, ay, az);
                const runtime::Entity hfEnt = aIsHF ? entityA : entityB;
                const runtime::Collider &objCol = aIsHF ? colliderB : colliderA;

                const HeightfieldData *hf = find_heightfield_data(hfEnt.index);
                if (hf == nullptr) {
                  continue;
                }

                // Compute object radius for footprint.
                float objRadius = 0.0F;
                if (objCol.shape == runtime::ColliderShape::Sphere) {
                  objRadius = objCol.halfExtents.x;
                } else if (objCol.shape == runtime::ColliderShape::Capsule) {
                  objRadius = objCol.halfExtents.y + objCol.halfExtents.x;
                } else {
                  objRadius = engine::math::length(objCol.halfExtents);
                }

                // Map object footprint to grid cell range.
                float gColMin = 0.0F;
                float gRowMin = 0.0F;
                float gColMax = 0.0F;
                float gRowMax = 0.0F;
                heightfield_world_to_grid(*hf, hfPos,
                                          objPos.x - objRadius,
                                          objPos.z - objRadius,
                                          gColMin, gRowMin);
                heightfield_world_to_grid(*hf, hfPos,
                                          objPos.x + objRadius,
                                          objPos.z + objRadius,
                                          gColMax, gRowMax);

                const auto cMin = static_cast<std::size_t>(
                    std::max(0.0F, std::floor(gColMin)));
                const auto rMin = static_cast<std::size_t>(
                    std::max(0.0F, std::floor(gRowMin)));
                const auto cMax = static_cast<std::size_t>(std::min(
                    static_cast<float>(hf->columns - 2U), std::floor(gColMax)));
                const auto rMax = static_cast<std::size_t>(std::min(
                    static_cast<float>(hf->rows - 2U), std::floor(gRowMax)));

                // Per-triangle sweep: track deepest penetration.
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

                    engine::math::Vec3 triVerts[2][3] = {
                        {v00, v10, v01}, {v10, v11, v01}};

                    for (int ti = 0; ti < 2; ++ti) {
                      // Upward-facing triangle face normal.
                      const engine::math::Vec3 e1 = engine::math::sub(
                          triVerts[ti][1], triVerts[ti][0]);
                      const engine::math::Vec3 e2 = engine::math::sub(
                          triVerts[ti][2], triVerts[ti][0]);
                      engine::math::Vec3 faceN =
                          engine::math::cross(e1, e2);
                      const float faceLen = engine::math::length(faceN);
                      if (faceLen < 1e-10F) {
                        continue; // degenerate triangle
                      }
                      faceN = engine::math::mul(faceN, 1.0F / faceLen);
                      if (faceN.y < 0.0F) {
                        faceN = engine::math::mul(faceN, -1.0F);
                      }

                      float tOverlap = 0.0F;

                      if (objCol.shape == runtime::ColliderShape::Sphere) {
                        const engine::math::Vec3 cp =
                            closest_point_on_triangle(
                                objPos, triVerts[ti][0], triVerts[ti][1],
                                triVerts[ti][2]);
                        const engine::math::Vec3 diff =
                            engine::math::sub(objPos, cp);
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
                      } else if (objCol.shape ==
                                 runtime::ColliderShape::Capsule) {
                        const float halfH = objCol.halfExtents.y;
                        const float capR = objCol.halfExtents.x;
                        const engine::math::Vec3 top(objPos.x,
                                                     objPos.y + halfH,
                                                     objPos.z);
                        const engine::math::Vec3 bot(objPos.x,
                                                     objPos.y - halfH,
                                                     objPos.z);
                        engine::math::Vec3 cpTri =
                            closest_point_on_triangle(
                                objPos, triVerts[ti][0], triVerts[ti][1],
                                triVerts[ti][2]);
                        engine::math::Vec3 cpSeg{};
                        closest_point_on_segment(bot, top, cpTri, cpSeg);
                        const engine::math::Vec3 cpTri2 =
                            closest_point_on_triangle(
                                cpSeg, triVerts[ti][0], triVerts[ti][1],
                                triVerts[ti][2]);
                        const engine::math::Vec3 diff2 =
                            engine::math::sub(cpSeg, cpTri2);
                        if (engine::math::dot(diff2, diff2) >=
                            capR * capR) {
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
                        const float effR =
                            std::abs(faceN.x) * objCol.halfExtents.x +
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
                        bestContact =
                            closest_point_on_triangle(objPos, triVerts[ti][0],
                                                      triVerts[ti][1],
                                                      triVerts[ti][2]);
                        anyContact = true;
                      }
                    }
                  }
                }

                if (!anyContact) {
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  maybe_wake_pair(bodyA, bodyB, vA2, vB2);
                }
                if (invMassSum <= 0.0F) {
                  continue;
                }

                engine::math::Vec3 resolveNormal = bestNormal;
                if (!aIsHF) {
                  resolveNormal = engine::math::mul(bestNormal, -1.0F);
                }

                resolve_contact(world, simToken, entityA, entityB, bodyA, bodyB,
                                invMassA, invMassB, invMassSum, resolveNormal,
                                bestOverlap, bestContact, colliderA, colliderB);
                continue;
              }
// -----------------------------------------------------------------------
              // ConvexHull vs anything (GJK/EPA generic path)
              // -----------------------------------------------------------------------
              if (aIsConvex || bIsConvex) {
                // Build support function and data for each side.
                auto shape_support = [](const runtime::Collider &col,
                                        runtime::Entity /*ent*/) -> SupportFn {
                  switch (col.shape) {
                  case runtime::ColliderShape::ConvexHull:
                    return &support_convex_hull;
                  case runtime::ColliderShape::Sphere:
                    return &support_sphere;
                  case runtime::ColliderShape::Capsule:
                    return &support_capsule;
                  case runtime::ColliderShape::AABB:
                  default:
                    return &support_aabb;
                  }
                };

                // Opaque data for each shape's support function.
                // Capsule: float[2]{radius, halfHeight}
                // Sphere: float (radius)
                // AABB: Vec3 (halfExtents)
                // ConvexHull: ConvexHullData*
                alignas(16) float dataStorageA[4]{};
                alignas(16) float dataStorageB[4]{};
                const void *dataA = nullptr;
                const void *dataB = nullptr;

                auto fill_data = [](const runtime::Collider &col,
                                    runtime::Entity ent,
                                    float *storage) -> const void * {
                  switch (col.shape) {
                  case runtime::ColliderShape::ConvexHull:
                    return find_hull_data(ent.index);
                  case runtime::ColliderShape::Sphere:
                    storage[0] = col.halfExtents.x;
                    return storage;
                  case runtime::ColliderShape::Capsule:
                    storage[0] = col.halfExtents.x; // radius
                    storage[1] = col.halfExtents.y; // halfHeight
                    return storage;
                  case runtime::ColliderShape::AABB:
                  default:
                    std::memcpy(storage, &col.halfExtents, sizeof(float) * 3);
                    return storage;
                  }
                };

                SupportFn supA = shape_support(colliderA, entityA);
                SupportFn supB = shape_support(colliderB, entityB);
                dataA = fill_data(colliderA, entityA, dataStorageA);
                dataB = fill_data(colliderB, entityB, dataStorageB);

                if ((dataA == nullptr) || (dataB == nullptr)) {
                  continue;
                }

                const engine::math::Vec3 posA(ax, ay, az);
                const engine::math::Vec3 posB(bx, by, bz);
                const GjkResult gjk =
                    gjk_epa(dataA, posA, supA, dataB, posB, supB);

                if (!gjk.intersecting || gjk.depth < 1e-6F) {
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  maybe_wake_pair(bodyA, bodyB, vA2, vB2);
                }
                if (invMassSum <= 0.0F) {
                  continue;
                }

                resolve_contact(world, simToken, entityA, entityB, bodyA, bodyB,
                                invMassA, invMassB, invMassSum, gjk.normal,
                                gjk.depth, gjk.contactPoint, colliderA,
                                colliderB);
                continue;
              }

              // -----------------------------------------------------------------------
              // Capsule vs Capsule
              // -----------------------------------------------------------------------
              if (aIsCapsule && bIsCapsule) {
                engine::math::Vec3 segAa, segAb, segBa, segBb;
                capsule_segment(engine::math::Vec3(ax, ay, az), colliderA,
                                segAa, segAb);
                capsule_segment(engine::math::Vec3(bx, by, bz), colliderB,
                                segBa, segBb);
                engine::math::Vec3 closestA, closestB;
                const float dist2 = closest_point_segment_segment(
                    segAa, segAb, segBa, segBb, closestA, closestB);
                const float rA = colliderA.halfExtents.x;
                const float rB = colliderB.halfExtents.x;
                const float sumR = rA + rB;
                if (dist2 >= sumR * sumR) {
                  continue;
                }
                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  maybe_wake_pair(bodyA, bodyB, vA2, vB2);
                }
                if (invMassSum <= 0.0F) {
                  continue;
                }
                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                const engine::math::Vec3 diff =
                    engine::math::sub(closestB, closestA);
                const engine::math::Vec3 normal =
                    (dist2 > 0.0F) ? engine::math::mul(diff, 1.0F / dist)
                                   : engine::math::Vec3(0.0F, 1.0F, 0.0F);
                const float overlap = sumR - dist;
                const engine::math::Vec3 contactPt = engine::math::mul(
                    engine::math::add(closestA, closestB), 0.5F);
                resolve_contact(world, simToken, entityA, entityB, bodyA, bodyB,
                                invMassA, invMassB, invMassSum, normal, overlap,
                                contactPt, colliderA, colliderB);
                continue;
              }

              // -----------------------------------------------------------------------
              // Capsule vs Sphere (handles both orderings)
              // -----------------------------------------------------------------------
              if ((aIsCapsule && bIsSphere) || (aIsSphere && bIsCapsule)) {
                const bool aIsCap = aIsCapsule;
                const engine::math::Vec3 capPos =
                    aIsCap ? engine::math::Vec3(ax, ay, az)
                           : engine::math::Vec3(bx, by, bz);
                const engine::math::Vec3 sphPos =
                    aIsCap ? engine::math::Vec3(bx, by, bz)
                           : engine::math::Vec3(ax, ay, az);
                const runtime::Collider &capCol =
                    aIsCap ? colliderA : colliderB;
                const float capR = capCol.halfExtents.x;
                const float sphR =
                    (aIsCap ? colliderB : colliderA).halfExtents.x;
                engine::math::Vec3 segA, segB;
                capsule_segment(capPos, capCol, segA, segB);
                engine::math::Vec3 closest;
                closest_point_on_segment(segA, segB, sphPos, closest);
                const engine::math::Vec3 diff =
                    engine::math::sub(sphPos, closest);
                const float dist2 = engine::math::dot(diff, diff);
                const float sumR = capR + sphR;
                if (dist2 >= sumR * sumR) {
                  continue;
                }
                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  maybe_wake_pair(bodyA, bodyB, vA2, vB2);
                }
                if (invMassSum <= 0.0F) {
                  continue;
                }
                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                // Normal points from capsule toward sphere.
                engine::math::Vec3 normal =
                    (dist2 > 0.0F) ? engine::math::mul(diff, 1.0F / dist)
                                   : engine::math::Vec3(0.0F, 1.0F, 0.0F);
                // Ensure normal points from A toward B.
                if (!aIsCap) {
                  normal = engine::math::mul(normal, -1.0F);
                }
                const float overlap = sumR - dist;
                const engine::math::Vec3 contactPt =
                    engine::math::mul(engine::math::add(closest, sphPos), 0.5F);
                resolve_contact(world, simToken, entityA, entityB, bodyA, bodyB,
                                invMassA, invMassB, invMassSum, normal, overlap,
                                contactPt, colliderA, colliderB);
                continue;
              }

              // -----------------------------------------------------------------------
              // Capsule vs AABB (handles both orderings)
              // -----------------------------------------------------------------------
              if ((aIsCapsule && bIsAABB) || (aIsAABB && bIsCapsule)) {
                const bool aIsCap = aIsCapsule;
                const engine::math::Vec3 capPos =
                    aIsCap ? engine::math::Vec3(ax, ay, az)
                           : engine::math::Vec3(bx, by, bz);
                const engine::math::Vec3 boxPos =
                    aIsCap ? engine::math::Vec3(bx, by, bz)
                           : engine::math::Vec3(ax, ay, az);
                const runtime::Collider &capCol =
                    aIsCap ? colliderA : colliderB;
                const runtime::Collider &boxCol =
                    aIsCap ? colliderB : colliderA;
                const float capR = capCol.halfExtents.x;

                engine::math::Vec3 segA, segB;
                capsule_segment(capPos, capCol, segA, segB);

                // Find closest point on capsule segment to the AABB.
                // Strategy: clamp each segment endpoint to AABB, then also
                // find the closest point on segment to the AABB center and
                // clamp that.  Take the pair with smallest distance.
                const engine::math::Vec3 cpA =
                    closest_point_on_aabb(segA, boxPos, boxCol.halfExtents);
                const engine::math::Vec3 cpB =
                    closest_point_on_aabb(segB, boxPos, boxCol.halfExtents);

                engine::math::Vec3 segClosest;
                closest_point_on_segment(segA, segB, boxPos, segClosest);
                const engine::math::Vec3 cpC = closest_point_on_aabb(
                    segClosest, boxPos, boxCol.halfExtents);

                // Evaluate distances from each candidate to its segment point.
                auto seg_dist2 = [](const engine::math::Vec3 &segPt,
                                    const engine::math::Vec3 &aabbPt) {
                  const engine::math::Vec3 d = engine::math::sub(segPt, aabbPt);
                  return engine::math::dot(d, d);
                };
                const float d2A = seg_dist2(segA, cpA);
                const float d2B = seg_dist2(segB, cpB);
                const float d2C = seg_dist2(segClosest, cpC);

                // Pick the candidate with the smallest distance.
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

                // Now find closest point on the segment to the best AABB
                // point, for a tighter fit.
                engine::math::Vec3 finalSeg;
                closest_point_on_segment(segA, segB, bestBox, finalSeg);
                const engine::math::Vec3 finalBox =
                    closest_point_on_aabb(finalSeg, boxPos, boxCol.halfExtents);
                const engine::math::Vec3 diff =
                    engine::math::sub(finalSeg, finalBox);
                const float dist2 = engine::math::dot(diff, diff);

                if (dist2 >= capR * capR) {
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  maybe_wake_pair(bodyA, bodyB, vA2, vB2);
                }
                if (invMassSum <= 0.0F) {
                  continue;
                }

                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                // Normal points from box toward capsule segment.
                engine::math::Vec3 normal =
                    (dist2 > 0.0F) ? engine::math::mul(diff, 1.0F / dist)
                                   : engine::math::Vec3(0.0F, 1.0F, 0.0F);
                // Ensure normal points from A to B.
                if (!aIsCap) {
                  normal = engine::math::mul(normal, -1.0F);
                }
                const float overlap = capR - dist;
                const engine::math::Vec3 contactPt = engine::math::mul(
                    engine::math::add(finalSeg, finalBox), 0.5F);
                resolve_contact(world, simToken, entityA, entityB, bodyA, bodyB,
                                invMassA, invMassB, invMassSum, normal, overlap,
                                contactPt, colliderA, colliderB);
                continue;
              }

              // -----------------------------------------------------------------------
              // Sphere vs Sphere
              // -----------------------------------------------------------------------
              if (aIsSphere && bIsSphere) {
                const float rA = colliderA.halfExtents.x;
                const float rB = colliderB.halfExtents.x;
                const float dx = bx - ax;
                const float dy = by - ay;
                const float dz = bz - az;
                const float dist2 = dx * dx + dy * dy + dz * dz;
                const float sumR = rA + rB;
                if (dist2 >= sumR * sumR) {
                  // Speculative contact for spheres: if gap is small,
                  // apply clamped impulse.
                  const float dist =
                      (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                  const float gap = dist - sumR;
                  if ((gap > 0.0F) && (gap < kSpeculativeDt * 300.0F)) {
                    const engine::math::Vec3 specN =
                        (dist2 > 0.0F) ? engine::math::Vec3(
                                             dx / dist, dy / dist, dz / dist)
                                       : engine::math::Vec3(0.0F, 1.0F, 0.0F);
                    resolve_speculative_contact(bodyA, bodyB, specN, invMassA,
                                                invMassB, invMassSum, gap,
                                                kSpeculativeDt);
                  }
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  if ((bodyA != nullptr) && bodyA->sleeping &&
                      (vB2 > kSleepThreshold)) {
                    bodyA->sleeping = false;
                    bodyA->sleepFrameCount = 0U;
                  }
                  if ((bodyB != nullptr) && bodyB->sleeping &&
                      (vA2 > kSleepThreshold)) {
                    bodyB->sleeping = false;
                    bodyB->sleepFrameCount = 0U;
                  }
                }

                if (invMassSum <= 0.0F) {
                  continue;
                }

                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                const float nx = dx / dist;
                const float ny = dy / dist;
                const float nz = dz / dist;
                const float overlap = sumR - dist;
                const float moveA = overlap * (invMassA / invMassSum);
                const float moveB = overlap * (invMassB / invMassSum);

                runtime::Transform *mutableA =
                    world.get_transform_write_ptr(entityA, simToken);
                runtime::Transform *mutableB =
                    world.get_transform_write_ptr(entityB, simToken);
                if ((mutableA == nullptr) || (mutableB == nullptr)) {
                  continue;
                }
                mutableA->position.x -= nx * moveA;
                mutableA->position.y -= ny * moveA;
                mutableA->position.z -= nz * moveA;
                mutableB->position.x += nx * moveB;
                mutableB->position.y += ny * moveB;
                mutableB->position.z += nz * moveB;

                const engine::math::Vec3 contactNormal(nx, ny, nz);
                const engine::math::Vec3 contactPt = engine::math::mul(
                    engine::math::add(mutableA->position, mutableB->position),
                    0.5F);
                const float combinedRest =
                    std::max(colliderA.restitution, colliderB.restitution);
                const float combinedStaticFric = std::sqrt(
                    colliderA.staticFriction * colliderB.staticFriction);
                const float combinedDynFric = std::sqrt(
                    colliderA.dynamicFriction * colliderB.dynamicFriction);
                apply_velocity_impulse(
                    bodyA, bodyB, contactNormal, invMassA, invMassB, invMassSum,
                    engine::math::sub(contactPt, mutableA->position),
                    engine::math::sub(contactPt, mutableB->position),
                    combinedRest, combinedStaticFric, combinedDynFric);
                continue;
              }

              // -----------------------------------------------------------------------
              // AABB vs Sphere (handles both orderings)
              // -----------------------------------------------------------------------
              if (aIsAABB != bIsAABB) {
                const bool aIsBox = aIsAABB;
                const float boxX = aIsBox ? ax : bx;
                const float boxY = aIsBox ? ay : by;
                const float boxZ = aIsBox ? az : bz;
                const float sphX = aIsBox ? bx : ax;
                const float sphY = aIsBox ? by : ay;
                const float sphZ = aIsBox ? bz : az;
                const runtime::Collider &boxCol =
                    aIsBox ? colliderA : colliderB;
                const float radius =
                    (aIsBox ? colliderB : colliderA).halfExtents.x;

                const float cpx =
                    std::max(boxX - boxCol.halfExtents.x,
                             std::min(sphX, boxX + boxCol.halfExtents.x));
                const float cpy =
                    std::max(boxY - boxCol.halfExtents.y,
                             std::min(sphY, boxY + boxCol.halfExtents.y));
                const float cpz =
                    std::max(boxZ - boxCol.halfExtents.z,
                             std::min(sphZ, boxZ + boxCol.halfExtents.z));

                const float dx = sphX - cpx;
                const float dy = sphY - cpy;
                const float dz = sphZ - cpz;
                const float dist2 = dx * dx + dy * dy + dz * dz;
                if (dist2 >= radius * radius) {
                  // Speculative contact: AABB vs Sphere.
                  const float dist =
                      (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                  const float gap = dist - radius;
                  if ((gap > 0.0F) && (gap < kSpeculativeDt * 300.0F)) {
                    engine::math::Vec3 specN =
                        (dist2 > 0.0F) ? engine::math::Vec3(
                                             dx / dist, dy / dist, dz / dist)
                                       : engine::math::Vec3(0.0F, 1.0F, 0.0F);
                    if (!aIsBox) {
                      specN = engine::math::mul(specN, -1.0F);
                    }
                    resolve_speculative_contact(bodyA, bodyB, specN, invMassA,
                                                invMassB, invMassSum, gap,
                                                kSpeculativeDt);
                  }
                  continue;
                }

                record_collision_pair(world, entityA.index, entityB.index);
                {
                  const float vA2 =
                      (bodyA != nullptr)
                          ? engine::math::length_sq(bodyA->velocity)
                          : 0.0F;
                  const float vB2 =
                      (bodyB != nullptr)
                          ? engine::math::length_sq(bodyB->velocity)
                          : 0.0F;
                  if ((bodyA != nullptr) && bodyA->sleeping &&
                      (vB2 > kSleepThreshold)) {
                    bodyA->sleeping = false;
                    bodyA->sleepFrameCount = 0U;
                  }
                  if ((bodyB != nullptr) && bodyB->sleeping &&
                      (vA2 > kSleepThreshold)) {
                    bodyB->sleeping = false;
                    bodyB->sleepFrameCount = 0U;
                  }
                }

                if (invMassSum <= 0.0F) {
                  continue;
                }

                const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
                float nx = (dist2 > 0.0F) ? (dx / dist) : 0.0F;
                float ny = (dist2 > 0.0F) ? (dy / dist) : 1.0F;
                float nz = (dist2 > 0.0F) ? (dz / dist) : 0.0F;
                const float overlap = radius - dist;

                // Normal points from box toward sphere; flip if A is the
                // sphere.
                if (!aIsBox) {
                  nx = -nx;
                  ny = -ny;
                  nz = -nz;
                }

                const float moveA = overlap * (invMassA / invMassSum);
                const float moveB = overlap * (invMassB / invMassSum);

                runtime::Transform *mutableA =
                    world.get_transform_write_ptr(entityA, simToken);
                runtime::Transform *mutableB =
                    world.get_transform_write_ptr(entityB, simToken);
                if ((mutableA == nullptr) || (mutableB == nullptr)) {
                  continue;
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
                    std::max(colliderA.restitution, colliderB.restitution);
                const float combinedStaticFric = std::sqrt(
                    colliderA.staticFriction * colliderB.staticFriction);
                const float combinedDynFric = std::sqrt(
                    colliderA.dynamicFriction * colliderB.dynamicFriction);
                apply_velocity_impulse(
                    bodyA, bodyB, aabbSphNormal, invMassA, invMassB, invMassSum,
                    engine::math::sub(closestPt, mutableA->position),
                    engine::math::sub(closestPt, mutableB->position),
                    combinedRest, combinedStaticFric, combinedDynFric);
                continue;
              }

              // -----------------------------------------------------------------------
              // AABB vs AABB
              // -----------------------------------------------------------------------
              const float overlapX = axis_overlap(
                  ax - colliderA.halfExtents.x, ax + colliderA.halfExtents.x,
                  bx - colliderB.halfExtents.x, bx + colliderB.halfExtents.x);
              const float overlapY = axis_overlap(
                  ay - colliderA.halfExtents.y, ay + colliderA.halfExtents.y,
                  by - colliderB.halfExtents.y, by + colliderB.halfExtents.y);
              const float overlapZ = axis_overlap(
                  az - colliderA.halfExtents.z, az + colliderA.halfExtents.z,
                  bz - colliderB.halfExtents.z, bz + colliderB.halfExtents.z);

              const bool hasOverlap =
                  (overlapX > 0.0F) && (overlapY > 0.0F) && (overlapZ > 0.0F);

              // Speculative contacts (E2a): if AABB pair is NOT overlapping but
              // the gap is small enough that approach velocity could close it
              // in one frame, apply a speculative impulse to prevent
              // penetration.
              if (!hasOverlap) {
                // Minimum overlap (most negative = largest gap on that axis).
                const float minOverlap =
                    std::min({overlapX, overlapY, overlapZ});
                const float gap = -minOverlap; // positive = actual gap distance

                if ((gap > 0.0F) && (gap < kSpeculativeDt * 300.0F)) {
                  // Determine the speculative contact normal (axis of smallest
                  // gap).
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
                  resolve_speculative_contact(bodyA, bodyB, specNormal,
                                              invMassA, invMassB, invMassSum,
                                              gap, kSpeculativeDt);
                }
                continue;
              }

              record_collision_pair(world, entityA.index, entityB.index);
              {
                const float vA2 = (bodyA != nullptr)
                                      ? engine::math::length_sq(bodyA->velocity)
                                      : 0.0F;
                const float vB2 = (bodyB != nullptr)
                                      ? engine::math::length_sq(bodyB->velocity)
                                      : 0.0F;
                if ((bodyA != nullptr) && bodyA->sleeping &&
                    (vB2 > kSleepThreshold)) {
                  bodyA->sleeping = false;
                  bodyA->sleepFrameCount = 0U;
                }
                if ((bodyB != nullptr) && bodyB->sleeping &&
                    (vA2 > kSleepThreshold)) {
                  bodyB->sleeping = false;
                  bodyB->sleepFrameCount = 0U;
                }
              }

              if (invMassSum <= 0.0F) {
                continue;
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

              const float moveA = pushAmount * (invMassA / invMassSum);
              const float moveB = pushAmount * (invMassB / invMassSum);

              runtime::Transform *mutableA =
                  world.get_transform_write_ptr(entityA, simToken);
              runtime::Transform *mutableB =
                  world.get_transform_write_ptr(entityB, simToken);
              if ((mutableA == nullptr) || (mutableB == nullptr)) {
                continue;
              }

              mutableA->position.x -= pushX * moveA;
              mutableA->position.y -= pushY * moveA;
              mutableA->position.z -= pushZ * moveA;
              mutableB->position.x += pushX * moveB;
              mutableB->position.y += pushY * moveB;
              mutableB->position.z += pushZ * moveB;

              const engine::math::Vec3 aabbNormal(pushX, pushY, pushZ);
              const engine::math::Vec3 midPt = engine::math::mul(
                  engine::math::add(mutableA->position, mutableB->position),
                  0.5F);
              const float combinedRest =
                  std::max(colliderA.restitution, colliderB.restitution);
              const float combinedStaticFric = std::sqrt(
                  colliderA.staticFriction * colliderB.staticFriction);
              const float combinedDynFric = std::sqrt(
                  colliderA.dynamicFriction * colliderB.dynamicFriction);
              apply_velocity_impulse(
                  bodyA, bodyB, aabbNormal, invMassA, invMassB, invMassSum,
                  engine::math::sub(midPt, mutableA->position),
                  engine::math::sub(midPt, mutableB->position), combinedRest,
                  combinedStaticFric, combinedDynFric);

            } // while nodeIdx
          } // for cz
        } // for cy
      } // for cx
    } // for i
  } // if (colliderCount >= 2U)

  solve_constraints(world, 1.0F / 60.0F);

  // Sleep check: after all collision responses and joint solving,
  // examine each rigid body. If velocity is below threshold for enough
  // consecutive frames, put it to sleep.
  for (std::size_t i = 0U; i < colliderCount; ++i) {
    runtime::RigidBody *body = world.get_rigid_body_ptr(entities[i]);
    if ((body == nullptr) || (body->inverseMass <= 0.0F) || body->sleeping) {
      continue;
    }
    const float energy = engine::math::length_sq(body->velocity) +
                         engine::math::length_sq(body->angularVelocity);
    if (energy < kSleepThreshold) {
      if (body->sleepFrameCount >= kSleepFramesRequired) {
        body->sleeping = true;
        body->velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
        body->angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
      } else {
        ++body->sleepFrameCount;
      }
    } else {
      body->sleepFrameCount = 0U;
    }
  }

  return true;
}

void set_gravity(runtime::World &world, float x, float y, float z) noexcept {
  world.physics_context().gravity = engine::math::Vec3(x, y, z);
}

engine::math::Vec3 get_gravity(const runtime::World &world) noexcept {
  return world.physics_context().gravity;
}

void set_collision_dispatch(runtime::World &world,
                            CollisionDispatchFn fn) noexcept {
  world.physics_context().collisionDispatch = fn;
}

void dispatch_collision_callbacks(runtime::World &world) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if ((ctx.collisionDispatch != nullptr) && (ctx.collisionPairCount > 0U)) {
    ctx.collisionDispatch(ctx.collisionPairData.data(), ctx.collisionPairCount);
  }
  ctx.collisionPairCount = 0U;
}

namespace {

engine::math::Vec3 aabb_hit_normal(const engine::math::Vec3 &hitPoint,
                                   const engine::math::Vec3 &center,
                                   const engine::math::Vec3 &halfExt) noexcept {
  const float dx = (hitPoint.x - center.x) / halfExt.x;
  const float dy = (hitPoint.y - center.y) / halfExt.y;
  const float dz = (hitPoint.z - center.z) / halfExt.z;
  const float ax = std::fabs(dx);
  const float ay = std::fabs(dy);
  const float az = std::fabs(dz);
  // If all deviations are zero (exact center hit), return zero normal.
  if ((ax < 1e-6F) && (ay < 1e-6F) && (az < 1e-6F)) {
    return engine::math::Vec3(0.0F, 0.0F, 0.0F);
  }
  if ((ax >= ay) && (ax >= az)) {
    return engine::math::Vec3(sign_or_positive(dx), 0.0F, 0.0F);
  }
  if (ay >= az) {
    return engine::math::Vec3(0.0F, sign_or_positive(dy), 0.0F);
  }
  return engine::math::Vec3(0.0F, 0.0F, sign_or_positive(dz));
}

// Ray-vs-capsule intersection.  The capsule is aligned along the Y axis,
// centred at `center` with halfHeight (center to hemisphere center) and
// radius.  Returns true if the ray hits within [0, maxDist], writing `outT`
// and `outNormal`.
bool ray_intersects_capsule(const engine::math::Ray &ray,
                            const engine::math::Vec3 &center, float halfHeight,
                            float radius, float maxDist, float &outT,
                            engine::math::Vec3 &outNormal) noexcept {
  // Translate ray into capsule-local space.
  const float ox = ray.origin.x - center.x;
  const float oy = ray.origin.y - center.y;
  const float oz = ray.origin.z - center.z;
  const float dx = ray.direction.x;
  const float dy = ray.direction.y;
  const float dz = ray.direction.z;

  float bestT = maxDist + 1.0F;
  engine::math::Vec3 bestN(0.0F, 1.0F, 0.0F);

  // 1) Infinite cylinder test (along Y): x^2 + z^2 = r^2.
  {
    const float a = dx * dx + dz * dz;
    const float b = 2.0F * (ox * dx + oz * dz);
    const float c = ox * ox + oz * oz - radius * radius;
    const float disc = b * b - 4.0F * a * c;
    if ((a > 1e-12F) && (disc >= 0.0F)) {
      const float sqrtDisc = std::sqrt(disc);
      const float inv2a = 1.0F / (2.0F * a);
      for (int sign = -1; sign <= 1; sign += 2) {
        const float t = (-b + static_cast<float>(sign) * sqrtDisc) * inv2a;
        if ((t >= 0.0F) && (t < bestT)) {
          const float hitY = oy + dy * t;
          if ((hitY >= -halfHeight) && (hitY <= halfHeight)) {
            bestT = t;
            const float hitX = ox + dx * t;
            const float hitZ = oz + dz * t;
            const float invR = 1.0F / std::max(radius, 1e-6F);
            bestN = engine::math::Vec3(hitX * invR, 0.0F, hitZ * invR);
          }
        }
      }
    }
  }

  // 2) Hemisphere tests (top at +halfHeight, bottom at -halfHeight).
  for (int h = -1; h <= 1; h += 2) {
    const float cy = static_cast<float>(h) * halfHeight;
    const float ooy = oy - cy;
    const float a = dx * dx + dy * dy + dz * dz;
    const float b = 2.0F * (ox * dx + ooy * dy + oz * dz);
    const float c = ox * ox + ooy * ooy + oz * oz - radius * radius;
    const float disc = b * b - 4.0F * a * c;
    if ((a > 1e-12F) && (disc >= 0.0F)) {
      const float sqrtDisc = std::sqrt(disc);
      const float inv2a = 1.0F / (2.0F * a);
      for (int sign = -1; sign <= 1; sign += 2) {
        const float t = (-b + static_cast<float>(sign) * sqrtDisc) * inv2a;
        if ((t >= 0.0F) && (t < bestT)) {
          const float hitY = oy + dy * t - cy;
          // Accept hit only on the correct hemisphere side.
          if ((h > 0 && hitY >= 0.0F) || (h < 0 && hitY <= 0.0F)) {
            bestT = t;
            const float hitX = ox + dx * t;
            const float hitZ = oz + dz * t;
            const float invR = 1.0F / std::max(radius, 1e-6F);
            bestN = engine::math::Vec3(hitX * invR, (oy + dy * t - cy) * invR,
                                       hitZ * invR);
          }
        }
      }
    }
  }

  if (bestT <= maxDist) {
    outT = bestT;
    outNormal = engine::math::normalize(bestN);
    return true;
  }
  return false;
}

// Ray-vs-convex-hull intersection using slab method against face planes.
// `center` is the world-space position of the hull entity.
bool ray_intersects_convex_hull(const engine::math::Ray &ray,
                                const engine::math::Vec3 &center,
                                const ConvexHullData &hull, float maxDist,
                                float &outT,
                                engine::math::Vec3 &outNormal) noexcept {
  float tNear = 0.0F;
  float tFar = maxDist;
  engine::math::Vec3 nearNormal(0.0F, 1.0F, 0.0F);

  for (std::size_t i = 0U; i < hull.planeCount; ++i) {
    const ConvexHullData::Plane &plane = hull.planes[i];
    // Plane in world space: dot(normal, X) = distance + dot(normal, center)
    const float worldDist =
        plane.distance + engine::math::dot(plane.normal, center);

    const float denom = engine::math::dot(plane.normal, ray.direction);
    const float numer = worldDist - engine::math::dot(plane.normal, ray.origin);

    if (std::fabs(denom) < 1e-10F) {
      // Ray parallel to plane.
      if (numer < 0.0F) {
        return false; // outside this plane
      }
      continue;
    }

    const float t = numer / denom;
    if (denom < 0.0F) {
      // Entry plane.
      if (t > tNear) {
        tNear = t;
        nearNormal = plane.normal;
      }
    } else {
      // Exit plane.
      if (t < tFar) {
        tFar = t;
      }
    }

    if (tNear > tFar) {
      return false;
    }
  }

  if ((tNear >= 0.0F) && (tNear <= maxDist)) {
    outT = tNear;
    outNormal = nearNormal;
    return true;
  }
  return false;
}

} // namespace

bool raycast(const runtime::World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             runtime::PhysicsRaycastHit *outHit,
             runtime::Entity skipEntity) noexcept {
  // Validate direction vector to prevent NaN from normalization.
  if (math::length_sq(direction) < 1e-12F) {
    return false;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return false;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }

  const math::Ray ray{origin, direction};
  bool hit = false;
  float bestT = maxDistance;

  for (std::size_t i = 0U; i < count; ++i) {
    if (entities[i] == skipEntity) {
      continue;
    }
    runtime::Transform transform{};
    if (!world.get_transform(entities[i], &transform)) {
      continue;
    }
    const runtime::Collider &col = colliders[i];
    float t = 0.0F;

    math::Vec3 shapeNormal(0.0F, 1.0F, 0.0F);

    if (col.shape == runtime::ColliderShape::Sphere) {
      const math::Sphere sphere{transform.position, col.halfExtents.x};
      if (!math::ray_intersects_sphere(ray, sphere, &t)) {
        continue;
      }
    } else if (col.shape == runtime::ColliderShape::Capsule) {
      if (!ray_intersects_capsule(ray, transform.position, col.halfExtents.y,
                                  col.halfExtents.x, bestT, t, shapeNormal)) {
        continue;
      }
    } else if (col.shape == runtime::ColliderShape::ConvexHull) {
      const ConvexHullData *hull = find_hull_data(entities[i].index);
      if ((hull == nullptr) ||
          !ray_intersects_convex_hull(ray, transform.position, *hull, bestT, t,
                                      shapeNormal)) {
        continue;
      }
    } else if (col.shape == runtime::ColliderShape::Heightfield) {
      const HeightfieldData *hf = find_heightfield_data(entities[i].index);
      if ((hf == nullptr) ||
          !ray_intersects_heightfield(ray, transform.position, *hf, bestT, t,
                                      shapeNormal)) {
        continue;
      }
    } else {
      const math::AABB box = math::aabb_from_center_half_extents(
          transform.position, col.halfExtents);
      if (!math::ray_intersects_aabb(ray, box, &t)) {
        continue;
      }
    }

    if ((t >= 0.0F) && (t <= bestT)) {
      bestT = t;
      hit = true;
      if (outHit != nullptr) {
        outHit->entity = entities[i];
        outHit->distance = t;
        outHit->point = math::add(origin, math::mul(direction, t));
        if (col.shape == runtime::ColliderShape::Sphere) {
          outHit->normal =
              math::normalize(math::sub(outHit->point, transform.position));
        } else if (col.shape == runtime::ColliderShape::Capsule ||
                   col.shape == runtime::ColliderShape::ConvexHull ||
                   col.shape == runtime::ColliderShape::Heightfield) {
          outHit->normal = shapeNormal;
        } else {
          outHit->normal = aabb_hit_normal(outHit->point, transform.position,
                                           col.halfExtents);
        }
      }
    }
  }

  return hit;
}

std::size_t raycast_all(const runtime::World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        runtime::PhysicsRaycastHit *outHits,
                        std::size_t maxHits) noexcept {
  // Validate direction vector to prevent NaN from normalization.
  if (math::length_sq(direction) < 1e-12F) {
    return 0U;
  }

  if ((outHits == nullptr) || (maxHits == 0U)) {
    return 0U;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return 0U;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return 0U;
  }

  const math::Ray ray{origin, direction};
  std::size_t hitCount = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    runtime::Transform transform{};
    if (!world.get_transform(entities[i], &transform)) {
      continue;
    }
    const runtime::Collider &col = colliders[i];
    float t = 0.0F;
    math::Vec3 shapeNormal(0.0F, 1.0F, 0.0F);

    if (col.shape == runtime::ColliderShape::Sphere) {
      const math::Sphere sphere{transform.position, col.halfExtents.x};
      if (!math::ray_intersects_sphere(ray, sphere, &t)) {
        continue;
      }
    } else if (col.shape == runtime::ColliderShape::Capsule) {
      if (!ray_intersects_capsule(ray, transform.position, col.halfExtents.y,
                                  col.halfExtents.x, maxDistance, t,
                                  shapeNormal)) {
        continue;
      }
    } else if (col.shape == runtime::ColliderShape::ConvexHull) {
      const ConvexHullData *hull = find_hull_data(entities[i].index);
      if ((hull == nullptr) ||
          !ray_intersects_convex_hull(ray, transform.position, *hull,
                                      maxDistance, t, shapeNormal)) {
        continue;
      }
    } else if (col.shape == runtime::ColliderShape::Heightfield) {
      const HeightfieldData *hf = find_heightfield_data(entities[i].index);
      if ((hf == nullptr) ||
          !ray_intersects_heightfield(ray, transform.position, *hf, maxDistance,
                                      t, shapeNormal)) {
        continue;
      }
    } else {
      const math::AABB box = math::aabb_from_center_half_extents(
          transform.position, col.halfExtents);
      if (!math::ray_intersects_aabb(ray, box, &t)) {
        continue;
      }
    }

    if ((t >= 0.0F) && (t <= maxDistance)) {
      if (hitCount < maxHits) {
        runtime::PhysicsRaycastHit &rh = outHits[hitCount];
        rh.entity = entities[i];
        rh.distance = t;
        rh.point = math::add(origin, math::mul(direction, t));
        if (col.shape == runtime::ColliderShape::Sphere) {
          rh.normal = math::normalize(math::sub(rh.point, transform.position));
        } else if (col.shape == runtime::ColliderShape::Capsule ||
                   col.shape == runtime::ColliderShape::ConvexHull ||
                   col.shape == runtime::ColliderShape::Heightfield) {
          rh.normal = shapeNormal;
        } else {
          rh.normal =
              aabb_hit_normal(rh.point, transform.position, col.halfExtents);
        }
        ++hitCount;
      }
    }
  }

  // Sort hits by distance.
  std::sort(outHits, outHits + hitCount,
            [](const runtime::PhysicsRaycastHit &a,
               const runtime::PhysicsRaycastHit &b) noexcept {
              return a.distance < b.distance;
            });

  return hitCount;
}

JointId add_distance_joint(runtime::World &world, runtime::Entity entityA,
                           runtime::Entity entityB, float distance) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  for (std::size_t i = 0U; i < runtime::World::kMaxPhysicsJoints; ++i) {
    if (!ctx.joints[i].active) {
      ctx.joints[i].entityA = entityA;
      ctx.joints[i].entityB = entityB;
      ctx.joints[i].type = runtime::World::JointType::Distance;
      ctx.joints[i].distance = distance;
      ctx.joints[i].active = true;
      ctx.joints[i].accumulatedImpulse = 0.0F;
      if (i >= ctx.jointCount) {
        ctx.jointCount = i + 1U;
      }
      return static_cast<JointId>(i);
    }
  }
  return kInvalidJointId;
}

void remove_joint(runtime::World &world, JointId id) noexcept {
  runtime::World::PhysicsContext &ctx = world.physics_context();
  if (id >= runtime::World::kMaxPhysicsJoints) {
    return;
  }
  ctx.joints[id].active = false;
  // Shrink high-water mark.
  while ((ctx.jointCount > 0U) && !ctx.joints[ctx.jointCount - 1U].active) {
    --ctx.jointCount;
  }
}

void wake_body(runtime::World &world, runtime::Entity entity) noexcept {
  runtime::RigidBody *body = world.get_rigid_body_ptr(entity);
  if (body != nullptr) {
    body->sleeping = false;
    body->sleepFrameCount = 0U;
  }
}

bool is_sleeping(const runtime::World &world, runtime::Entity entity) noexcept {
  runtime::RigidBody rb{};
  if (!world.get_rigid_body(entity, &rb)) {
    return false;
  }
  return rb.sleeping;
}

} // namespace engine::physics
