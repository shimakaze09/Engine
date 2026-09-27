// Declares the canonical collider description for the engine's built-in
// primitive shapes, so every spawn path (the editor's Create menu, Lua's
// engine.spawn_shape) installs the same collider, hull provenance
// included, from the runtime tier that owns collider installation instead
// of describing one of its own.

#pragma once

#include "engine/runtime/world_component_types.h"

namespace engine::runtime {

/// Applies the canonical convex hull that `source` names to `collider`: the
/// ConvexHull shape, the provenance tag `World::add_collider` rebuilds the
/// payload from, and the builder's own local half extents. Returns false and
/// leaves `collider` untouched when the source names no primitive hull or its
/// builder rejects it, so the caller's authored fallback shape stands.
bool apply_primitive_hull(HullSource source, Collider *collider) noexcept;

/// The collider every spawn path gives `shape`: its box, sphere or capsule
/// extents, or its canonical convex hull where it has one (a builder that
/// rejects leaves the authored fallback, a capsule for a cylinder and a
/// box for a pyramid). A plane's thin box sits wholly below its surface,
/// so its top is the ground that is drawn. Material and filter fields keep
/// Collider's defaults.
Collider primitive_collider(PrimitiveShape shape) noexcept;

} // namespace engine::runtime
