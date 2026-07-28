// Implements physics behavior for the Engine physics system.

#include "engine/physics/physics.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/math/aabb.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/physics/ccd.h"
#include "engine/physics/collider.h"
#include "engine/physics/constraint_solver.h"
#include "engine/physics/convex_hull.h"

#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"
#include "joint_handle.h"

namespace engine::physics {

namespace {

bool cvar_exists(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }

  core::CVarInfo infos[256] = {};
  const std::size_t count = core::cvar_get_all(infos, 256U);
  for (std::size_t i = 0U; i < count; ++i) {
    if ((infos[i].name != nullptr) && (std::strcmp(infos[i].name, name) == 0)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool register_physics_cvars() noexcept {
  bool ok = true;
  if (!cvar_exists("physics.solver_iterations")) {
    ok = core::cvar_register_int("physics.solver_iterations", 8,
                                 "Number of constraint solver iterations") &&
         ok;
  }
  if (!cvar_exists("physics.ccd_threshold")) {
    ok = core::cvar_register_float(
             "physics.ccd_threshold", 2.0F,
             "Minimum velocity magnitude (m/s) to trigger CCD") &&
         ok;
  }
  return ok;
}

PhysicsContext::PhysicsContext() noexcept
    : shapeStore(new (std::nothrow) PhysicsShapeStore()) {}

PhysicsContext::PhysicsContext(const PhysicsContext &other) noexcept
    : PhysicsContext() {
  *this = other;
}

PhysicsContext &
PhysicsContext::operator=(const PhysicsContext &other) noexcept {
  if (this == &other) {
    return *this;
  }

  gravity = other.gravity;
  joints = other.joints;
  jointCount = other.jointCount;
  collisionPairData = other.collisionPairData;
  collisionPairCount = other.collisionPairCount;
  collisionDispatch = other.collisionDispatch;
  pairHashKeys = other.pairHashKeys;
  pairHashStamps = other.pairHashStamps;
  pairHashGeneration = other.pairHashGeneration;
  testedStamps = other.testedStamps;
  testedGeneration = other.testedGeneration;

  if (other.shapeStore == nullptr) {
    shapeStore.reset();
  } else {
    if (shapeStore == nullptr) {
      shapeStore.reset(new (std::nothrow) PhysicsShapeStore());
    }
    if (shapeStore != nullptr) {
      *shapeStore = *other.shapeStore;
    }
  }

  return *this;
}

/// Finds the matching object or resource for hull data.
ConvexHullData *find_hull_data(PhysicsContext &context,
                               Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    if (store->convexHullEntity[i] == entity) {
      return &store->convexHullData[i];
    }
  }
  return nullptr;
}

/// Finds the matching object or resource for hull data.
const ConvexHullData *find_hull_data(const PhysicsContext &context,
                                     Entity entity) noexcept {
  const PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    if (store->convexHullEntity[i] == entity) {
      return &store->convexHullData[i];
    }
  }
  return nullptr;
}

ConvexHullData *allocate_hull_data(PhysicsContext &context,
                                   Entity entity) noexcept {
  ConvexHullData *existing = find_hull_data(context, entity);
  if (existing != nullptr) {
    return existing;
  }
  PhysicsShapeStore *store = context.shapeStore.get();
  if ((store == nullptr) || (store->convexHullCount >= kMaxConvexHulls)) {
    return nullptr;
  }
  store->convexHullEntity[store->convexHullCount] = entity;
  store->convexHullData[store->convexHullCount] = ConvexHullData{};
  return &store->convexHullData[store->convexHullCount++];
}

/// Finds the matching object or resource for heightfield data.
HeightfieldData *find_heightfield_data(PhysicsContext &context,
                                       Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    if (store->heightfieldEntity[i] == entity) {
      return &store->heightfieldData[i];
    }
  }
  return nullptr;
}

/// Finds the matching object or resource for heightfield data.
const HeightfieldData *find_heightfield_data(const PhysicsContext &context,
                                             Entity entity) noexcept {
  const PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    if (store->heightfieldEntity[i] == entity) {
      return &store->heightfieldData[i];
    }
  }
  return nullptr;
}

