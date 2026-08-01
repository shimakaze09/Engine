// Declares the blocked-body warning diagnostic hooks resolve_collisions
// calls at its entry and exit to detect velocity-driven bodies whose
// achieved displacement persistently falls far short of their commanded
// velocity (a script driving a body into blocking geometry).

#pragma once

#include "engine/physics/physics_world_view.h"

namespace engine::physics {

/// Captures each awake dynamic body's pre-solve speed as this step's
/// commanded speed. Call at resolve_collisions entry, before any solver
/// stage modifies velocities.
void capture_blocked_body_commands(PhysicsWorldView &world) noexcept;

/// Compares each captured commanded speed against the body's achieved
/// read-to-write transform displacement, counts consecutive blocked steps,
/// and logs one warning per blocking episode once the
/// physics.blocked_warn_steps threshold is reached. Call at
/// resolve_collisions exit, after positional correction has settled.
void report_blocked_bodies(PhysicsWorldView &world,
                           float deltaSeconds) noexcept;

} // namespace engine::physics
