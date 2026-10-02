// Implements the navigation mesh bake over a World: a column sampler that
// casts straight down through the bake's bounds with the physics ray
// queries, keeps the static surfaces it meets and measures the headroom
// above each with a ray straight up.

#include "engine/runtime/navigation_bake.h"

#include <cmath>

#include "engine/math/component_types.h"
#include "engine/physics/physics_query.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

/// The most colliders one column ray reports, and one headroom ray.
constexpr std::size_t kMaxColumnHits = 32U;
/// How far above a surface the headroom ray starts, so it does not meet
/// the surface it leaves.
constexpr float kHeadroomLift = 0.01F;

struct SamplerContext final {
  const World *world = nullptr;
  float top = 0.0F;
  float depth = 0.0F;
  float headroom = 0.0F;
};

/// True when the collider on `entity` is part of the level: no rigid body
/// owns it, or the one that does is Static.
bool is_static(const World &world, Entity entity) noexcept {
  const Entity owner = world.rigid_body_owner(entity);
  if (owner == kInvalidEntity) {
    return true;
  }
  const math::RigidBody *body = world.get_rigid_body_ptr(owner);
  return (body == nullptr) ||
         (math::body_type(*body) == math::BodyType::Static);
}

std::size_t sample_column(void *context, float x, float z,
                          navigation::NavSurface *out,
                          std::size_t capacity) noexcept {
  const auto &c = *static_cast<const SamplerContext *>(context);
  physics::PhysicsRaycastHit hits[kMaxColumnHits] = {};
  const std::size_t found = physics::raycast_all(
      *c.world, math::Vec3(x, c.top, z), math::Vec3(0.0F, -1.0F, 0.0F), c.depth,
      hits, kMaxColumnHits);
  std::size_t count = 0U;
  for (std::size_t i = 0U; (i < found) && (count < capacity); ++i) {
    const physics::PhysicsRaycastHit &hit = hits[i];
    if (!is_static(*c.world, hit.entity)) {
      continue;
    }
    // The headroom is the distance to the nearest static collider above;
    // past the agent's height it no longer matters.
    physics::PhysicsRaycastHit above[kMaxColumnHits] = {};
    const math::Vec3 from(hit.point.x, hit.point.y + kHeadroomLift,
                          hit.point.z);
    const std::size_t overhead =
        physics::raycast_all(*c.world, from, math::Vec3(0.0F, 1.0F, 0.0F),
                             c.headroom, above, kMaxColumnHits);
    float clearance = c.headroom + kHeadroomLift;
    for (std::size_t j = 0U; j < overhead; ++j) {
      if (is_static(*c.world, above[j].entity)) {
        clearance = above[j].distance + kHeadroomLift;
        break;
      }
    }
    navigation::NavSurface &surface = out[count++];
    surface.y = hit.point.y;
    surface.normalY = hit.normal.y;
    surface.clearance = clearance;
  }
  return count;
}

} // namespace

bool bake_navigation_mesh(const World &world,
                          const navigation::NavBakeSettings &settings,
                          navigation::NavMesh *out) noexcept {
  SamplerContext context{};
  context.world = &world;
  // The column ray starts just above the bounds, so a surface on their top
  // is met, and reaches just below them.
  context.top = settings.boundsMax.y + 1.0F;
  context.depth = (settings.boundsMax.y - settings.boundsMin.y) + 2.0F;
  context.headroom = settings.agentHeight;
  if (!std::isfinite(context.top) || !std::isfinite(context.depth) ||
      !(context.depth > 0.0F) || !std::isfinite(context.headroom) ||
      !(context.headroom > 0.0F)) {
    return navigation::bake_nav_mesh(settings, nullptr, nullptr, out);
  }
  return navigation::bake_nav_mesh(settings, &sample_column, &context, out);
}

} // namespace engine::runtime
