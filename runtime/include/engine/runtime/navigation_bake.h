// Declares the navigation mesh bake over a World: the level an agent walks
// is the World's static colliders (a collider whose entity and ancestors
// carry no rigid body, or a Static one), sampled column by column with the
// physics world's own ray queries, so every collider shape the physics
// queries understand bakes the same way it collides.

#pragma once

#include "engine/navigation/nav_mesh.h"

#include "engine/math/vec3.h"
#include "engine/runtime/world_component_types.h"

namespace engine::runtime {

class World;

/// Bakes the static colliders of `world` inside the settings' bounds into
/// `out`. Dynamic and kinematic bodies and triggers are not part of the
/// level. False, logged, with `out` unchanged, for invalid settings or a
/// failed allocation.
bool bake_navigation_mesh(const World &world,
                          const navigation::NavBakeSettings &settings,
                          navigation::NavMesh *out) noexcept;

/// The bake settings a surface's volume makes around `center`.
navigation::NavBakeSettings
nav_bake_settings_for(const NavMeshSurfaceComponent &surface,
                      const math::Vec3 &center) noexcept;

/// Bakes the NavMeshSurface on `entity` over `world`: its volume around
/// the entity's world position, for its agent. False, logged, with `out`
/// unchanged, for an entity without a surface or a failed bake.
bool bake_nav_mesh_surface(const World &world, Entity entity,
                           navigation::NavMesh *out) noexcept;

} // namespace engine::runtime
