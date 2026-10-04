// Declares the one bake-and-write path for a NavMeshSurface's .navmesh
// file, shared by the editor's Bake button and engine_validate's
// --bake-navmesh, and the check that a committed file is still what the
// scene bakes, which engine_validate's --check-navmesh runs. Unity bakes
// from both its editor and its build API (NavMeshBuilder); one path keeps
// the two from producing different files.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/runtime/world.h"

namespace engine::runtime {

/// The largest .navmesh file read back, in bytes: a full grid of the most
/// columns a bake samples, each with the most surfaces, is about 40 MiB,
/// so a file past this is not one the bake wrote.
inline constexpr std::uint64_t kMaxNavMeshFileBytes = 128ULL * 1024ULL * 1024ULL;

/// What writing a surface's file did.
enum class NavMeshWriteResult : std::uint8_t {
  /// Baked and written; the file has an asset identity.
  Written = 0,
  /// The path is empty or does not end in .navmesh.
  BadPath = 1,
  /// The entity has no surface or the bake failed (logged).
  BakeFailed = 2,
  /// Nothing walkable inside the volume; nothing was written.
  Empty = 3,
  /// The baked mesh could not be encoded.
  EncodeFailed = 4,
  /// The staged write failed; the previous file is kept.
  WriteFailed = 5,
};

/// The outcome of write_nav_mesh_surface_file.
struct NavMeshWriteReport final {
  NavMeshWriteResult result = NavMeshWriteResult::BakeFailed;
  /// Polygons in the written mesh.
  std::size_t polygons = 0U;
  /// False when the file was written but its sidecar could not be.
  bool identified = true;
};

/// Bakes the surface on `entity` over `world` and writes it to `vfsPath`
/// through a staged atomic replacement, then gives the file an asset
/// identity (a rebake keeps the one it has). Nothing is written, and the
/// previous file is kept, unless the result is Written.
NavMeshWriteReport write_nav_mesh_surface_file(const World &world,
                                               Entity entity,
                                               const char *vfsPath) noexcept;

/// Whether a surface's file is what the scene bakes now.
enum class NavMeshFileState : std::uint8_t {
  /// The file holds exactly the bytes a bake writes now.
  Current = 0,
  /// No file at the path.
  Missing = 1,
  /// The file differs from a bake now: the level or the surface changed.
  Stale = 2,
  /// The file exists but could not be read.
  Unreadable = 3,
  /// The path is empty or does not end in .navmesh.
  BadPath = 4,
  /// The bake failed, or found nothing walkable, so there is nothing to
  /// compare.
  BakeFailed = 5,
};

/// Bakes the surface on `entity` in memory and compares the encoding with
/// the file at `vfsPath` byte for byte. The encoding is deterministic, so
/// any difference means the colliders, the surface or its position moved
/// since the file was baked.
NavMeshFileState check_nav_mesh_surface_file(const World &world, Entity entity,
                                             const char *vfsPath) noexcept;

} // namespace engine::runtime
