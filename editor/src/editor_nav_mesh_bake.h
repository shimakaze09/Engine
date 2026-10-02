// Declares the editor's Bake for a NavMeshSurface: the surface's volume
// is baked over the editing World and written as its .navmesh file, which
// gets an asset identity so the catalog and Find Usages know it, and the
// loaded scene reads it again.

#pragma once

#include <cstddef>

#include "engine/runtime/world.h"

namespace engine::editor {

/// What a bake did, for the Inspector to show.
struct NavMeshBakeReport final {
  bool written = false;
  char message[192] = {};
};

/// Chooses where a surface with no path is baked to: beside nothing else,
/// under the asset mount, named after the open scene (or "NavMesh" before
/// it is saved) with a number added when that name is taken. False when
/// no free name fits `size`.
bool choose_nav_mesh_path(const char *sceneDocumentPath, char *out,
                          std::size_t size) noexcept;

/// Bakes the surface on `entity` over `world` and writes its .navmesh
/// file, through a staged atomic replacement, at `pathBuffer`; an empty
/// buffer is first filled by choose_nav_mesh_path. Nothing is written when
/// the volume holds nothing walkable or the bake fails, and the previous
/// file is kept. The report says which.
NavMeshBakeReport bake_nav_mesh_surface_file(const runtime::World &world,
                                             runtime::Entity entity,
                                             char *pathBuffer,
                                             std::size_t pathSize) noexcept;

} // namespace engine::editor
