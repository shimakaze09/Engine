// Implements ccd behavior for the Engine physics system.

#include "engine/physics/ccd.h"

#include "engine/core/cvar.h"
#include "engine/math/aabb.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

// ---------------------------------------------------------------------------
// physics.ccd_threshold CVar is registered by register_physics_cvars().
// Bodies slower than this skip CCD entirely.
// ---------------------------------------------------------------------------
namespace {

constexpr int kMaxBilateralIterations = 32;
constexpr float kTolerance = 1e-4F;

/// Clamps a raw physics.ccd_threshold value: non-finite or negative values
/// fall back to the registered default.
float validated_ccd_threshold(float raw) noexcept {
  return (std::isfinite(raw) && (raw >= 0.0F)) ? raw : 2.0F;
}

/// Returns positive axis separation, or a non-positive overlap measure.
float aabb_separating_distance(const math::AABB &a,
                               const math::AABB &b) noexcept {
  const float dx = std::max(b.min.x - a.max.x, a.min.x - b.max.x);
  const float dy = std::max(b.min.y - a.max.y, a.min.y - b.max.y);
  const float dz = std::max(b.min.z - a.max.z, a.min.z - b.max.z);
  return std::max({dx, dy, dz});
}

/// Builds parent-aware geometry, falling back to the supplied local transform.
bool build_geometry(const PhysicsWorldView &world, Entity entity,
                    const Collider &collider, const Transform *fallback,
                    ColliderWorldGeometry *outGeometry) noexcept {
  PhysicsTransform transform{};
  math::Mat4 worldMatrix{};
  if (world.get_physics_transform(entity, &transform)) {
    worldMatrix = transform.matrix;
  } else if (fallback != nullptr) {
    worldMatrix = math::compose_trs(fallback->position, fallback->rotation,
                                    fallback->scale);
  } else {
    return false;
  }
  const ConvexHullData *hull = nullptr;
  if (collider.shape == ColliderShape::ConvexHull) {
    hull = get_hull_data_ptr(world.physics_context(), entity);
  }
  return make_collider_world_geometry(collider, worldMatrix, hull, outGeometry);
}

/// Translates cached geometry in world space without rebuilding local shape
/// data.
ColliderWorldGeometry
translated_geometry(const ColliderWorldGeometry &source,
                    const math::Vec3 &translation) noexcept {
  ColliderWorldGeometry result = source;
  result.localToWorld.columns[3].x += translation.x;
  result.localToWorld.columns[3].y += translation.y;
  result.localToWorld.columns[3].z += translation.z;
  result.center = math::add(result.center, translation);
  result.worldAabb.min = math::add(result.worldAabb.min, translation);
  result.worldAabb.max = math::add(result.worldAabb.max, translation);
  return result;
}

/// Adapts world geometry to the fixed GJK support-function signature.
math::Vec3 support_geometry(const void *data, const math::Vec3 &,
                            const math::Vec3 &direction) noexcept {
  return collider_support_point(
      *static_cast<const ColliderWorldGeometry *>(data), direction);
}

/// Runs exact convex intersection for one transformed CCD candidate.
GjkResult geometry_intersection(const ColliderWorldGeometry &a,
                                const ColliderWorldGeometry &b) noexcept {
  return gjk_epa(&a, a.center, &support_geometry, &b, b.center,
                 &support_geometry);
}

/// Produces the response normal from target B toward moving collider A.
math::Vec3 contact_normal(const ColliderWorldGeometry &a,
                          const ColliderWorldGeometry &b,
                          const GjkResult &intersection) noexcept {
  if (math::length_sq(intersection.normal) > 1.0e-12F) {
    return math::normalize(math::mul(intersection.normal, -1.0F));
  }
  const math::Vec3 delta = math::sub(a.center, b.center);
  const float len = math::length(delta);
  if (len > 1.0e-8F) {
    return math::div(delta, len);
  }
  return math::Vec3(0.0F, 1.0F, 0.0F);
}

/// Approximates the shared boundary point from opposing support points.
math::Vec3 contact_point(const ColliderWorldGeometry &a,
                         const ColliderWorldGeometry &b,
                         const math::Vec3 &normal) noexcept {
  const math::Vec3 onA = collider_support_point(a, math::mul(normal, -1.0F));
  const math::Vec3 onB = collider_support_point(b, normal);
  return math::mul(math::add(onA, onB), 0.5F);
}

} // namespace

float ccd_velocity_threshold() noexcept {
  return validated_ccd_threshold(
      core::cvar_get_float("physics.ccd_threshold", 2.0F));
}

