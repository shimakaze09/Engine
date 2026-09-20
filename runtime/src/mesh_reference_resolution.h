// Declares the pass that turns the mesh references a World carries into
// streaming loads by way of the asset catalog, and the catalog note a
// path-driven load leaves behind. A saved scene stores mesh identity as an
// id; the catalog maps the id back to the path the bytes live at, so a
// reopened scene draws its meshes with no script naming them. A reference
// the catalog cannot place is reported once per World content through the
// diagnostics record, never once per frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"
#include "engine/runtime/service_registry.h"

namespace engine {

namespace runtime {
class World;
}

/// Registers `virtualPath` as the catalog path of a mesh id that has no
/// record yet, so a mesh loaded by path is listed and named like one the
/// mount walk found. A record already present is kept. False when the
/// arguments are invalid or the metadata table is full.
bool note_mesh_asset_path(renderer::AssetDatabase *database,
                          renderer::AssetId id,
                          const char *virtualPath) noexcept;

/// The mesh ids already reported as unplaceable for the World content the
/// pass last saw; a new content epoch clears it. Full means later ids are
/// counted but not reported, said once.
struct UnresolvedMeshReports final {
  static constexpr std::size_t kMaxReported = 64U;
  std::array<renderer::AssetId, kMaxReported> ids{};
  std::size_t count = 0U;
  std::uint32_t contentEpoch = 0U;
  bool overflowReported = false;
};

/// What one pass did.
struct MeshResolutionPass final {
  /// Loads enqueued this pass.
  std::size_t requested = 0U;
  /// References whose id the catalog has no mesh path for, counted per
  /// reference; the report table decides what is logged.
  std::size_t unresolved = 0U;
};

/// Requests a streaming load for every mesh the World references (mesh
/// components and foliage LODs) whose asset is Unloaded and whose id the
/// catalog maps to a mesh path. Loading, Ready and Failed ids are left as
/// they are, so a failed load is not retried every frame. Runs once per
/// frame; its cost is one table probe per reference and it never
/// allocates. `reports` keeps the unplaceable ids already logged.
MeshResolutionPass request_referenced_mesh_assets(
    const runtime::World &world, runtime::EngineAssetDatabaseService *service,
    UnresolvedMeshReports *reports) noexcept;

} // namespace engine
