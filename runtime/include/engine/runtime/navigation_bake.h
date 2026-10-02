// Declares the navigation mesh bake over a World: the level an agent walks
// is the World's static colliders (a collider whose entity and ancestors
// carry no rigid body, or a Static one), sampled column by column with the
// physics world's own ray queries, so every collider shape the physics
// queries understand bakes the same way it collides.

#pragma once

#include "engine/navigation/nav_mesh.h"

namespace engine::runtime {

class World;

/// Bakes the static colliders of `world` inside the settings' bounds into
/// `out`. Dynamic and kinematic bodies and triggers are not part of the
/// level. False, logged, with `out` unchanged, for invalid settings or a
/// failed allocation.
bool bake_navigation_mesh(const World &world,
                          const navigation::NavBakeSettings &settings,
                          navigation::NavMesh *out) noexcept;

} // namespace engine::runtime