HeightfieldData *allocate_heightfield_data(PhysicsContext &context,
                                           Entity entity) noexcept {
  HeightfieldData *existing = find_heightfield_data(context, entity);
  if (existing != nullptr) {
    return existing;
  }
  PhysicsShapeStore *store = context.shapeStore.get();
  if ((store == nullptr) || (store->heightfieldCount >= kMaxHeightfields)) {
    return nullptr;
  }
  store->heightfieldEntity[store->heightfieldCount] = entity;
  store->heightfieldData[store->heightfieldCount] = HeightfieldData{};
  return &store->heightfieldData[store->heightfieldCount++];
}

void remove_hull_data(PhysicsContext &context, Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    if (store->convexHullEntity[i] == entity) {
      const std::size_t last = store->convexHullCount - 1U;
      if (i != last) {
        store->convexHullData[i] = store->convexHullData[last];
        store->convexHullEntity[i] = store->convexHullEntity[last];
      }
      store->convexHullData[last] = ConvexHullData{};
      store->convexHullEntity[last] = kInvalidEntity;
      --store->convexHullCount;
      return;
    }
  }
}

void remove_heightfield_data(PhysicsContext &context, Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    if (store->heightfieldEntity[i] == entity) {
      const std::size_t last = store->heightfieldCount - 1U;
      if (i != last) {
        store->heightfieldData[i] = store->heightfieldData[last];
        store->heightfieldEntity[i] = store->heightfieldEntity[last];
      }
      store->heightfieldData[last] = HeightfieldData{};
      store->heightfieldEntity[last] = kInvalidEntity;
      --store->heightfieldCount;
      return;
    }
  }
}

bool validate_convex_hull_data(const ConvexHullData &hull) noexcept {
  return (hull.vertexCount > 0U) &&
         (hull.vertexCount <= ConvexHullData::kMaxVertices) &&
         (hull.planeCount > 0U) &&
         (hull.planeCount <= ConvexHullData::kMaxPlanes);
}

bool validate_heightfield_data(const HeightfieldData &heightfield) noexcept {
  if ((heightfield.rows < 2U) || (heightfield.columns < 2U) ||
      (heightfield.rows > HeightfieldData::kMaxResolution) ||
      (heightfield.columns > HeightfieldData::kMaxResolution) ||
      (heightfield.spacingX <= 0.0F) || (heightfield.spacingZ <= 0.0F)) {
    return false;
  }

  return heightfield.rows <=
         (HeightfieldData::kMaxSamples / heightfield.columns);
}

// Public accessors used by the runtime bridge.
bool set_convex_hull_data(PhysicsContext &context, Entity entity,
                          const ConvexHullData &hull) noexcept {
  if (!validate_convex_hull_data(hull)) {
    return false;
  }

  ConvexHullData *slot = allocate_hull_data(context, entity);
  if (slot == nullptr) {
    return false;
  }
  *slot = hull;
  return true;
}

const ConvexHullData *get_convex_hull_data(const PhysicsContext &context,
                                           Entity entity) noexcept {
  return find_hull_data(context, entity);
}

void remove_shape_payloads(PhysicsContext &context, Entity entity) noexcept {
  remove_hull_data(context, entity);
  remove_heightfield_data(context, entity);
}

/// Sets the requested value for heightfield data impl.
bool set_heightfield_data(PhysicsContext &context, Entity entity,
                          const HeightfieldData &hf) noexcept {
  if (!validate_heightfield_data(hf)) {
    return false;
  }

  HeightfieldData *slot = allocate_heightfield_data(context, entity);
  if (slot == nullptr) {
    return false;
  }
  *slot = hf;
  return true;
}

