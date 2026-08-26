// Declares the canonical collider description for the engine's built-in
// primitive shapes (#310), so a spawn path derives its hull provenance from
// the runtime tier that owns collider installation instead of building
// physics payloads of its own.

#pragma once

#include "engine/runtime/world_component_types.h"

namespace engine::runtime {

/// Applies the canonical convex hull that `source` names to `collider`: the
/// ConvexHull shape, the provenance tag `World::add_collider` rebuilds the
/// payload from, and the builder's own local half extents. Returns false and
/// leaves `collider` untouched when the source names no primitive hull or its
/// builder rejects it, so the caller's authored fallback shape stands.
bool apply_primitive_hull(HullSource source, Collider *collider) noexcept;

} // namespace engine::runtime
