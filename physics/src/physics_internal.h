// Declares module-private helpers shared across the physics TUs: shape
// payload lookups and the tuning values used by more than one stage.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/physics_context.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace engine::physics {

// Broadphase spatial-hash shape shared by the resolve scratch below and
// physics.cpp's grid passes (moved from physics.cpp for #170).
constexpr std::size_t kSpatialHashBuckets = 4096U;

// Linked-list node for the broadphase spatial hash grid.
struct SpatialNode final {
  std::uint32_t colliderIdx;
  std::uint32_t next;
};

// Max spatial-hash entries: each collider may touch up to 8 cells (the
// corners of its AABB).
constexpr std::size_t kMaxNodes = kMaxColliders * 8U;

// Scratch buffers for resolve_collisions (~19 MB). Heap-backed and owned
// by the PhysicsContext (#170) — never a plain thread_local array: ~19 MB
// of static TLS is carved out of every new thread's stack allocation on
// glibc, which starves threads created with small explicit stacks (Mesa's
// GL driver workers overflowed and crashed the editor on startup exactly
// that way), and per-thread heap ownership retained one block in every
// worker that ever ran the resolve job.
struct ResolveScratch final {
  std::array<ColliderWorldGeometry, kMaxColliders> geometries{};
  std::array<Entity, kMaxColliders> bodyOwners{};
  std::array<engine::math::Vec3, kMaxColliders> bodyCenters{};
  std::array<bool, kMaxColliders> geometryValid{};
  std::array<float, kMaxColliders> posX{};
  std::array<float, kMaxColliders> posY{};
  std::array<float, kMaxColliders> posZ{};
  std::array<std::uint32_t, kSpatialHashBuckets> buckets{};
  std::array<SpatialNode, kMaxNodes> nodes{};
  std::array<float, kMaxColliders> expandX{};
  std::array<float, kMaxColliders> expandY{};
  std::array<float, kMaxColliders> expandZ{};
  std::array<std::uint32_t, kMaxColliders> overflowList{};
  std::array<bool, kMaxColliders> isOverflow{};
};

/// Bodies below this energy for kSleepFramesRequired frames go to sleep;
/// contacts wake a sleeper only when the other body exceeds it.
constexpr float kSleepThreshold = 0.01F;

/// Restitution acts only above this approach speed (m/s): slow pushing or
/// resting contacts absorb fully, shared by discrete and CCD responses.
constexpr float kRestitutionSpeedThreshold = 1.0F;

/// Sign of value, treating zero as positive.
inline float sign_or_positive(float value) noexcept {
  return (value < 0.0F) ? -1.0F : 1.0F;
}

/// Closest point on line segment AB to point P; returns the clamped [0,1]
/// parameter. Shared by narrow_phase's axis-aligned capsule paths and ccd's
/// rotation-aware capsule normals so both stay exact against the same math.
inline float
closest_point_on_segment(const math::Vec3 &a, const math::Vec3 &b,
                         const math::Vec3 &p, math::Vec3 &outClosest) noexcept {
  const math::Vec3 ab = math::sub(b, a);
  const float ab2 = math::dot(ab, ab);
  if (ab2 < 1e-12F) {
    outClosest = a;
    return 0.0F;
  }
  float t = math::dot(math::sub(p, a), ab) / ab2;
  t = std::max(0.0F, std::min(1.0F, t));
  outClosest = math::add(a, math::mul(ab, t));
  return t;
}

/// Closest points between two line segments (P0-P1 and Q0-Q1); returns the
/// squared distance between them. See closest_point_on_segment for sharing
/// rationale.
inline float
closest_point_segment_segment(const math::Vec3 &p0, const math::Vec3 &p1,
                              const math::Vec3 &q0, const math::Vec3 &q1,
                              math::Vec3 &outClosestP,
                              math::Vec3 &outClosestQ) noexcept {
  const math::Vec3 d1 = math::sub(p1, p0);
  const math::Vec3 d2 = math::sub(q1, q0);
  const math::Vec3 r = math::sub(p0, q0);
  const float a = math::dot(d1, d1);
  const float e = math::dot(d2, d2);
  const float f = math::dot(d2, r);

  float s = 0.0F;
  float t = 0.0F;

  if (a <= 1e-12F && e <= 1e-12F) {
    outClosestP = p0;
    outClosestQ = q0;
    const math::Vec3 diff = math::sub(outClosestP, outClosestQ);
    return math::dot(diff, diff);
  }

  if (a <= 1e-12F) {
    s = 0.0F;
    t = std::max(0.0F, std::min(f / e, 1.0F));
  } else {
    const float c = math::dot(d1, r);
    if (e <= 1e-12F) {
      t = 0.0F;
      s = std::max(0.0F, std::min(-c / a, 1.0F));
    } else {
      const float b = math::dot(d1, d2);
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

  outClosestP = math::add(p0, math::mul(d1, s));
  outClosestQ = math::add(q0, math::mul(d2, t));
  const math::Vec3 diff = math::sub(outClosestP, outClosestQ);
  return math::dot(diff, diff);
}

/// Entity's convex-hull payload slot, or nullptr when none is set.
ConvexHullData *find_hull_data(PhysicsContext &context,
                               Entity entity) noexcept;
/// Const overload of find_hull_data.
const ConvexHullData *find_hull_data(const PhysicsContext &context,
                                     Entity entity) noexcept;
/// Entity's heightfield payload slot, or nullptr when none is set.
HeightfieldData *find_heightfield_data(PhysicsContext &context,
                                       Entity entity) noexcept;
/// Const overload of find_heightfield_data.
const HeightfieldData *find_heightfield_data(const PhysicsContext &context,
                                             Entity entity) noexcept;

} // namespace engine::physics
