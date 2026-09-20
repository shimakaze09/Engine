// Declares the mount walk that fills a MetadataStore with the assets a
// mounted directory holds, so a saved reference resolves to a path and a
// picker can list what exists before anything has loaded. Runs once at a
// cold boundary; a record a loader already registered keeps its richer
// form. The walk knows what an asset is only through the asset type table.

#pragma once

#include <cstddef>

#include "engine/content/metadata_store.h"

namespace engine::content {

/// What one mount walk did, for the caller's log line and for tests.
struct MountRegistration final {
  /// Files registered by this walk.
  std::size_t registered = 0U;
  /// Files whose id was already in the store; their record is kept.
  std::size_t alreadyKnown = 0U;
  /// Files that are not a runtime asset form: unclassified suffixes, the
  /// authored source of a cooked type, hidden entries.
  std::size_t skipped = 0U;
  /// Files refused with a diagnostic: the store is full or the virtual
  /// path does not fit a record whole.
  std::size_t refused = 0U;
};

/// Registers every runtime-form asset file under `osRoot` as
/// `mountPrefix/<relative path>`, typed by the asset type table: the
/// cooked form of a cooked or derived type, the authored form of a source
/// type. Types the table identifies by content rather than suffix are not
/// registered here; their loaders are. Entries under a `.thumbnails`
/// directory and dot-files are hidden. Never registers a path that would
/// not fit a record whole. Returns zero counts when either argument is
/// null or the root cannot be walked.
MountRegistration register_mounted_assets(MetadataStore *store,
                                          const char *mountPrefix,
                                          const char *osRoot) noexcept;

} // namespace engine::content