/// Bilateral advancement CCD (Erwin Coumans, GDC 2013): sweeps the moving
/// collider through dt-normalized time, advancing by conservative separation
/// bounds until contact or the current-best TOI. Only fast movers sweep —
/// the per-step cached physics.ccd_threshold value gates speed (the global
/// cvar mutex must never be taken inside parallel chunk jobs), and the
/// step's travel must exceed half the collider's smallest extent.
/// Candidates are gated on the previous resolve's snapshot, matched by
/// ENTITY IDENTITY through the published entity-index -> slot map (slot in
/// range and stored entity equal in index AND generation), so sparse-set
/// reorders between the publish and this sweep cannot mismatch entries:
/// snapshot AABBs (expanded by one step of positional correction drift)
/// reject most pairs before the expensive geometry build, and candidate
/// velocities come from the snapshot because reading live
/// RigidBody::velocity races with the parallel integration chunks. When
/// no snapshot entry vouches for a candidate (first step after play start
/// or scene load, collider added since the last resolve) that body is
/// treated as static — never read live — which is deterministic and
/// conservative: the speculative-contact path still catches the encounter
/// on the next resolved step. A pair missed because the OTHER body races
/// toward a slow mover is covered by that body's own sweep.
CcdSweepResult bilateral_advance_ccd(const PhysicsWorldView &world,
                                     Entity entity, const RigidBody &body,
                                     const Collider &collider,
                                     const Transform &transform,
                                     float dt) noexcept {

  CcdSweepResult result{};

  if (!std::isfinite(dt) || (dt <= 0.0F)) {
    return result;
  }

  ColliderWorldGeometry movingGeometry{};
  if (!build_geometry(world, entity, collider, &transform, &movingGeometry)) {
    return result;
  }

  const float speed = math::length(body.velocity);
  if (speed < 1e-6F) {
    return result;
  }

  const PhysicsContext &physicsCtx = world.physics_context();
  const float threshold = validated_ccd_threshold(physicsCtx.ccdThresholdCvar);
  if (speed < threshold) {
    return result;
  }

  const float travelDist = speed * dt;

  const math::Vec3 movingHe = math::aabb_half_extents(movingGeometry.worldAabb);
  const float minHalf = std::min({movingHe.x, movingHe.y, movingHe.z});
  if (travelDist <= minHalf * 0.5F) {
    return result;
  }

  const std::size_t count = world.collider_count();
  if (count == 0U) {
    return result;
  }
  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if (!world.get_collider_range(0U, count, &entities, &colliders)) {
    return result;
  }

  float bestToi = 1.0F;
  bool anyHit = false;
  math::Vec3 bestNormal(0.0F, 1.0F, 0.0F);
  math::Vec3 bestContactPt{};
  std::uint32_t bestHitEntity = 0U;
  math::Vec3 bestOtherVel(0.0F, 0.0F, 0.0F);
  Entity bestOtherOwner = kInvalidEntity;
  float bestOtherRestitution = 0.0F;
  const Entity movingOwner = world.rigid_body_owner(entity);

  const PhysicsShapeStore *snapshotStore = physicsCtx.shapeStore.get();
  const std::size_t snapshotCount = physicsCtx.ccdColliderCount;
  constexpr std::uint32_t kNoSnapshotSlot = 0xFFFFFFFFU;

  constexpr float kSnapshotDriftSlop = 0.1F;
  const math::Vec3 gateDisplacement = math::mul(body.velocity, dt);
  math::AABB gateBounds = movingGeometry.worldAabb;
  gateBounds.min.x += std::min(gateDisplacement.x, 0.0F) - kSnapshotDriftSlop;
  gateBounds.min.y += std::min(gateDisplacement.y, 0.0F) - kSnapshotDriftSlop;
  gateBounds.min.z += std::min(gateDisplacement.z, 0.0F) - kSnapshotDriftSlop;
  gateBounds.max.x += std::max(gateDisplacement.x, 0.0F) + kSnapshotDriftSlop;
  gateBounds.max.y += std::max(gateDisplacement.y, 0.0F) + kSnapshotDriftSlop;
  gateBounds.max.z += std::max(gateDisplacement.z, 0.0F) + kSnapshotDriftSlop;

  for (std::size_t i = 0U; i < count; ++i) {
    if (entities[i] == entity) {
      continue;
    }

    std::uint32_t snapshotSlot = kNoSnapshotSlot;
    if ((snapshotStore != nullptr) &&
        (entities[i].index < snapshotStore->ccdSlotByEntityIndex.size())) {
      const std::uint32_t slot =
          snapshotStore->ccdSlotByEntityIndex[entities[i].index];
      if ((slot < snapshotCount) &&
          (snapshotStore->ccdColliderEntities[slot] == entities[i])) {
        snapshotSlot = slot;
      }
    }

    if (snapshotSlot != kNoSnapshotSlot) {
      const math::AABB &otherBounds =
          snapshotStore->ccdColliderAabbs[snapshotSlot];
      if ((gateBounds.min.x > otherBounds.max.x) ||
          (gateBounds.max.x < otherBounds.min.x) ||
          (gateBounds.min.y > otherBounds.max.y) ||
          (gateBounds.max.y < otherBounds.min.y) ||
          (gateBounds.min.z > otherBounds.max.z) ||
          (gateBounds.max.z < otherBounds.min.z)) {
        continue;
      }
    }

    const Collider &other = colliders[i];
    if (((collider.collisionLayer & other.collisionMask) == 0U) ||
        ((other.collisionLayer & collider.collisionMask) == 0U)) {
      continue;
    }

    const Entity otherOwner =
        (snapshotSlot != kNoSnapshotSlot)
            ? snapshotStore->ccdColliderOwners[snapshotSlot]
            : world.rigid_body_owner(entities[i]);
    if ((movingOwner != kInvalidEntity) && (movingOwner == otherOwner)) {
      continue;
    }

    math::Vec3 otherVel(0.0F, 0.0F, 0.0F);
    if (snapshotSlot != kNoSnapshotSlot) {
      otherVel = snapshotStore->ccdColliderVelocities[snapshotSlot];
    }
    const math::Vec3 relVel = math::sub(body.velocity, otherVel);
    const float relSpeed = math::length(relVel);
    if (relSpeed < 1e-6F) {
      continue;
    }

    ColliderWorldGeometry otherGeometry{};
    if (!build_geometry(world, entities[i], other, nullptr, &otherGeometry)) {
      continue;
    }

    const math::Vec3 relativeDisplacement = math::mul(relVel, dt);
    const ColliderWorldGeometry endGeometry =
        translated_geometry(movingGeometry, relativeDisplacement);
    const math::Vec3 sweepMin(
        std::min(movingGeometry.worldAabb.min.x, endGeometry.worldAabb.min.x),
        std::min(movingGeometry.worldAabb.min.y, endGeometry.worldAabb.min.y),
        std::min(movingGeometry.worldAabb.min.z, endGeometry.worldAabb.min.z));
    const math::Vec3 sweepMax(
        std::max(movingGeometry.worldAabb.max.x, endGeometry.worldAabb.max.x),
        std::max(movingGeometry.worldAabb.max.y, endGeometry.worldAabb.max.y),
        std::max(movingGeometry.worldAabb.max.z, endGeometry.worldAabb.max.z));

    if (sweepMin.x > otherGeometry.worldAabb.max.x ||
        sweepMax.x < otherGeometry.worldAabb.min.x ||
        sweepMin.y > otherGeometry.worldAabb.max.y ||
        sweepMax.y < otherGeometry.worldAabb.min.y ||
        sweepMin.z > otherGeometry.worldAabb.max.z ||
        sweepMax.z < otherGeometry.worldAabb.min.z) {
      continue;
    }

    float tLo = 0.0F;
    bool foundContact = false;
    ColliderWorldGeometry contactGeometry{};
    GjkResult contactIntersection{};
    const math::Vec3 motionDirection = math::div(relVel, relSpeed);

    for (int iter = 0; iter < kMaxBilateralIterations; ++iter) {
      const ColliderWorldGeometry candidateGeometry = translated_geometry(
          movingGeometry, math::mul(relativeDisplacement, tLo));
      const GjkResult intersection =
          geometry_intersection(candidateGeometry, otherGeometry);
      if (intersection.intersecting) {
        foundContact = true;
        contactGeometry = candidateGeometry;
        contactIntersection = intersection;
        break;
      }

      const float boundsSeparation = aabb_separating_distance(
          candidateGeometry.worldAabb, otherGeometry.worldAabb);
      const math::Vec3 movingFront =
          collider_support_point(candidateGeometry, motionDirection);
      const math::Vec3 targetBack = collider_support_point(
          otherGeometry, math::mul(motionDirection, -1.0F));
      const float projectedSeparation =
          math::dot(math::sub(targetBack, movingFront), motionDirection);
      const float separation = std::max(boundsSeparation, projectedSeparation);
      const float minimumAdvance =
          std::max(kTolerance / (relSpeed * dt), 1.0e-5F);
      const float advance =
          separation > kTolerance
              ? std::max(separation / (relSpeed * dt), minimumAdvance)
              : minimumAdvance;
      tLo += advance;

      if (tLo >= bestToi) {
        break;
      }
    }

    if (foundContact && (tLo < bestToi)) {
      bestToi = tLo;
      anyHit = true;
      bestNormal =
          contact_normal(contactGeometry, otherGeometry, contactIntersection);
      bestContactPt = contact_point(contactGeometry, otherGeometry, bestNormal);
      bestHitEntity = entities[i].index;
      bestOtherVel = otherVel;
      bestOtherOwner = otherOwner;
      bestOtherRestitution = other.restitution;
    }
  }

  if (anyHit) {
    result.hit = true;
    result.timeOfImpact = std::max(0.0F, bestToi);
    result.contactNormal = bestNormal;
    result.contactPoint = bestContactPt;
    result.hitEntityIndex = bestHitEntity;
    result.targetVelocity = bestOtherVel;
    result.combinedRestitution =
        std::max(collider.restitution, bestOtherRestitution);
    // inverseMass is Input-phase-only state, safe to read beside the
    // parallel chunk jobs that only write velocities.
    const RigidBody *otherBody = (bestOtherOwner != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(bestOtherOwner)
                                     : nullptr;
    result.targetInverseMass =
        (otherBody != nullptr) ? otherBody->inverseMass : 0.0F;
    result.targetRespondsInCcd = (result.targetInverseMass > 0.0F) &&
                                 (math::length(bestOtherVel) >= threshold);
  }

  return result;
}

} // namespace engine::physics
