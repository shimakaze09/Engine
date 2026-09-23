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
///
/// `ref` is the asset's persistent identity where the caller knows it: a
/// built-in derives one from its path, so a scene may name it. It is nil
/// for a load that carries no authored identity, such as a script asking
/// for a file by name — that mesh is reachable by id for this session and
/// by nothing afterwards, which is what asking by name means.
bool note_mesh_asset_path(renderer::AssetDatabase *database,
                          renderer::AssetId id, const char *virtualPath,
                          const core::AssetRef &ref) noexcept;

/// What the pass has already reported for the World content it last saw;
/// a new content epoch clears it. Full means later entries are counted
/// but not reported, said once.
struct UnresolvedMeshReports final {
  static constexpr std::size_t kMaxReported = 64U;
  /// Ids the catalog has no mesh path for.
  std::array<renderer::AssetId, kMaxReported> ids{};
  std::size_t count = 0U;
  /// References the catalog has no asset for at all, which is the earlier
  /// failure: an id at least said where to look.
  std::array<core::AssetRef, kMaxReported> refs{};
  std::size_t refCount = 0U;
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
  /// Authored references bound to a catalogued asset this pass.
  std::size_t bound = 0U;
  /// Authored references that named no catalogued asset.
  std::size_t unbound = 0U;
};

/// Binds every authored asset reference the World carries to the id the
/// catalog currently gives that asset, then requests a streaming load for
/// every mesh so bound whose asset is Unloaded and whose id the catalog
/// maps to a mesh path.
///
/// The World is mutable because binding is the point: a reference is the
/// authored identity a document carries, and the id beside it is this
/// session's answer for where that asset lives, so the pass writes the
/// answer back into the component. A component whose id was set directly
/// — a runtime load by path, or a built-in the bootstrap placed — carries
/// no reference and keeps the id it has.
///
/// Loading, Ready and Failed ids are left as they are, so a failed load is
/// not retried every frame. Runs once per frame; its cost is one table
/// probe per reference and it never allocates. `reports` keeps the
/// references and ids already logged.
MeshResolutionPass request_referenced_mesh_assets(
    runtime::World &world, runtime::EngineAssetDatabaseService *service,
    UnresolvedMeshReports *reports) noexcept;

} // namespace engine
