// Procedural built-in mesh builders (plane, cube, sphere, cylinder, capsule,
// pyramid) used for bootstrap content and script-spawned primitives.

#pragma once

#include "engine/renderer/mesh_loader.h"

namespace engine::renderer {

// Vertex layout for all builders: 6 floats per vertex (px, py, pz, nx, ny,
// nz), CCW winding for outward-facing normals. Each returns false when GPU
// mesh creation fails.

/// 10x10 single-quad ground plane. NOTE: generated at y = +0.5 so it aligns
/// with the top face of the unit cube primitive.
bool build_plane_mesh(GpuMesh *outMesh) noexcept;

/// Unit cube centered on the origin (half extent 0.5).
bool build_cube_mesh(GpuMesh *outMesh) noexcept;

/// UV sphere of radius 0.5 (12 stacks x 24 slices).
bool build_sphere_mesh(GpuMesh *outMesh) noexcept;

/// Capped cylinder of radius 0.5 and height 1 (24 slices).
bool build_cylinder_mesh(GpuMesh *outMesh) noexcept;

/// Capsule of radius 0.5 with unit-length body (8 hemisphere stacks,
/// 16 slices).
bool build_capsule_mesh(GpuMesh *outMesh) noexcept;

/// Triangular pyramid with unit-ish footprint and apex at y = +0.5.
bool build_pyramid_mesh(GpuMesh *outMesh) noexcept;

} // namespace engine::renderer
