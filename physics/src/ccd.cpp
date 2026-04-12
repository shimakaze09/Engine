#include "engine/physics/ccd.h"

#include "engine/core/cvar.h"
#include "engine/math/aabb.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"
#include "engine/runtime/world.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::physics {

// ---------------------------------------------------------------------------
// CVar: physics.ccd_threshold (default 2.0 m/s)
// Bodies slower than this skip CCD entirely.
// ---------------------------------------------------------------------------
namespace {

const bool g_ccdThresholdCVar = core::cvar_register_float(
    "physics.ccd_threshold", 2.0F,
    "Minimum velocity magnitude (m/s) to trigger CCD");

constexpr int kMaxBilateralIterations = 32;
constexpr float kTolerance = 1e-4F;

// Forward declarations from physics.cpp — defined there, used here.
// We re-declare locally rather than exposing them publicly.

// Compute broadphase half-extents for any collider shape.
math::Vec3 broadphase_half_extents_ccd(const runtime::Collider &col) noexcept {
  if (col.shape == runtime::ColliderShape::Capsule) {
    const float r = col.halfExtents.x;
    const float hh = col.halfExtents.y;
    return math::Vec3(r, hh + r, r);
  }
  return col.halfExtents;
}

// Separating distance between two AABBs given their centers and half-extents.
// Returns a positive value when separated, negative when overlapping.
float aabb_separating_distance(const math::Vec3 &posA,
                               const math::Vec3 &heA,
                               const math::Vec3 &posB,
                               const math::Vec3 &heB) noexcept {
  const float dx =
      std::fabs(posA.x - posB.x) - (heA.x + heB.x);
  const float dy =
      std::fabs(posA.y - posB.y) - (heA.y + heB.y);
  const float dz =
      std::fabs(posA.z - posB.z) - (heA.z + heB.z);
  // The separating distance is the maximum of the three axis distances.
  // If all are negative, they overlap — return the largest (least negative).
  return std::max({dx, dy, dz});
}

// Sphere-vs-sphere separating distance.
float sphere_separating_distance(const math::Vec3 &posA, float radiusA,
                                 const math::Vec3 &posB,
                                 float radiusB) noexcept {
  const float dist = math::length(math::sub(posA, posB));
  return dist - (radiusA + radiusB);
}

// Generic separating distance between two colliders at given positions.
// Positive means separated, negative means overlapping.
float separating_distance(const runtime::Collider &colA,
                          const math::Vec3 &posA,
                          const runtime::Collider &colB,
                          const math::Vec3 &posB) noexcept {
  const bool aIsSphere = (colA.shape == runtime::ColliderShape::Sphere);
  const bool bIsSphere = (colB.shape == runtime::ColliderShape::Sphere);

  if (aIsSphere && bIsSphere) {
    return sphere_separating_distance(posA, colA.halfExtents.x, posB,
                                      colB.halfExtents.x);
  }

  // For all other shape combinations, approximate using AABB bounds.
  const math::Vec3 heA = broadphase_half_extents_ccd(colA);
  const math::Vec3 heB = broadphase_half_extents_ccd(colB);
  return aabb_separating_distance(posA, heA, posB, heB);
}

// Compute the contact normal between two overlapping shapes (A hitting B).
// Returns a normal pointing from B toward A (outward from surface being hit).
math::Vec3 contact_normal_between(const runtime::Collider & /*colA*/,
                                  const math::Vec3 &posA,
                                  const runtime::Collider & /*colB*/,
                                  const math::Vec3 &posB) noexcept {
  const math::Vec3 delta = math::sub(posA, posB);
  const float len = math::length(delta);
  if (len > 1e-8F) {
    return math::div(delta, len);
  }
  return math::Vec3(0.0F, 1.0F, 0.0F);
}

} // namespace

float ccd_velocity_threshold() noexcept {
  return core::cvar_get_float("physics.ccd_threshold", 2.0F);
}

