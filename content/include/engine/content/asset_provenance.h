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

#include <array>

#include "engine/content/asset_identity.h"
#include "engine/content/asset_metadata.h"

namespace engine::content {

/// Fixed-capacity map from a cooked output's path, relative to the root
/// it was indexed under, to the AssetRef the cook recorded for it.
///
/// Over a megabyte, so it never goes on a stack: Windows gives a thread
/// 1 MiB by default where Linux gives 8 MiB, and one of these is larger
/// than the whole Windows allowance. Copying and moving are deleted so
/// that stays a compile error rather than a crash only one platform
/// shows — including the `*index = ProvenanceIndex{}` spelling of a
/// reset, whose temporary is what a build_provenance_index caller would
/// otherwise pay for. Declare one at namespace scope, in a heap
/// allocation, or as a member of something already there.
struct ProvenanceIndex final {
  static constexpr std::size_t kMaxOutputs = 4096U;
  static constexpr std::size_t kMaxPathLength = 260U;
  /// Dependency ids across every stamp; a stamp's outputs share its run.
  static constexpr std::size_t kMaxDependencies = 8192U;

  struct Entry final {
    char relativePath[kMaxPathLength] = {};
    AssetRef ref{};
    /// The run of `dependencies` the output's stamp recorded.
    std::uint32_t firstDependency = 0U;
    std::uint32_t dependencyCount = 0U;
  };

  std::array<Entry, kMaxOutputs> entries = std::array<Entry, kMaxOutputs>();
  std::size_t count = 0U;
  std::array<AssetId, kMaxDependencies> dependencies =
      std::array<AssetId, kMaxDependencies>();
  std::size_t dependencyCount = 0U;
  /// Outputs a stamp named that did not fit the index; a walk that
  /// reports any of these is incomplete and says so.
  std::size_t overflowed = 0U;
  /// Dependencies a stamp named that did not fit `dependencies`; reported
  /// the same way.
  std::size_t dependenciesDropped = 0U;

  ProvenanceIndex() = default;
  ProvenanceIndex(const ProvenanceIndex &) = delete;
  ProvenanceIndex(ProvenanceIndex &&) = delete;
  ProvenanceIndex &operator=(const ProvenanceIndex &) = delete;
  ProvenanceIndex &operator=(ProvenanceIndex &&) = delete;
  ~ProvenanceIndex() = default;
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