const HeightfieldData *get_heightfield_data(const PhysicsContext &context,
                                            Entity entity) noexcept {
  return find_heightfield_data(context, entity);
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

/// Begins the requested operation or profiling range for generation.
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

// Wake a sleeping body if the other body has velocity above threshold.
void maybe_wake_pair(RigidBody *bodyA, RigidBody *bodyB, float vA2,
                     float vB2) noexcept {
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
void apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
                            const engine::math::Vec3 &normal, float invMassA,
                            float invMassB, float invMassSum,
                            const engine::math::Vec3 &contactOffsetA,
                            const engine::math::Vec3 &contactOffsetB,
                            float restitution, float staticFric,
                            float dynamicFric) noexcept;

// Resolve a collision between two shapes given contact normal, overlap, and
// the contact point.  Applies positional correction and velocity impulse.
void resolve_contact(PhysicsWorldView &world,
                     const PhysicsWorldView::SimulationAccessToken &simToken,
                     Entity bodyEntityA, Entity bodyEntityB,
                     const engine::math::Vec3 &bodyCenterA,
                     const engine::math::Vec3 &bodyCenterB, RigidBody *bodyA,
                     RigidBody *bodyB, float invMassA, float invMassB,
                     float invMassSum, const engine::math::Vec3 &normal,
                     float overlap, const engine::math::Vec3 &contactPt,
                     const Collider &colliderA,
                     const Collider &colliderB) noexcept {
  const float moveA = overlap * (invMassA / invMassSum);
  const float moveB = overlap * (invMassB / invMassSum);

  Transform *mutableA =
      (invMassA > 0.0F) ? world.get_transform_write_ptr(bodyEntityA, simToken)
                        : nullptr;
  Transform *mutableB =
      (invMassB > 0.0F) ? world.get_transform_write_ptr(bodyEntityB, simToken)
                        : nullptr;
  if (((invMassA > 0.0F) && (mutableA == nullptr)) ||
      ((invMassB > 0.0F) && (mutableB == nullptr))) {
    return;
  }

  if (mutableA != nullptr) {
    mutableA->position =
        engine::math::sub(mutableA->position, engine::math::mul(normal, moveA));
  }
  if (mutableB != nullptr) {
    mutableB->position =
        engine::math::add(mutableB->position, engine::math::mul(normal, moveB));
  }

  const float combinedRest =
      std::max(colliderA.restitution, colliderB.restitution);
  const float combinedStaticFric =
      std::sqrt(colliderA.staticFriction * colliderB.staticFriction);
  const float combinedDynFric =
      std::sqrt(colliderA.dynamicFriction * colliderB.dynamicFriction);
  const engine::math::Vec3 correctedCenterA =
      engine::math::sub(bodyCenterA, engine::math::mul(normal, moveA));
  const engine::math::Vec3 correctedCenterB =
      engine::math::add(bodyCenterB, engine::math::mul(normal, moveB));
  apply_velocity_impulse(bodyA, bodyB, normal, invMassA, invMassB, invMassSum,
                         engine::math::sub(contactPt, correctedCenterA),
                         engine::math::sub(contactPt, correctedCenterB),
                         combinedRest, combinedStaticFric, combinedDynFric);
}

// Resolve a speculative contact (E2a/E2b).
// Bodies are NOT yet overlapping but are approaching. Apply a clamped velocity
// impulse to prevent penetration in the next frame — no positional correction,
// no restitution, and the impulse is clamped to zero minimum (can only push
// apart, never pull together).
void resolve_speculative_contact(RigidBody *bodyA, RigidBody *bodyB,
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

void apply_velocity_impulse(RigidBody *bodyA, RigidBody *bodyB,
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

const ConvexHullData *get_hull_data_ptr(const PhysicsContext &context,
                                        Entity entity) noexcept {
  return find_hull_data(context, entity);
}

// Forward declaration — step_physics delegates to step_physics_range.
bool step_physics_range(PhysicsWorldView &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;

bool step_physics(PhysicsWorldView &world, float deltaSeconds) noexcept {
  return step_physics_range(world, 0U, world.transform_count(), deltaSeconds);
}

bool step_physics_range(PhysicsWorldView &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept {
  const PhysicsContext &physicsCtx = world.physics_context();
  const Entity *entities = nullptr;
  const Transform *readTransforms = nullptr;
  Transform *writeTransforms = nullptr;

  if (!world.get_transform_update_range(startIndex, count, &entities,
                                        &readTransforms, &writeTransforms)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const Entity entity = entities[i];
    if (world.movement_authority(entity) == MovementAuthority::Script) {
      writeTransforms[i] = readTransforms[i];
      continue;
    }

    RigidBody *body = world.get_rigid_body_ptr(entity);
    Transform updated = readTransforms[i];

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

      // CCD: sweep every collider owned by this rigid-body root, including
      // child colliders in a compound object, and keep the earliest impact.
      bool clamped = false;
      CcdSweepResult earliestCcd{};
      const std::size_t colliderCount = world.collider_count();
      const Entity *colliderEntities = nullptr;
      const Collider *colliders = nullptr;
      if ((colliderCount > 0U) &&
          !world.get_collider_range(0U, colliderCount, &colliderEntities,
                                    &colliders)) {
        return false;
      }
      for (std::size_t colliderIndex = 0U; colliderIndex < colliderCount;
           ++colliderIndex) {
        if (world.rigid_body_owner(colliderEntities[colliderIndex]) != entity) {
          continue;
        }
        const CcdSweepResult candidate = bilateral_advance_ccd(
            world, colliderEntities[colliderIndex], *body,
            colliders[colliderIndex], readTransforms[i], deltaSeconds);
        if (candidate.hit && (!earliestCcd.hit || (candidate.timeOfImpact <
                                                   earliestCcd.timeOfImpact))) {
          earliestCcd = candidate;
        }
      }
      if (earliestCcd.hit) {
        const float safeToi = std::max(0.0F, earliestCcd.timeOfImpact - 0.01F);
        updated.position =
            engine::math::add(readTransforms[i].position,
                              engine::math::mul(displacement, safeToi));

        const float normalVelocity =
            engine::math::dot(body->velocity, earliestCcd.contactNormal);
        if (normalVelocity < 0.0F) {
          body->velocity = engine::math::sub(
              body->velocity, engine::math::mul(earliestCcd.contactNormal,
                                                2.0F * normalVelocity));
        }
        clamped = true;
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
    }

    // Angular velocity integration (independent of linear mass).
    if ((body != nullptr) && (body->inverseInertia > 0.0F)) {
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

namespace {

// Narrow-phase dispatch context: everything resolve_collisions computes for a
// broadphase candidate pair before shape-specific handling. Each shape-pair
// tester below consumes one of these instead of sharing one 1,000-line body
// (REVIEW_FINDINGS A4).
struct PairContext final {
  PhysicsWorldView &world;
  const PhysicsWorldView::SimulationAccessToken &simToken;
  PhysicsContext &physicsCtx;
  Entity entityA;
  Entity entityB;
  Entity bodyEntityA;
  Entity bodyEntityB;
  const Collider &colliderA;
  const Collider &colliderB;
  const ColliderWorldGeometry &geometryA;
  const ColliderWorldGeometry &geometryB;
  RigidBody *bodyA;
  RigidBody *bodyB;
  float invMassA;
  float invMassB;
  float invMassSum;
  engine::math::Vec3 posA;
  engine::math::Vec3 posB;
  engine::math::Vec3 bodyCenterA;
  engine::math::Vec3 bodyCenterB;
  bool requiresAffineNarrowPhase;
  float speculativeDt;
};

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

  // Compute object radius for footprint.
  float objRadius = 0.0F;
  if (objCol.shape == ColliderShape::Sphere) {
    objRadius = objCol.halfExtents.x;
  } else if (objCol.shape == ColliderShape::Capsule) {
    objRadius = objCol.halfExtents.y + objCol.halfExtents.x;
  } else {
    objRadius = engine::math::length(objCol.halfExtents);
  }

  // Map object footprint to grid cell range.
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

      engine::math::Vec3 triVerts[2][3] = {{v00, v10, v01}, {v10, v11, v01}};

      for (int ti = 0; ti < 2; ++ti) {
        // Upward-facing triangle face normal.
        const engine::math::Vec3 e1 =
            engine::math::sub(triVerts[ti][1], triVerts[ti][0]);
        const engine::math::Vec3 e2 =
            engine::math::sub(triVerts[ti][2], triVerts[ti][0]);
        engine::math::Vec3 faceN = engine::math::cross(e1, e2);
        const float faceLen = engine::math::length(faceN);
        if (faceLen < 1e-10F) {
          continue; // degenerate triangle
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
  // Normal points from capsule toward sphere.
  engine::math::Vec3 normal = (dist2 > 0.0F)
                                  ? engine::math::mul(diff, 1.0F / dist)
                                  : engine::math::Vec3(0.0F, 1.0F, 0.0F);
  // Ensure normal points from A toward B.
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
  const engine::math::Vec3 cpC =
      closest_point_on_aabb(segClosest, boxPos, boxCol.halfExtents);

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
  const engine::math::Vec3 diff = engine::math::sub(finalSeg, finalBox);
  const float dist2 = engine::math::dot(diff, diff);

  if (dist2 >= capR * capR) {
    return;
  }

  if (!record_pair_and_wake(pair)) {
    return;
  }

  const float dist = (dist2 > 0.0F) ? std::sqrt(dist2) : 0.0001F;
  // Normal points from box toward capsule segment.
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
  // The solver expects the normal to point from A toward B; the computed
  // direction is box → capsule, so flip when A is the capsule.
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
    // Speculative contact for spheres: if gap is small,
    // apply clamped impulse.
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
    // Speculative contact: AABB vs Sphere.
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

  // Normal points from box toward sphere; flip if A is the sphere.
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

  // Speculative contacts (E2a): if AABB pair is NOT overlapping but the gap is
  // small enough that approach velocity could close it in one frame, apply a
  // speculative impulse to prevent penetration.
  if (!hasOverlap) {
    // Minimum overlap (most negative = largest gap on that axis).
    const float minOverlap = std::min({overlapX, overlapY, overlapZ});
    const float gap = -minOverlap; // positive = actual gap distance

    if ((gap > 0.0F) && (gap < pair.speculativeDt * 300.0F)) {
      // Determine the speculative contact normal (axis of smallest gap).
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

  const engine::math::Vec3 aabbNormal(pushX, pushY, pushZ);
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

} // namespace

bool resolve_collisions(PhysicsWorldView &world, float deltaSeconds) noexcept {
  const auto simToken = world.simulation_access_token();
  PhysicsContext &physicsCtx = world.physics_context();
  if (deltaSeconds <= 0.0F) {
    return false;
  }

  physicsCtx.collisionPairCount = 0U;
  begin_generation(&physicsCtx.pairHashGeneration,
                   physicsCtx.pairHashStamps.data(),
                   physicsCtx.pairHashStamps.size());

  const std::size_t colliderCount = world.collider_count();

  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((colliderCount > 0U) &&
      !world.get_collider_range(0U, colliderCount, &entities, &colliders)) {
    return false;
  }

  // Build collider world geometry serially in dense order after all parallel
  // integration jobs. This is the only simulation stage allowed to compose
  // child transforms from the write buffer.
  thread_local static std::array<ColliderWorldGeometry, kMaxColliders>
      geometries{};
  thread_local static std::array<Entity, kMaxColliders> bodyOwners{};
  thread_local static std::array<engine::math::Vec3, kMaxColliders>
      bodyCenters{};
  thread_local static std::array<bool, kMaxColliders> geometryValid{};
  thread_local static std::array<float, kMaxColliders> posX{}, posY{}, posZ{};

  for (std::size_t i = 0U; i < colliderCount; ++i) {
    PhysicsTransform entityTransform{};
    const ConvexHullData *hull = nullptr;
    if (colliders[i].shape == ColliderShape::ConvexHull) {
      hull = find_hull_data(physicsCtx, entities[i]);
    }

    geometryValid[i] =
        world.get_simulation_physics_transform(entities[i], simToken,
                                               &entityTransform) &&
        make_collider_world_geometry(colliders[i], entityTransform.matrix, hull,
                                     &geometries[i]);
    bodyOwners[i] = kInvalidEntity;
    bodyCenters[i] = engine::math::Vec3(0.0F, 0.0F, 0.0F);
    posX[i] = 0.0F;
    posY[i] = 0.0F;
    posZ[i] = 0.0F;
    if (!geometryValid[i]) {
      continue;
    }

    posX[i] = geometries[i].center.x;
    posY[i] = geometries[i].center.y;
    posZ[i] = geometries[i].center.z;
    bodyOwners[i] = world.rigid_body_owner(entities[i], simToken);
    if (bodyOwners[i] == kInvalidEntity) {
      bodyCenters[i] = geometries[i].center;
      continue;
    }

    PhysicsTransform bodyTransform{};
    if (!world.get_simulation_physics_transform(bodyOwners[i], simToken,
                                                &bodyTransform)) {
      geometryValid[i] = false;
      bodyOwners[i] = kInvalidEntity;
      continue;
    }
    bodyCenters[i] = bodyTransform.position;
  }

  if (colliderCount >= 2U) {

    // ---- Broadphase: spatial hash grid
    // ----------------------------------------

    // Compute cell size: max of kDefaultCellSize and 2× largest half-extent.
    float cellSize = kDefaultCellSize;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      if (!geometryValid[i]) {
        continue;
      }
      const engine::math::Vec3 he =
          engine::math::aabb_half_extents(geometries[i].worldAabb);
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
    constexpr std::size_t kMaxNodes = kMaxColliders * 8U;
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
    // For speculative contacts: expand AABB by velocity * active timestep.
    const float speculativeDt = deltaSeconds;
    for (std::size_t i = 0U; i < colliderCount; ++i) {
      if (!geometryValid[i]) {
        continue;
      }

      // Expand AABB by velocity to detect speculative contacts.
      const Entity bodyOwner = bodyOwners[i];
      const RigidBody *bodyI = (bodyOwner != kInvalidEntity)
                                   ? world.get_rigid_body_ptr(bodyOwner)
                                   : nullptr;
      float expandX = 0.0F;
      float expandY = 0.0F;
      float expandZ = 0.0F;
      if ((bodyI != nullptr) && (bodyI->inverseMass > 0.0F)) {
        const engine::math::Vec3 centerOffset =
            engine::math::sub(geometries[i].center, bodyCenters[i]);
        const engine::math::Vec3 pointVelocity = engine::math::add(
            bodyI->velocity,
            engine::math::cross(bodyI->angularVelocity, centerOffset));
        expandX = std::fabs(pointVelocity.x) * speculativeDt;
        expandY = std::fabs(pointVelocity.y) * speculativeDt;
        expandZ = std::fabs(pointVelocity.z) * speculativeDt;
      }

      const engine::math::AABB &bounds = geometries[i].worldAabb;
      const std::int32_t minCX = cell_coord(bounds.min.x - expandX);
      const std::int32_t maxCX = cell_coord(bounds.max.x + expandX);
      const std::int32_t minCY = cell_coord(bounds.min.y - expandY);
      const std::int32_t maxCY = cell_coord(bounds.max.y + expandY);
      const std::int32_t minCZ = cell_coord(bounds.min.z - expandZ);
      const std::int32_t maxCZ = cell_coord(bounds.max.z + expandZ);
      for (std::int32_t cx = minCX; cx <= maxCX; ++cx) {
        for (std::int32_t cy = minCY; cy <= maxCY; ++cy) {
          for (std::int32_t cz = minCZ; cz <= maxCZ; ++cz) {
            insert_node(hash_cell(cx, cy, cz), static_cast<std::uint32_t>(i));
          }
        }
      }
    }

    for (std::size_t i = 0U; i < colliderCount; ++i) {
      if (!geometryValid[i]) {
        continue;
      }
      const Entity entityA = entities[i];
      const Entity authorityEntityA =
          (bodyOwners[i] != kInvalidEntity) ? bodyOwners[i] : entityA;
      if (world.movement_authority(authorityEntityA) ==
          MovementAuthority::Script) {
        continue;
      }

      const float ax = posX[i];
      const float ay = posY[i];
      const float az = posZ[i];

      begin_generation(&physicsCtx.testedGeneration,
                       physicsCtx.testedStamps.data(),
                       physicsCtx.testedStamps.size());
      physicsCtx.testedStamps[i] = physicsCtx.testedGeneration;

      const engine::math::AABB &boundsA = geometries[i].worldAabb;
      const std::int32_t minCX = cell_coord(boundsA.min.x);
      const std::int32_t maxCX = cell_coord(boundsA.max.x);
      const std::int32_t minCY = cell_coord(boundsA.min.y);
      const std::int32_t maxCY = cell_coord(boundsA.max.y);
      const std::int32_t minCZ = cell_coord(boundsA.min.z);
      const std::int32_t maxCZ = cell_coord(boundsA.max.z);

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

              const Entity entityB = entities[j];
              if (!geometryValid[j]) {
                continue;
              }
              const Entity authorityEntityB =
                  (bodyOwners[j] != kInvalidEntity) ? bodyOwners[j] : entityB;
              if (world.movement_authority(authorityEntityB) ==
                  MovementAuthority::Script) {
                continue;
              }

              if ((bodyOwners[i] != kInvalidEntity) &&
                  (bodyOwners[i] == bodyOwners[j])) {
                continue;
              }

              const float bx = posX[j];
              const float by = posY[j];
              const float bz = posZ[j];

              const Collider &colliderA = colliders[i];
              const Collider &colliderB = colliders[j];

              // Collision layer/mask filtering (P1-M3-C2b).
              if (((colliderA.collisionLayer & colliderB.collisionMask) ==
                   0U) ||
                  ((colliderB.collisionLayer & colliderA.collisionMask) ==
                   0U)) {
                continue;
              }

              const Entity bodyEntityA = bodyOwners[i];
              const Entity bodyEntityB = bodyOwners[j];
              RigidBody *bodyA = (bodyEntityA != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(bodyEntityA)
                                     : nullptr;
              RigidBody *bodyB = (bodyEntityB != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(bodyEntityB)
                                     : nullptr;
              const float invMassA =
                  (bodyA != nullptr) ? bodyA->inverseMass : kStaticInverseMass;
              const float invMassB =
                  (bodyB != nullptr) ? bodyB->inverseMass : kStaticInverseMass;
              const float invMassSum = invMassA + invMassB;

              const auto shapeA = colliderA.shape;
              const auto shapeB = colliderB.shape;
              const bool aIsAABB = (shapeA == ColliderShape::AABB);
              const bool bIsAABB = (shapeB == ColliderShape::AABB);
              const bool aIsCapsule = (shapeA == ColliderShape::Capsule);
              const bool bIsCapsule = (shapeB == ColliderShape::Capsule);
              const bool aIsSphere = (shapeA == ColliderShape::Sphere);
              const bool bIsSphere = (shapeB == ColliderShape::Sphere);
              const bool aIsConvex = (shapeA == ColliderShape::ConvexHull);
              const bool bIsConvex = (shapeB == ColliderShape::ConvexHull);
              const bool aIsHeightfield =
                  (shapeA == ColliderShape::Heightfield);
              const bool bIsHeightfield =
                  (shapeB == ColliderShape::Heightfield);

              const bool compoundA =
                  (bodyEntityA != kInvalidEntity) && (bodyEntityA != entityA);
              const bool compoundB =
                  (bodyEntityB != kInvalidEntity) && (bodyEntityB != entityB);
              const bool offsetBodyA =
                  (bodyEntityA != kInvalidEntity) &&
                  (engine::math::length_sq(engine::math::sub(
                       geometries[i].center, bodyCenters[i])) > 1.0e-12F);
              const bool offsetBodyB =
                  (bodyEntityB != kInvalidEntity) &&
                  (engine::math::length_sq(engine::math::sub(
                       geometries[j].center, bodyCenters[j])) > 1.0e-12F);
              const bool requiresAffineNarrowPhase =
                  compoundA || compoundB || offsetBodyA || offsetBodyB ||
                  has_non_identity_linear_transform(geometries[i]) ||
                  has_non_identity_linear_transform(geometries[j]);

              const PairContext pair{world,
                                     simToken,
                                     physicsCtx,
                                     entityA,
                                     entityB,
                                     bodyEntityA,
                                     bodyEntityB,
                                     colliderA,
                                     colliderB,
                                     geometries[i],
                                     geometries[j],
                                     bodyA,
                                     bodyB,
                                     invMassA,
                                     invMassB,
                                     invMassSum,
                                     engine::math::Vec3(ax, ay, az),
                                     engine::math::Vec3(bx, by, bz),
                                     bodyCenters[i],
                                     bodyCenters[j],
                                     requiresAffineNarrowPhase,
                                     speculativeDt};

              // Narrow phase: dispatch to the shape-pair handler.
              if (aIsHeightfield || bIsHeightfield) {
                narrow_phase_heightfield(pair);
                continue;
              }
              if (requiresAffineNarrowPhase || aIsConvex || bIsConvex) {
                narrow_phase_convex_gjk(pair);
                continue;
              }

              if (aIsCapsule && bIsCapsule) {
                narrow_phase_capsule_capsule(pair);
                continue;
              }

              if ((aIsCapsule && bIsSphere) || (aIsSphere && bIsCapsule)) {
                narrow_phase_capsule_sphere(pair);
                continue;
              }

              if ((aIsCapsule && bIsAABB) || (aIsAABB && bIsCapsule)) {
                narrow_phase_capsule_aabb(pair);
                continue;
              }

              if (aIsSphere && bIsSphere) {
                narrow_phase_sphere_sphere(pair);
                continue;
              }

              if (aIsAABB != bIsAABB) {
                narrow_phase_aabb_sphere(pair);
                continue;
              }

              narrow_phase_aabb_aabb(pair);

            } // while nodeIdx
          } // for cz
        } // for cy
      } // for cx
    } // for i
  } // if (colliderCount >= 2U)

  solve_constraints(world, deltaSeconds);

  // Sleep check: after all collision responses and joint solving,
  // examine each rigid body. If velocity is below threshold for enough
  // consecutive frames, put it to sleep.
  const std::size_t rigidBodyCount = world.rigid_body_count();
  const Entity *rigidBodyEntities = nullptr;
  RigidBody *rigidBodies = nullptr;
  if ((rigidBodyCount > 0U) &&
      !world.get_rigid_body_range(0U, rigidBodyCount, &rigidBodyEntities,
                                  &rigidBodies)) {
    return false;
  }
  (void)rigidBodyEntities;
  for (std::size_t i = 0U; i < rigidBodyCount; ++i) {
    RigidBody *body = &rigidBodies[i];
    if ((body->inverseMass <= 0.0F) || body->sleeping) {
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

/// Sets the requested value for gravity.
void set_gravity(PhysicsWorldView &world, float x, float y, float z) noexcept {
  world.physics_context().gravity = engine::math::Vec3(x, y, z);
}

engine::math::Vec3 get_gravity(const PhysicsWorldView &world) noexcept {
  return world.physics_context().gravity;
}

/// Sets the requested value for collision dispatch.
void set_collision_dispatch(PhysicsWorldView &world,
                            CollisionDispatchFn fn) noexcept {
  world.physics_context().collisionDispatch = fn;
}

void dispatch_collision_callbacks(PhysicsWorldView &world) noexcept {
  PhysicsContext &ctx = world.physics_context();
  if ((ctx.collisionDispatch != nullptr) && (ctx.collisionPairCount > 0U)) {
    ctx.collisionDispatch(ctx.collisionPairData.data(), ctx.collisionPairCount);
  }
  ctx.collisionPairCount = 0U;
}

JointId add_distance_joint(PhysicsWorldView &world, Entity entityA,
                           Entity entityB, float distance) noexcept {
  Transform transformA{};
  Transform transformB{};
  if (!std::isfinite(distance) || (distance < 0.0F) ||
      (entityA == kInvalidEntity) || (entityB == kInvalidEntity) ||
      (entityA == entityB) || !world.get_transform(entityA, &transformA) ||
      !world.get_transform(entityB, &transformB)) {
    core::log_message(core::LogLevel::Error, "physics",
                      "invalid distance joint endpoints or distance");
    return kInvalidJointId;
  }

  PhysicsJointSlot *joint = nullptr;
  PhysicsContext &context = world.physics_context();
  const JointId id = claim_joint_slot(context, &joint);
  if ((id == kInvalidJointId) || (joint == nullptr)) {
    core::log_message(core::LogLevel::Error, "physics", "joint table full");
    return kInvalidJointId;
  }

  joint->entityA = entityA;
  joint->entityB = entityB;
  joint->type = JointType::Distance;
  joint->distance = distance;
  joint->active = true;
  joint->accumulatedImpulse = 0.0F;
  return id;
}

void remove_joint(PhysicsWorldView &world, JointId id) noexcept {
  PhysicsContext &context = world.physics_context();
  std::size_t slotIndex = 0U;
  PhysicsJointSlot *joint = find_joint_slot(context, id, &slotIndex);
  if (joint == nullptr) {
    return;
  }

  retire_joint_slot(*joint);
  while ((context.jointCount > 0U) &&
         !context.joints[context.jointCount - 1U].active) {
    --context.jointCount;
  }
}
void wake_body(PhysicsWorldView &world, Entity entity) noexcept {
  RigidBody *body = world.get_rigid_body_ptr(entity);
  if (body != nullptr) {
    body->sleeping = false;
    body->sleepFrameCount = 0U;
  }
}

/// Returns whether is sleeping.
bool is_sleeping(const PhysicsWorldView &world, Entity entity) noexcept {
  RigidBody rb{};
  if (!world.get_rigid_body(entity, &rb)) {
    return false;
  }
  return rb.sleeping;
}

} // namespace engine::physics
