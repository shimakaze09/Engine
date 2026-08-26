// Declares the runtime-private map from a collider's hull provenance to the
// physics builder that produces its canonical payload, so the mapping exists
// once for both the collider description (#310) and the install-path rebuild.

#pragma once

#include "engine/physics/collider.h"
#include "engine/runtime/world_component_types.h"

namespace engine::runtime {

/// Builds the canonical convex hull recorded by `source`; false when the
/// source names no primitive hull or the builder rejects it.
bool build_primitive_hull(HullSource source,
                          physics::ConvexHullData *outHull) noexcept;

} // namespace engine::runtime
