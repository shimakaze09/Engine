// Declares physics query types and APIs for the Engine physics system.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/physics_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::physics {

class PhysicsWorldView;

// ------ Sweep Result --------------------------------------------------------

struct SweepHit final {
  std::uint32_t entityIndex = 0U;
  math::Vec3 contactPoint{};
  math::Vec3 normal{};
  float distance = 0.0F;
  float timeOfImpact = 0.0F;
};

// ------ Query Functions------------------------------------------------------

/// Returns the nearest ray intersection while optionally skipping one entity.
bool raycast(const PhysicsWorldView &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance,
             PhysicsRaycastHit *outHit,
             Entity skipEntity = kInvalidEntity) noexcept;

/// Returns the nearest maxHits ray intersections sorted by distance.
/// Direction is normalized internally; maxDistance must be finite and
/// positive. Respects the collision mask.
std::size_t raycast_all(const PhysicsWorldView &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        PhysicsRaycastHit *outHits, std::size_t maxHits,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

// Overlap queries — return entity indices.
std::size_t overlap_sphere(const PhysicsWorldView &world,
                           const math::Vec3 &center, float radius,
                           std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Collects entity indices overlapping the AABB (mask-filtered);
/// returns the count.
std::size_t overlap_box(const PhysicsWorldView &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

/// Sweeps a sphere along a normalized copy of direction.
/// Returns the earliest hit when maxDistance is finite and positive.
/// skipEntity excludes that entity's colliders and every collider whose
/// rigid-body owner it is (compound bodies are skipped as one unit).
bool sweep_sphere(const PhysicsWorldView &world, const math::Vec3 &origin,
                  float radius, const math::Vec3 &direction, float maxDistance,
                  SweepHit *outHit, std::uint32_t mask = 0xFFFFFFFFU,
                  Entity skipEntity = kInvalidEntity) noexcept;

/// Sweeps an AABB along a normalized copy of direction.
/// Returns the earliest hit when maxDistance is finite and positive.
bool sweep_box(const PhysicsWorldView &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, SweepHit *outHit,
               std::uint32_t mask = 0xFFFFFFFFU) noexcept;

} // namespace engine::physics
