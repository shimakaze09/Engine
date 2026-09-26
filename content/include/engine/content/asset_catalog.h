// Declares the asset catalog: the engine's one record of what assets
// exist, where each one is, what identity it carries and what it depends
// on. Every system that names an asset — the renderer's loaders, scripts,
// the editor's browser and pickers, the scene reference resolver —
// resolves through one instance, which the engine pipeline owns. It also
// declares the mount walk that fills it with the assets a mounted
// directory holds, so a saved reference resolves to a path and a picker
// can list what exists before anything has loaded. The walk knows what an
// asset is only through the asset type table.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/content/asset_metadata.h"

namespace engine::content {

/// The engine's one record of what assets exist: a fixed-slot table keyed
/// by AssetId, open-addressed, so a record's address never moves while it
/// is held. `generation` moves once per write that lands — a register or
/// replace, a tag or dependency the record did not already carry, a clear
/// — and never on a refused one, so a consumer that noted it can tell the
/// catalog changed since.
struct AssetCatalog final {
  static constexpr std::size_t kMaxMetadata = 4096U;
  std::array<AssetMetadata, kMaxMetadata> entries =
      std::array<AssetMetadata, kMaxMetadata>();
  std::array<bool, kMaxMetadata> occupied{};
  std::uint64_t generation = 0U;
};

/// Resets every slot back to the empty state.
void clear_asset_catalog(AssetCatalog *catalog) noexcept;

/// Inserts the record keyed by its assetId, or replaces the whole record
/// already there; false when the id is invalid, a string is unterminated,
/// a count exceeds its array, or the table is full.
bool register_asset_metadata(AssetCatalog *catalog,
                             const AssetMetadata &metadata) noexcept;

/// Outcome of register_asset_metadata_if_absent.
enum class CatalogInsert : std::uint8_t {
  /// The record was added.
  Registered,
  /// The id already had a record, which is kept unchanged.
  AlreadyKnown,
  /// Refused for any reason register_asset_metadata refuses.
  Refused,
};

/// Adds the record only when its id has none, keeping the first writer's
/// record — the rule for a walk or a reference that knows less about an
/// asset than a loader that registered it.
CatalogInsert
register_asset_metadata_if_absent(AssetCatalog *catalog,
                                  const AssetMetadata &metadata) noexcept;

/// True when register_asset_metadata would find a slot for `id`: it has a
/// record already, or the table has room. Lets a caller that must write
/// several things together check the catalog's part before writing any.
bool can_register_asset_metadata(const AssetCatalog *catalog,
                                 AssetId id) noexcept;

/// Finds the matching record for the id; nullptr when absent.
const AssetMetadata *find_asset_metadata(const AssetCatalog *catalog,
                                         AssetId id) noexcept;

/// Finds the record at `virtualPath`, compared by canonical spelling, so
/// "assets//a.png" finds "assets/a.png"; nullptr when no record is there
/// or the path is not canonicalizable. Checks the stored path itself, so
/// two paths whose ids collide never answer for each other.
const AssetMetadata *
find_asset_metadata_by_path(const AssetCatalog *catalog,
                            const char *virtualPath) noexcept;

/// Finds the record whose persistent identity is `ref`; nullptr when no
/// catalogued asset carries it. This is the resolution a saved reference
/// goes through: the GUID answers "which asset", the record answers
/// "where it is now", so a rename or a move costs the reference nothing.
///
/// Linear over the catalog, and deliberately so: it runs when a document
/// loads, never per frame, and a reference that resolves to a path is
/// then addressed by that path's id.
const AssetMetadata *find_asset_metadata_by_ref(const AssetCatalog *catalog,
                                                const AssetRef &ref) noexcept;

/// Reports every pair of catalogued assets that claim one GUID, writing
/// up to `capacity` offending records into `outRecords` and returning how
/// many exist. A duplicate is an error to report, never to resolve:
/// picking a winner would silently rebind references somebody wrote.
std::size_t find_duplicate_guid_records(const AssetCatalog *catalog,
                                        const AssetMetadata **outRecords,
                                        std::size_t capacity) noexcept;

/// Adds a tag to the id's metadata; false when unknown or tags full.
bool add_asset_tag(AssetCatalog *catalog, AssetId id, const char *tag) noexcept;

/// True when the id's metadata carries the tag.
bool asset_has_tag(const AssetCatalog *catalog, AssetId id,
                   const char *tag) noexcept;

/// Collects up to maxIds ids carrying the tag; returns the count.
std::size_t query_assets_by_tag(const AssetCatalog *catalog, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept;

/// Collects up to maxIds ids of the given type; returns the count.
std::size_t query_assets_by_type(const AssetCatalog *catalog,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept;

/// Copies up to maxIds direct dependencies of the id; returns the count.
std::size_t get_dependencies(const AssetCatalog *catalog, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept;

/// Records a directed dependency edge id -> depId; false when full.
bool add_asset_dependency(AssetCatalog *catalog, AssetId id,
                          AssetId depId) noexcept;

/// Copies up to maxIds ids of the catalogued assets that record a direct
/// dependency on `id` into outIds and returns how many there are, which
/// may exceed maxIds. The catalog stores edges forward only, so this scans
/// every record's dependency list: at most kMaxMetadata x
/// AssetMetadata::kMaxDependencies comparisons, for a change or an edit,
/// never per frame. Scanning keeps the answer exact with no second index
/// to fall out of step and no cap on how many assets share a dependency.
std::size_t find_asset_dependents(const AssetCatalog *catalog, AssetId id,
                                  AssetId *outIds, std::size_t maxIds) noexcept;

/// Called once per dependent an asset change reaches. `dependent` is the
/// asset to bring up to date; `cause` is the asset it records a dependency
/// on that changed, the changed asset itself or a dependent visited before.
using AssetChangeVisitor = void (*)(AssetId dependent, AssetId cause,
                                    void *userData);

/// Tells everything that depends on `changed`, directly or through other
/// assets, that it changed: calls `visit` once per catalogued dependent,
/// breadth first, so a dependent is visited after the asset it was reached
/// through. A dependent reached along several paths is visited once, and a
/// cycle ends. `changed` itself need not be catalogued -- a file a cooked
/// asset was built from is a dependency without a record of its own. The
/// visitor must not register, replace or clear catalog records. Returns
/// how many dependents were visited; the catalog is read, never written,
/// so its generation does not move.
std::size_t notify_asset_changed(const AssetCatalog *catalog, AssetId changed,
                                 AssetChangeVisitor visit,
                                 void *userData) noexcept;

/// Loads an asset and all its dependencies depth-first, dependency-first,
/// invoking loadCallback exactly once per distinct asset in dependency
/// order (callers reach their own state through userData), so a shared
/// dependency is never loaded twice however wide the graph. False on a
/// cycle, excessive depth, a failed callback, or a graph that reaches more
/// ids the catalog holds no metadata for than the traversal can remember —
/// that limit is reported rather than met by repeating a load.
bool load_with_dependencies(AssetCatalog *catalog, AssetId rootId,
                            bool (*loadCallback)(AssetId id, void *userData),
                            void *userData) noexcept;

// --- Mount walk ---

/// What one mount walk did, for the caller's log line and for tests.
struct MountRegistration final {
  /// Files registered by this walk.
  std::size_t registered = 0U;
  /// Files whose id was already in the catalog; their record is kept.
  std::size_t alreadyKnown = 0U;
  /// Files that are not a runtime asset form: unclassified suffixes, the
  /// authored source of a cooked type, hidden entries.
  std::size_t skipped = 0U;
  /// Files refused with a diagnostic: the catalog is full or the virtual
  /// path does not fit a record whole.
  std::size_t refused = 0U;
  /// Registered files that came out with no persistent identity: a
  /// source whose sidecar is absent, unreadable or malformed, or a
  /// cooked output no cook stamp claims. Each is named in a diagnostic.
  std::size_t unidentified = 0U;
  /// Registered files sharing a full AssetRef with another. Every path
  /// in every colliding set is named; the walk never picks a winner and
  /// never regenerates an identity to break the tie.
  std::size_t duplicateRefs = 0U;
  /// Registered paths that differ from another only by letter case: one
  /// file on Windows and macOS, two on Linux, so the project builds for
  /// one teammate and not another.
  std::size_t caseCollisions = 0U;
  /// False when the mount did not index cleanly — any of the three
  /// counts above is non-zero, or the root could not be walked. The
  /// records that were readable are still in the catalog, so a caller can
  /// show the project while refusing to cook or package it.
  bool ok = false;
};

/// Registers every runtime-form asset file under `osRoot` as
/// `mountPrefix/<relative path>`, typed by the asset type table: the
/// cooked form of a cooked or derived type, the authored form of a source
/// type. Entries under a `.thumbnails` directory and dot-files are
/// hidden. Never registers a path that would not fit a record whole. A
/// cooked output records, as its dependencies, the files its cook stamp
/// says the cook read beside the source (a glTF's external buffers and
/// images), so a change to one reaches it through notify_asset_changed.
///
/// Validates identity as it goes and fails closed: an asset with no
/// identity, two assets claiming one, and two paths differing only by
/// case are each reported by path and clear `ok`. Indexing is where
/// these have to be caught — the CI gate catches them before they are
/// committed, but a project assembled on a machine is not obliged to
/// have gone through CI, and silently accepting a nil identity means a
/// reference that resolves to nothing much later, with nothing to say
/// why.
///
/// Returns zero counts with `ok` false when either argument is null or
/// the root cannot be walked.
MountRegistration register_mounted_assets(AssetCatalog *catalog,
                                          const char *mountPrefix,
                                          const char *osRoot) noexcept;

} // namespace engine::content
