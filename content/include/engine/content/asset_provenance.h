// Declares the index of which source produced which cooked output.
//
// A cooked file carries no identity of its own: it is regenerable, and
// it belongs to the source that made it. That relationship has to be a
// fact the cook recorded, not a guess from the filename — "hero.mesh"
// beside both "hero.gltf" and "hero.glb" has two equally good-looking
// producers, and so does "hero.walk.anim" beside "hero.gltf" and
// "hero.walk.gltf". A guess picks one silently and binds every
// reference to the wrong asset.
//
// So the cook writes the producing source's GUID and each output's local
// id into the cook stamp, and this index reads them back, together with
// the files the cook read beside the source (its DEP_HASH lines): every
// output of a cook depends on them, and the catalog records that as the
// output's dependency edges. It is built
// from the stamps found under a root, which keeps working when cooked
// data moves out of the source tree into a cache root: the stamps move
// with the outputs they certify.

#pragma once

#include <cstddef>
#include <cstdint>

#include <string>
#include <vector>

#include "engine/content/asset_identity.h"
#include "engine/content/asset_metadata.h"

namespace engine::content {

/// Map from a cooked output's path, relative to the root it was indexed
/// under, to the AssetRef the cook recorded for it and the files the cook
/// read. It grows with the stamps it reads, so a project of any size is
/// indexed whole; it lives for one mount walk, which is cold work that
/// already allocates.
struct ProvenanceIndex final {
  /// Longest output path the index records; a longer one is counted in
  /// `overflowed`, since the catalog could not record it whole either.
  static constexpr std::size_t kMaxPathLength = 260U;

  struct Entry final {
    std::string relativePath{};
    AssetRef ref{};
    /// The run of `dependencies` the output's stamp recorded.
    std::uint32_t firstDependency = 0U;
    std::uint32_t dependencyCount = 0U;
  };

  /// Sorted by relativePath once the index is built.
  std::vector<Entry> entries{};
  /// Dependency ids across every stamp; a stamp's outputs share its run.
  std::vector<AssetId> dependencies{};
  /// Outputs whose path is too long to record; a walk that reports any of
  /// these is incomplete and says so.
  std::size_t overflowed = 0U;
};

/// Reads every "*.cookstamp" under `osRoot` and records what each one
/// says produced its outputs. Paths are stored relative to `osRoot` with
/// '/' separators, the same spelling the catalog walk uses. Each
/// dependency is recorded as the id of `mountPrefix/<path under osRoot>`,
/// the id the catalog gives a file there; one outside `osRoot` has no
/// such id and is left out. Returns false when the root cannot be walked;
/// a stamp that cannot be read is skipped, because a cooked output with
/// no readable provenance is reported as unidentified by the caller
/// rather than guessed at here.
bool build_provenance_index(const char *osRoot, const char *mountPrefix,
                            ProvenanceIndex *out) noexcept;

/// The index's record for the output at `relativePath`, or nullptr when
/// no stamp claims it.
const ProvenanceIndex::Entry *
find_provenance_entry(const ProvenanceIndex &index,
                      const char *relativePath) noexcept;

/// The AssetRef the cook recorded for the output at `relativePath`, or a
/// nil ref when no stamp claims it.
AssetRef provenance_for_output(const ProvenanceIndex &index,
                               const char *relativePath) noexcept;

} // namespace engine::content
