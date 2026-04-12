#pragma once

#include "engine/math/vec3.h"

#include <cstddef>
#include <cstdint>

namespace engine::runtime {
class World;
struct PhysicsRaycastHit;
} // namespace engine::runtime

namespace engine::physics {

// ------ Sweep Result --------------------------------------------------------

struct SweepHit final {
  std::uint32_t entityIndex = 0U;
  math::Vec3 contactPoint{};
  math::Vec3 normal{};
  float distance = 0.0F;
  float timeOfImpact = 0.0F;
};

// ------ Query Functions------------------------------------------------------

// Raycast returning all hits, sorted by distance.  Respects collision mask.
std::size_t raycast_all(const runtime::World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        runtime::PhysicsRaycastHit *outHits,
                        std::size_t maxHits,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

// Overlap queries — return entity indices.
std::size_t overlap_sphere(const runtime::World &world,
                           const math::Vec3 &center, float radius,
                           std::uint32_t *outEntityIndices,
                           std::size_t maxResults,
                           std::uint32_t mask = 0xFFFFFFFFU) noexcept;

std::size_t overlap_box(const runtime::World &world, const math::Vec3 &center,
                        const math::Vec3 &halfExtents,
                        std::uint32_t *outEntityIndices, std::size_t maxResults,
                        std::uint32_t mask = 0xFFFFFFFFU) noexcept;

// Sweep queries — move a shape along a direction and report first collision.
bool sweep_sphere(const runtime::World &world, const math::Vec3 &origin,
                  float radius, const math::Vec3 &direction, float maxDistance,
                  SweepHit *outHit, std::uint32_t mask = 0xFFFFFFFFU) noexcept;

bool sweep_box(const runtime::World &world, const math::Vec3 &center,
               const math::Vec3 &halfExtents, const math::Vec3 &direction,
               float maxDistance, SweepHit *outHit,
               std::uint32_t mask = 0xFFFFFFFFU) noexcept;

} // namespace engine::physics
