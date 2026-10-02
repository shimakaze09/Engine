// Implements what a NavMeshSurface component means for a bake: whether
// its settings can bake, the bake settings its volume makes around its
// entity, and the bake of one surface over its World.

#include "engine/content/asset_type_table.h"
#include "engine/runtime/navigation_bake.h"
#include "engine/runtime/world.h"

#include <cmath>

namespace engine::runtime {

namespace {

bool finite_positive(float value) noexcept {
  return std::isfinite(value) && (value > 0.0F);
}

} // namespace

navigation::NavBakeSettings
nav_bake_settings_for(const NavMeshSurfaceComponent &surface,
                      const math::Vec3 &center) noexcept {
  navigation::NavBakeSettings settings{};
  settings.boundsMin = math::sub(center, surface.halfExtents);
  settings.boundsMax = math::add(center, surface.halfExtents);
  settings.cellSize = surface.cellSize;
  settings.agentRadius = surface.agentRadius;
  settings.agentHeight = surface.agentHeight;
  settings.maxClimb = surface.maxClimb;
  settings.maxSlopeDegrees = surface.maxSlopeDegrees;
  return settings;
}

bool nav_mesh_surface_is_valid(
    const NavMeshSurfaceComponent &surface) noexcept {
  if (!finite_positive(surface.halfExtents.x) ||
      !finite_positive(surface.halfExtents.y) ||
      !finite_positive(surface.halfExtents.z)) {
    return false;
  }
  // The volume around the origin: where the entity stands moves the box,
  // never how many columns it holds.
  if (!navigation::nav_bake_settings_are_valid(
          nav_bake_settings_for(surface, math::Vec3(0.0F, 0.0F, 0.0F)))) {
    return false;
  }
  if (surface.navMeshPath[0] == '\0') {
    return true;
  }
  const content::AssetClassification kind =
      content::classify_asset_path(surface.navMeshPath);
  return (kind.tag == content::AssetTypeTag::NavMesh) && kind.source;
}

bool bake_nav_mesh_surface(const World &world, Entity entity,
                           navigation::NavMesh *out) noexcept {
  NavMeshSurfaceComponent surface{};
  if ((out == nullptr) || !world.get_nav_mesh_surface(entity, &surface)) {
    return false;
  }
  // The composed world position when the hierarchy has been propagated;
  // an entity not yet propagated stands where its own transform puts it.
  math::Vec3 center = math::Vec3(0.0F, 0.0F, 0.0F);
  const WorldTransform *placed = world.get_world_transform_read_ptr(entity);
  Transform local{};
  if (placed != nullptr) {
    center = placed->position;
  } else if (world.get_transform(entity, &local)) {
    center = local.position;
  }
  return bake_navigation_mesh(world, nav_bake_settings_for(surface, center),
                              out);
}

} // namespace engine::runtime
