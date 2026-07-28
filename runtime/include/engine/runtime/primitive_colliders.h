// Declares canonical convex-hull builders for the built-in shape primitives.

#pragma once

// Cylinder and pyramid props collide as convex hulls (Unity/Unreal have no
// cylinder or pyramid collider primitives either); these builders produce
// hulls matching the renderer's procedural meshes so collision is WYSIWYG.

#include "engine/physics/collider.h"

namespace engine::runtime {

/// Builds the unit cylinder hull (radius 0.5, half height 0.5, 16 slices —
/// the render mesh uses 24; 16 keeps Quickhull inside the plane budget).
bool build_cylinder_hull(physics::ConvexHullData *outHull) noexcept;

/// Builds the unit pyramid (tetrahedron) hull matching the render mesh.
bool build_pyramid_hull(physics::ConvexHullData *outHull) noexcept;

} // namespace engine::runtime
