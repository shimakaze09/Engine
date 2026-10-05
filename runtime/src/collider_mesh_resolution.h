// Declares the pass that gives each TriMesh collider the triangles of the
// mesh asset it names: the collider's reference resolves through the
// asset catalog to the cooked mesh, whose positions and indices build the
// collision mesh. A mesh is built once and shared by every collider that
// names it, and rebuilt when the asset is reimported. A reference that
// cannot be built is reported once per World content and the collider
// collides with nothing, as Unity's MeshCollider with no mesh does.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/content/asset_catalog.h"
#include "engine/physics/tri_mesh.h"

namespace engine::runtime {

class World;

/// Built collision meshes, and what the pass has reported for the World
/// content it last saw.
struct ColliderMeshCache final {
  static constexpr std::size_t kMaxMeshes = 64U;
  static constexpr std::size_t kMaxReported = 64U;

  struct Entry final {
    content::AssetId id = content::kInvalidAssetId;
    /// The asset's reload generation the mesh was built from.
    std::uint32_t generation = 0U;
    physics::TriMeshRef mesh{};
  };
  std::array<Entry, kMaxMeshes> entries{};
  std::size_t count = 0U;
  /// The entry a full cache replaces next. A replaced mesh lives on in
  /// every collider that holds it; only later colliders build anew.
  std::size_t nextReplaced = 0U;

  /// Assets and references already reported as unbuildable this content
  /// epoch, so a broken reference is not retried and logged every frame.
  std::array<content::AssetId, kMaxReported> failedIds{};
  std::size_t failedIdCount = 0U;
  std::array<core::AssetRef, kMaxReported> failedRefs{};
  std::size_t failedRefCount = 0U;
  std::uint32_t contentEpoch = 0U;
  bool contentSeen = false;
};

/// What one pass did.
struct ColliderMeshPass final {
  /// Colliders given a mesh this pass.
  std::size_t installed = 0U;
  /// Meshes built from their cooked files this pass.
  std::size_t built = 0U;
  /// Colliders whose mesh could not be built (reported once each).
  std::size_t failed = 0U;
};

/// Builds a collision mesh from the cooked mesh file at `osPath`: its
/// vertex positions and triangle list. False, with `*outMesh` unchanged and
/// `*outReason` saying why, for a file that will not load (missing, torn
/// cook generation, malformed) or a mesh that will not build.
bool build_collision_mesh_from_file(const char *osPath,
                                    physics::TriMeshRef *outMesh,
                                    const char **outReason) noexcept;

/// Gives every TriMesh collider in `world` that names a mesh the mesh its
/// reference resolves to now, building what the cache does not hold. Runs
/// in the Input phase, once per frame; with nothing new to do it costs one
/// catalog lookup and one cache scan per TriMesh collider.
ColliderMeshPass install_collider_meshes(World &world,
                                         const content::AssetCatalog *catalog,
                                         ColliderMeshCache *cache) noexcept;

} // namespace engine::runtime
