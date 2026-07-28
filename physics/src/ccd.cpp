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
  return core::cvar_get_float("physics.ccd_threshold", 2.0F);
}

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

  // Only trigger CCD if speed exceeds threshold.
  const float threshold = ccd_velocity_threshold();
  if (speed < threshold) {
    return result;
  }

  // The body will travel this far in dt.
  const float travelDist = speed * dt;

  // Minimum half-extent of the moving shape — if travel < this, skip CCD.
  const math::Vec3 movingHe = math::aabb_half_extents(movingGeometry.worldAabb);
  const float minHalf = std::min({movingHe.x, movingHe.y, movingHe.z});
  if (travelDist <= minHalf * 0.5F) {
    return result; // Travel distance too small relative to collider size.
  }

  // Iterate over all colliders in the world, find earliest TOI.
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
  const Entity movingOwner = world.rigid_body_owner(entity);

  // The previous resolve's snapshot provides candidate bounds and ownership:
  // per-candidate hierarchy walks and geometry builds dominate the sweep
  // cost, so candidates are rejected on snapshot AABBs first.
  const PhysicsContext &physicsCtx = world.physics_context();
  const PhysicsShapeStore *snapshotStore = physicsCtx.shapeStore.get();
  const bool snapshotUsable =
      (snapshotStore != nullptr) && (physicsCtx.ccdColliderCount == count);

  // Conservative reject bounds: the mover's whole-step swept AABB expanded
  // by a slop covering one step of snapshot drift (positional corrections).
  // Pairs missed because the OTHER body races toward a slow mover are
  // covered by that body's own CCD sweep.
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

    if (snapshotUsable) {
      const math::AABB &otherBounds = snapshotStore->ccdColliderAabbs[i];
      if ((gateBounds.min.x > otherBounds.max.x) ||
          (gateBounds.max.x < otherBounds.min.x) ||
          (gateBounds.min.y > otherBounds.max.y) ||
          (gateBounds.max.y < otherBounds.min.y) ||
          (gateBounds.min.z > otherBounds.max.z) ||
          (gateBounds.max.z < otherBounds.min.z)) {
        continue;
      }
    }

    // Collision layer/mask filtering.
    const Collider &other = colliders[i];
    if (((collider.collisionLayer & other.collisionMask) == 0U) ||
        ((other.collisionLayer & collider.collisionMask) == 0U)) {
      continue;
    }

    const Entity otherOwner =
        (snapshotUsable &&
         (snapshotStore->ccdColliderEntities[i] == entities[i]))
            ? snapshotStore->ccdColliderOwners[i]
            : world.rigid_body_owner(entities[i]);
    if ((movingOwner != kInvalidEntity) && (movingOwner == otherOwner)) {
      continue;
    }

    // Reject on relative velocity BEFORE building world geometry: geometry
    // construction (transform composition + matrix inverse) is by far the
    // most expensive part of a candidate visit, and bodies moving together
    // can never produce a sweep hit.
    const RigidBody *otherBody = (otherOwner != kInvalidEntity)
                                     ? world.get_rigid_body_ptr(otherOwner)
                                     : nullptr;
    const math::Vec3 otherVel = (otherBody != nullptr)
                                    ? otherBody->velocity
                                    : math::Vec3(0.0F, 0.0F, 0.0F);
    const math::Vec3 relVel = math::sub(body.velocity, otherVel);
    const float relSpeed = math::length(relVel);
    if (relSpeed < 1e-6F) {
      continue;
    }

    ColliderWorldGeometry otherGeometry{};
    if (!build_geometry(world, entities[i], other, nullptr, &otherGeometry)) {
      continue;
    }

    // Quick conservative AABB sweep test: check if the swept AABB of the
    // moving body intersects the other's AABB at all.
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

    // Does swept AABB overlap with static AABB of other object?
    if (sweepMin.x > otherGeometry.worldAabb.max.x ||
        sweepMax.x < otherGeometry.worldAabb.min.x ||
        sweepMin.y > otherGeometry.worldAabb.max.y ||
        sweepMax.y < otherGeometry.worldAabb.min.y ||
        sweepMin.z > otherGeometry.worldAabb.max.z ||
        sweepMax.z < otherGeometry.worldAabb.min.z) {
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
        break; // Passed beyond best known TOI or end of step.
      }
    }

    if (foundContact && (tLo < bestToi)) {
      bestToi = tLo;
      anyHit = true;
      bestNormal =
          contact_normal(contactGeometry, otherGeometry, contactIntersection);
      bestContactPt = contact_point(contactGeometry, otherGeometry, bestNormal);
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
