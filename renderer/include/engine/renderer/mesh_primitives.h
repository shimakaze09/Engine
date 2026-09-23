// Procedural built-in mesh builders (plane, cube, sphere, cylinder, capsule,
// pyramid) used for bootstrap content and script-spawned primitives.

#pragma once

#include <cstddef>

#include "engine/renderer/mesh_loader.h"

namespace engine::renderer {

// Vertex layout for all builders: 6 floats per vertex (px, py, pz, nx, ny,
// nz), CCW winding for outward-facing normals. Each returns false when GPU
// mesh creation fails.

/// Where the plane's surface sits relative to the entity's origin: on it.
/// An object resting on a plane therefore sits at the plane's own height
/// plus the object's half extent, and placing the plane at the origin
/// puts its ground at zero.
///
/// It was 0.5 until the convention changed, generated level with the top
/// face of the unit cube so a floor built from both was continuous. That
/// saved a floor nobody builds and cost every author who placed a ground
/// plane at the origin and then stood something on it half a metre too
/// high. Named rather than inlined so the relationship stays checkable
/// from a test, and so a future change to it moves the geometry and the
/// checks together.
inline constexpr float kBuiltinPlaneSurfaceY = 0.0F;

/// Half extent of the unit cube, and the radius of the sphere, cylinder
/// and capsule: each of those is centered on the entity's origin, so an
/// object of one rests on a surface at that surface plus its half
/// extent.
inline constexpr float kBuiltinCubeHalfExtent = 0.5F;
inline constexpr float kBuiltinSphereRadius = 0.5F;

/// 10x10 single-quad ground plane, its surface at kBuiltinPlaneSurfaceY.
bool build_plane_mesh(GpuMesh *outMesh) noexcept;

/// Unit cube centered on the origin (half extent kBuiltinCubeHalfExtent).
bool build_cube_mesh(GpuMesh *outMesh) noexcept;

/// UV sphere of kBuiltinSphereRadius (12 stacks x 24 slices).
bool build_sphere_mesh(GpuMesh *outMesh) noexcept;

/// Capped cylinder of radius 0.5 and height 1 (24 slices).
bool build_cylinder_mesh(GpuMesh *outMesh) noexcept;

/// Capsule of radius 0.5 with unit-length body (8 hemisphere stacks,
/// 16 slices).
bool build_capsule_mesh(GpuMesh *outMesh) noexcept;

/// Triangular pyramid with unit-ish footprint and apex at y = +0.5.
bool build_pyramid_mesh(GpuMesh *outMesh) noexcept;

/// Fills the pyramid's CPU vertex data (12 verts x 6 floats, CCW outward
/// winding) so orientation is verifiable without a GPU; returns the float
/// count written (0 when the capacity is too small).
std::size_t fill_pyramid_vertices(float *outVerts,
                                  std::size_t capacity) noexcept;

/// Grass tuft: seven double-sided tapered blades rooted at y = 0, heights
/// around 0.5, for foliage patches.
bool build_grass_tuft_mesh(GpuMesh *outMesh) noexcept;

} // namespace engine::renderer
