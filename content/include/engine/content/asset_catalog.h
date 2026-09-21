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
  /// records that were readable are still in the store, so a caller can
  /// show the project while refusing to cook or package it.
  bool ok = false;
};

/// Registers every runtime-form asset file under `osRoot` as
/// `mountPrefix/<relative path>`, typed by the asset type table: the
/// cooked form of a cooked or derived type, the authored form of a source
/// type. Entries under a `.thumbnails` directory and dot-files are
/// hidden. Never registers a path that would not fit a record whole.
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
MountRegistration register_mounted_assets(MetadataStore *store,
                                          const char *mountPrefix,
                                          const char *osRoot) noexcept;

} // namespace engine::content
