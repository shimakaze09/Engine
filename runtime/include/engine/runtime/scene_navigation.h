// Declares the navigation meshes of the loaded scene: each NavMeshSurface
// in the World names a .navmesh file its editor bake wrote, and the scene
// reads it, so a game walks the mesh baked in the editor without baking at
// run time (Unity's NavMeshSurface keeps its NavMeshData asset the same
// way). The files are read only when the surfaces or their paths change,
// or a bake asks for it, so a frame with nothing new costs one comparison
// per surface.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/navigation/nav_mesh.h"
#include "engine/runtime/world_component_types.h"

namespace engine::runtime {

class World;

/// The most surfaces whose meshes a scene holds at once.
inline constexpr std::size_t kMaxSceneNavMeshes = 8U;

/// The meshes the World's surfaces name. Owned by the frame pipeline.
class SceneNavigation final {
public:
  SceneNavigation() noexcept = default;
  SceneNavigation(const SceneNavigation &) = delete;
  SceneNavigation &operator=(const SceneNavigation &) = delete;

  /// Brings the meshes in line with `world`'s surfaces. A surface whose
  /// entity, path or World content changed, or every surface after
  /// request_scene_navigation_reload, has its file read again. A surface
  /// with no path, or whose file is missing, unreadable or damaged, has no
  /// mesh, and why is logged once per change. Surfaces past
  /// kMaxSceneNavMeshes are left without a mesh, with one Warning. Main
  /// thread, outside the simulation.
  void update(const World &world) noexcept;

  /// Forgets every mesh.
  void clear() noexcept;

  /// Number of surfaces tracked, loaded or not.
  std::size_t count() const noexcept { return m_count; }
  /// The surface entity of slot `index`; kInvalidEntity out of range.
  Entity surface_at(std::size_t index) const noexcept;
  /// The mesh slot `index` holds; nullptr when out of range or the
  /// surface's file did not load.
  const navigation::NavMesh *mesh_at(std::size_t index) const noexcept;
  /// The mesh of `surface`'s slot; nullptr when it has none.
  const navigation::NavMesh *mesh_for(Entity surface) const noexcept;

private:
  struct Slot final {
    Entity surface{};
    char path[NavMeshSurfaceComponent::kMaxPathLength + 1U] = {};
    bool loaded = false;
    navigation::NavMesh mesh{};
  };

  Slot m_slots[kMaxSceneNavMeshes]{};
  std::size_t m_count = 0U;
  std::uint32_t m_contentEpoch = 0U;
  std::uint32_t m_reloadGeneration = 0U;
  bool m_bound = false;
  bool m_overflowReported = false;
};

/// Asks every SceneNavigation to read its files again on its next update:
/// the editor's Bake calls it after writing a .navmesh, whose path is the
/// one already loaded. Safe from any thread.
void request_scene_navigation_reload() noexcept;

/// Reads the .navmesh file at the VFS path `path` into `out`. False,
/// logged, with `out` unchanged, when the file is missing, larger than a
/// navigation mesh can be, or does not decode.
bool load_nav_mesh_file(const char *path, navigation::NavMesh *out) noexcept;

} // namespace engine::runtime
