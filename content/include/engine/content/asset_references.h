// Declares the references authored documents make to other assets, and
// the catalog's index of them. A scene, prefab, material or animation
// controller names an asset by its AssetRef text (a GUID, or
// "<guid>#<local id>" for a sub-asset) or, for scripts, controllers and
// clips, by its virtual path. One scanner reads both out of a document's
// JSON string values, as Unity finds the GUIDs a scene's text names; Find
// Usages and the index both use it, so the two agree on what a reference
// is.
//
// The index is a table of document-to-asset edges beside the catalog's
// records, not their 32-entry dependency arrays: a scene can name far more
// assets than that, and a material's record array is rewritten whenever it
// loads. It is built once every mount is catalogued, since a scene may name
// a package's asset or a built-in mesh, and a document is indexed again
// when the editor saves it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_type_table.h"
#include "engine/core/asset_identity.h"

namespace engine::content {

/// The largest document the scanner reads, in bytes.
inline constexpr std::size_t kMaxReferencingDocumentBytes = 4U * 1024U * 1024U;

/// True for the asset kinds whose documents can reference other assets:
/// scenes, prefabs, materials and animation controllers.
bool asset_kind_references_assets(AssetTypeTag kind) noexcept;

/// One string value a document holds that may name an asset: either an
/// AssetRef (`isRef`), or text that names one only if it is a catalogued
/// virtual path.
struct DocumentReference final {
  bool isRef = false;
  core::AssetRef ref{};
  /// The decoded string, null-terminated; empty for a ref.
  const char *text = "";
};

/// Called once per string value the document holds, in document order.
using DocumentReferenceVisitor = void (*)(const DocumentReference &reference,
                                          void *userData) noexcept;

/// Parses `text` as JSON and visits every string value in it: object
/// member values and array elements, at any depth, never keys. A value
/// that parses as AssetRef text is visited as a ref; any other string up
/// to 259 bytes, as text. False, with nothing visited, when `text` is not
/// a JSON document.
bool scan_document_references(const char *text, std::size_t size,
                              DocumentReferenceVisitor visit,
                              void *userData) noexcept;

/// Replaces the edges recorded for `document` with one to each of
/// `targets` (duplicates and invalid ids dropped). False, with the
/// previous edges kept and an error logged, when the table cannot grow.
bool set_document_references(AssetCatalog *catalog, AssetId document,
                             const AssetId *targets,
                             std::size_t count) noexcept;

/// Copies up to `maxIds` of the assets `document` references into `outIds`
/// and returns how many it references, which may exceed `maxIds`.
std::size_t get_document_references(const AssetCatalog *catalog,
                                    AssetId document, AssetId *outIds,
                                    std::size_t maxIds) noexcept;

/// Copies up to `maxIds` of the documents that reference `target` into
/// `outIds` and returns how many there are, which may exceed `maxIds`.
std::size_t find_asset_referrers(const AssetCatalog *catalog, AssetId target,
                                 AssetId *outIds, std::size_t maxIds) noexcept;

/// Collects every asset `root` needs, directly or through other assets:
/// the dependencies records list (a material's parent and textures, a
/// cooked output's cook inputs) and the references documents make, breadth
/// first, each once, `root` excluded. Ids with no record (a cook input the
/// catalog does not hold) are included and not followed further. Copies up
/// to `maxIds` into `outIds` and returns how many there are, which may
/// exceed `maxIds`; 0 when the walk cannot allocate (logged). What a
/// package of `root` must carry.
std::size_t collect_asset_closure(const AssetCatalog *catalog, AssetId root,
                                  AssetId *outIds, std::size_t maxIds) noexcept;

/// What indexing one document, or all of them, found.
struct DocumentIndexReport final {
  /// Documents read and indexed.
  std::size_t documents = 0U;
  /// Edges recorded.
  std::size_t references = 0U;
  /// AssetRefs that name nothing catalogued: no edge is recorded.
  std::size_t dangling = 0U;
  /// Documents that could not be read, were larger than the scanner
  /// reads, or are not JSON; each is logged, and keeps no edges.
  std::size_t failed = 0U;
};

/// Reads the catalogued document `document` through the VFS and records
/// its references, replacing the ones it had: each AssetRef that names a
/// catalogued asset, and each string that is a catalogued asset's path.
DocumentIndexReport index_document_references(AssetCatalog *catalog,
                                              AssetId document) noexcept;

/// Indexes every catalogued document (asset_kind_references_assets), after
/// every mount is catalogued. Cold path: it reads every document once.
DocumentIndexReport index_catalogued_documents(AssetCatalog *catalog) noexcept;

} // namespace engine::content