CcdSweepResult bilateral_advance_ccd(
    const runtime::World &world, runtime::Entity entity,
    const runtime::RigidBody &body, const runtime::Collider &collider,
    const runtime::Transform &transform, float dt) noexcept {

  CcdSweepResult result{};

  const float speed = math::length(body.velocity);
  if (speed < 1e-6F) {
    return result;
  }

  // Only trigger CCD if speed exceeds threshold.
  const float threshold = ccd_velocity_threshold();
  if (speed < threshold) {
    return result;
  }

  // The body will travel this far in dt.
  const float travelDist = speed * dt;

  // Minimum half-extent of the moving shape — if travel < this, skip CCD.
  const math::Vec3 movingHe = broadphase_half_extents_ccd(collider);
  const float minHalf = std::min({movingHe.x, movingHe.y, movingHe.z});
  if (travelDist <= minHalf * 0.5F) {
    return result; // Travel distance too small relative to collider size.
  }

  // Iterate over all colliders in the world, find earliest TOI.
  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return result;
  }

  const runtime::Entity *entities = nullptr;
  const runtime::Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return result;
  }

  float bestToi = 1.0F;
  bool anyHit = false;
  math::Vec3 bestNormal(0.0F, 1.0F, 0.0F);
  math::Vec3 bestContactPt{};
  std::uint32_t bestHitEntity = 0U;

  for (std::size_t i = 0U; i < count; ++i) {
    if (entities[i] == entity) {
      continue;
    }

    // Collision layer/mask filtering.
    const runtime::Collider &other = colliders[i];
    if (((collider.collisionLayer & other.collisionMask) == 0U) ||
        ((other.collisionLayer & collider.collisionMask) == 0U)) {
      continue;
    }

    runtime::Transform otherTransform{};
    if (!world.get_transform(entities[i], &otherTransform)) {
      continue;
    }

    // Get the other body's velocity (might be moving too).
    const runtime::RigidBody *otherBody =
        world.get_rigid_body_ptr(entities[i]);
    const math::Vec3 otherVel =
        (otherBody != nullptr) ? otherBody->velocity
                               : math::Vec3(0.0F, 0.0F, 0.0F);

    // Relative velocity of moving body w.r.t. other.
    const math::Vec3 relVel = math::sub(body.velocity, otherVel);
    const float relSpeed = math::length(relVel);
    if (relSpeed < 1e-6F) {
      continue;
    }

    // Quick conservative AABB sweep test: check if the swept AABB of the
    // moving body intersects the other's AABB at all.
    const math::Vec3 otherHe = broadphase_half_extents_ccd(other);
    const math::Vec3 endPos =
        math::add(transform.position, math::mul(relVel, dt));

    // Compute axis-aligned bounding box of the swept path.
    const math::Vec3 sweepMin(
        std::min(transform.position.x - movingHe.x, endPos.x - movingHe.x),
        std::min(transform.position.y - movingHe.y, endPos.y - movingHe.y),
        std::min(transform.position.z - movingHe.z, endPos.z - movingHe.z));
    const math::Vec3 sweepMax(
        std::max(transform.position.x + movingHe.x, endPos.x + movingHe.x),
        std::max(transform.position.y + movingHe.y, endPos.y + movingHe.y),
        std::max(transform.position.z + movingHe.z, endPos.z + movingHe.z));

    // Does swept AABB overlap with static AABB of other object?
    if (sweepMin.x > otherTransform.position.x + otherHe.x ||
        sweepMax.x < otherTransform.position.x - otherHe.x ||
        sweepMin.y > otherTransform.position.y + otherHe.y ||
        sweepMax.y < otherTransform.position.y - otherHe.y ||
        sweepMin.z > otherTransform.position.z + otherHe.z ||
        sweepMax.z < otherTransform.position.z - otherHe.z) {
      continue; // No possible intersection, skip.
    }

    // -----------------------------------------------------------------------
    // Bilateral Advancement (Erwin Coumans, GDC 2013)
    // -----------------------------------------------------------------------
    // We sweep body A from t=0 to t=1 (in dt-normalised time).
    // At each iteration we compute the separating distance between A(t) and B.
    // We advance t by sep_dist / relSpeed (conservative bound).
    // We stop when either:
    //   - sep_dist <= tolerance (contact found at current t)
    //   - t >= bestToi (a closer hit already found)
    //   - iterations exhausted
    // -----------------------------------------------------------------------

    float tLo = 0.0F;
    float tHi = bestToi;
    bool foundContact = false;

    for (int iter = 0; iter < kMaxBilateralIterations; ++iter) {
      // Position of A at time tLo.
      const math::Vec3 posA =
          math::add(transform.position, math::mul(relVel, tLo * dt));
      // Position of B (assumed stationary in relative frame at otherPos).
      const math::Vec3 &posB = otherTransform.position;

      const float sep = separating_distance(collider, posA, other, posB);

      if (sep <= kTolerance) {
        // Contact (or overlap) at tLo.
        foundContact = true;
        break;
      }

      // Advance conservatively: sep / relSpeed gives time to close the gap,
      // but we normalise by dt to get t-fraction.
      const float advance = sep / (relSpeed * dt);
      tLo += advance;

      if (tLo >= tHi) {
        break; // Passed beyond best known TOI or end of step.
      }
    }

    if (foundContact && (tLo < bestToi)) {
      bestToi = tLo;
      anyHit = true;

      const math::Vec3 hitPosA =
          math::add(transform.position, math::mul(relVel, tLo * dt));
      bestNormal =
          contact_normal_between(collider, hitPosA, other,
                                 otherTransform.position);
      bestContactPt = math::mul(
          math::add(hitPosA, otherTransform.position), 0.5F);
      bestHitEntity = entities[i].index;
    }
  }

  if (anyHit) {
    result.hit = true;
    result.timeOfImpact = std::max(0.0F, bestToi);
    result.contactNormal = bestNormal;
    result.contactPoint = bestContactPt;
    result.hitEntityIndex = bestHitEntity;
  }

  return result;
}

} // namespace engine::physics
