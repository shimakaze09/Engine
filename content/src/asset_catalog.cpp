// Implements the mount walk behind register_mounted_assets: a recursive
// directory iteration over one mount's OS root that classifies each file
// by the asset type table and registers the runtime forms under their
// virtual paths. Cold path: filesystem iteration and its allocations are
// acceptable here and nowhere on a frame.

#include "engine/content/asset_catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/content/asset_metadata.h"
#include "engine/content/asset_provenance.h"
#include "engine/content/asset_sidecar.h"
#include "engine/content/asset_type_table.h"
#include "engine/core/diagnostic.h"
#include "engine/core/logging.h"

namespace engine::content {
namespace {

/// A file is the runtime form of its type when the table says the type
/// reaches the runtime in that form: cooked types by their cooked
/// suffix, source types by their authored suffix, derived types by their
/// produced suffix.
bool is_runtime_form(const AssetClassification &classification) noexcept {
  if (classification.tag == AssetTypeTag::Unknown) {
    return false;
  }
  const AssetTypeDescriptor &descriptor =
      asset_type_descriptor(classification.tag);
  switch (descriptor.policy) {
  case AssetSourcePolicy::Source:
    return classification.source;
  case AssetSourcePolicy::Cooked:
  case AssetSourcePolicy::Derived:
    return !classification.source;
  }
  return false;
}

/// The authored identity of a file the walk is about to catalogue.
///
/// A source-policy asset carries its own sidecar, so it is the primary
/// asset of its own GUID. A cooked or derived output has no sidecar —
/// it is regenerable and belongs to the source that made it — so its
/// identity is whatever the cook recorded for it. The relationship is
/// never inferred from the filename: "hero.mesh" beside both
/// "hero.gltf" and "hero.glb" has two equally plausible producers, and
/// picking one silently binds every reference to the wrong asset.
///
/// Returns a nil ref when nothing answers: the asset is still
/// catalogued and still reachable by path, and the caller reports it.
AssetRef resolve_authored_ref(const std::filesystem::path &osPath,
                              const std::string &relativePath,
                              const AssetClassification &classification,
                              const ProvenanceIndex &provenance,
                              const char **outWhy) noexcept {
  if (classification.source) {
    AssetSidecar sidecar{};
    switch (read_asset_sidecar(osPath.string().c_str(), &sidecar)) {
    case SidecarReadResult::Ok:
      return asset_ref_primary(sidecar.guid);
    case SidecarReadResult::Absent:
      *outWhy = "has no .meta sidecar, so it has no identity; import it "
                "(asset_packer --init-meta) rather than leaving references "
                "to it unresolvable";
      return AssetRef{};
    case SidecarReadResult::Unreadable:
      *outWhy = "has a .meta sidecar that cannot be read; repair it rather "
                "than letting the asset index without an identity";
      return AssetRef{};
    case SidecarReadResult::Malformed:
      *outWhy = "has a malformed .meta sidecar; repair it rather than "
                "letting the asset index without an identity";
      return AssetRef{};
    }
    return AssetRef{};
  }

  const AssetRef fromCook =
      provenance_for_output(provenance, relativePath.c_str());
  if (!asset_ref_is_valid(fromCook)) {
    *outWhy = "is a cooked output no cook stamp claims, so which source "
              "produced it is unknown; recook it rather than guessing from "
              "its filename";
  }
  return fromCook;
}

/// Gives a cooked output the dependency edges its cook stamp recorded:
/// the files the cook read beside the source, so a change to one reaches
/// the output through content::notify_asset_changed. An output whose
/// stamp names more than a record holds keeps the first ones and says
/// which asset lost the rest.
void add_cook_dependencies(const ProvenanceIndex &provenance,
                           const std::string &relativePath,
                           AssetMetadata *metadata) noexcept {
  const ProvenanceIndex::Entry *entry =
      find_provenance_entry(provenance, relativePath.c_str());
  if (entry == nullptr) {
    return;
  }
  std::size_t dropped = 0U;
  for (std::uint32_t i = 0U; i < entry->dependencyCount; ++i) {
    const AssetId dependency =
        provenance.dependencies[entry->firstDependency + i];
    if (!asset_metadata_has_dependency(metadata, dependency) &&
        !asset_metadata_add_dependency(metadata, dependency)) {
      ++dropped;
    }
  }
  if (dropped > 0U) {
    char problem[160] = {};
    std::snprintf(problem, sizeof(problem),
                  "asset catalog: %zu of this asset's cook dependencies "
                  "exceed the %zu a record holds; changes to them will not "
                  "reach it",
                  dropped, AssetMetadata::kMaxDependencies);
    core::log_path_diagnostic(core::LogLevel::Warning, "assets",
                              metadata->filePath.data(), problem);
  }
}

/// One registered file, kept for the identity validation the walk runs
/// once it has seen the whole mount.
struct RegisteredEntry final {
  std::string virtualPath{};
  AssetRef ref{};
};

/// The indices of `entries` in the order `less` sorts them, ties kept in
/// walk order, so every set of equal entries is one run.
template <typename Less>
std::vector<std::size_t> sorted_indices(std::size_t count, Less less) {
  std::vector<std::size_t> order(count);
  for (std::size_t i = 0U; i < count; ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), less);
  return order;
}

/// Names every path in every set of entries that share an AssetRef, and
/// returns how many entries were involved. Never picks a winner: a
/// duplicate identity is an error to repair, and choosing between them
/// would silently rebind references somebody already wrote. Sorting keeps
/// it linear-logarithmic in the size of the mount.
std::size_t report_duplicate_refs(const std::vector<RegisteredEntry> &entries) {
  const auto refLess = [&entries](std::size_t lhs, std::size_t rhs) {
    const AssetRef &a = entries[lhs].ref;
    const AssetRef &b = entries[rhs].ref;
    const int guid = std::memcmp(&a.guid, &b.guid, sizeof(a.guid));
    return (guid != 0) ? (guid < 0) : (a.localId < b.localId);
  };
  const std::vector<std::size_t> order =
      sorted_indices(entries.size(), refLess);
  std::size_t offenders = 0U;
  for (std::size_t start = 0U; start < order.size();) {
    std::size_t end = start + 1U;
    while ((end < order.size()) &&
           (entries[order[end]].ref == entries[order[start]].ref)) {
      ++end;
    }
    const AssetRef &ref = entries[order[start]].ref;
    if (((end - start) > 1U) && asset_ref_is_valid(ref)) {
      char guidText[kAssetGuidTextLength + 1U] = {};
      static_cast<void>(
          format_asset_guid(ref.guid, guidText, sizeof(guidText)));
      char message[256] = {};
      std::snprintf(message, sizeof(message),
                    "asset catalog: %s (local id %016llx) is claimed by more "
                    "than one asset; repair the duplicate rather than "
                    "letting references resolve to whichever indexed last",
                    guidText, static_cast<unsigned long long>(ref.localId));
      core::log_message(core::LogLevel::Error, "assets", message);
      for (std::size_t i = start; i < end; ++i) {
        core::log_path_diagnostic(core::LogLevel::Error, "assets",
                                  entries[order[i]].virtualPath.c_str(),
                                  "asset catalog: claims that identity");
        ++offenders;
      }
    }
    start = end;
  }
  return offenders;
}

/// Names every path that differs from another only by letter case, and
/// returns how many were involved.
std::size_t
report_case_collisions(const std::vector<RegisteredEntry> &entries) {
  std::vector<std::string> folded(entries.size());
  for (std::size_t i = 0U; i < entries.size(); ++i) {
    folded[i] = entries[i].virtualPath;
    for (char &ch : folded[i]) {
      if ((ch >= 'A') && (ch <= 'Z')) {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
  }
  const std::vector<std::size_t> order = sorted_indices(
      entries.size(), [&folded](std::size_t lhs, std::size_t rhs) {
        return folded[lhs] < folded[rhs];
      });
  std::size_t offenders = 0U;
  for (std::size_t start = 0U; start < order.size();) {
    std::size_t end = start + 1U;
    while ((end < order.size()) &&
           (folded[order[end]] == folded[order[start]])) {
      ++end;
    }
    if ((end - start) > 1U) {
      for (std::size_t i = start; i < end; ++i) {
        core::log_path_diagnostic(
            core::LogLevel::Error, "assets",
            entries[order[i]].virtualPath.c_str(),
            "asset catalog: differs from another asset only by letter case, "
            "so this project is one file on Windows and macOS and two on "
            "Linux; rename one of them");
        ++offenders;
      }
    }
    start = end;
  }
  return offenders;
}

/// Thumbnail caches and dot-files are never assets.
bool is_hidden(const std::filesystem::path &relative) noexcept {
  for (const std::filesystem::path &part : relative) {
    const std::string name = part.string();
    if (!name.empty() && (name[0] == '.')) {
      return true;
    }
  }
  return false;
}

} // namespace

MountRegistration register_mounted_assets(AssetCatalog *catalog,
                                          const char *mountPrefix,
                                          const char *osRoot) noexcept {
  MountRegistration result{};
  if ((catalog == nullptr) || (mountPrefix == nullptr) ||
      (mountPrefix[0] == '\0') || (osRoot == nullptr) || (osRoot[0] == '\0')) {
    return result;
  }

  std::error_code ec{};
  const std::filesystem::path root(osRoot);
  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    core::log_path_diagnostic(core::LogLevel::Warning, "assets", mountPrefix,
                              "asset catalog: the mount's directory cannot "
                              "be walked; saved references will not resolve "
                              "until their assets load by path");
    return result;
  }

  // Read once for the whole walk: the stamps say which source produced
  // each cooked output, and re-reading them per file would be O(n^2).
  // The index is freed with the walk rather than held for the process's
  // life.
  const auto provenance = std::make_unique<ProvenanceIndex>();
  static_cast<void>(
      build_provenance_index(osRoot, mountPrefix, provenance.get()));

  // Collected so identity can be validated across the whole mount once
  // the walk has seen every asset, rather than per file.
  std::vector<RegisteredEntry> registered{};

  const std::filesystem::recursive_directory_iterator end{};
  for (; it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    const std::filesystem::directory_entry &entry = *it;
    std::error_code kindEc{};
    if (entry.is_symlink(kindEc) || kindEc) {
      continue;
    }
    if (!entry.is_regular_file(kindEc) || kindEc) {
      continue;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), root, kindEc);
    if (kindEc || relative.empty() || is_hidden(relative)) {
      ++result.skipped;
      continue;
    }
    const std::string generic = relative.generic_string();
    const AssetClassification classification =
        classify_asset_path(generic.c_str());
    if (!is_runtime_form(classification)) {
      ++result.skipped;
      continue;
    }

    AssetMetadata metadata{};
    const int written =
        std::snprintf(metadata.filePath.data(), metadata.filePath.size(),
                      "%s/%s", mountPrefix, generic.c_str());
    if ((written < 0) ||
        (static_cast<std::size_t>(written) >= metadata.filePath.size())) {
      // An identity that does not fit whole would name a different asset.
      core::log_path_diagnostic(core::LogLevel::Warning, "assets",
                                generic.c_str(),
                                "asset catalog: the path is too long to "
                                "record under the mount; the asset is not "
                                "catalogued");
      ++result.refused;
      continue;
    }
    metadata.assetId = make_asset_id_from_path(metadata.filePath.data());
    metadata.typeTag = classification.tag;
    const char *unidentifiedWhy = nullptr;
    metadata.ref = resolve_authored_ref(entry.path(), generic, classification,
                                        *provenance, &unidentifiedWhy);
    if (metadata.assetId == kInvalidAssetId) {
      ++result.refused;
      continue;
    }
    if (!classification.source) {
      add_cook_dependencies(*provenance, generic, &metadata);
    }
    const CatalogInsert insert =
        register_asset_metadata_if_absent(catalog, metadata);
    if (insert == CatalogInsert::AlreadyKnown) {
      ++result.alreadyKnown;
      continue;
    }
    if (insert == CatalogInsert::Refused) {
      char problem[192] = {};
      std::snprintf(problem, sizeof(problem),
                    "asset catalog: the catalog holds its limit of %zu "
                    "records, or has no memory for more; this asset is not "
                    "catalogued",
                    catalog->recordLimit);
      core::log_path_diagnostic(core::LogLevel::Warning, "assets",
                                metadata.filePath.data(), problem);
      ++result.refused;
      continue;
    }
    ++result.registered;
    registered.push_back(
        RegisteredEntry{std::string(metadata.filePath.data()), metadata.ref});
    if (unidentifiedWhy != nullptr) {
      char problem[320] = {};
      std::snprintf(problem, sizeof(problem), "asset catalog: this asset %s",
                    unidentifiedWhy);
      core::log_path_diagnostic(core::LogLevel::Error, "assets",
                                metadata.filePath.data(), problem);
      ++result.unidentified;
    }
  }

  result.duplicateRefs = report_duplicate_refs(registered);
  result.caseCollisions = report_case_collisions(registered);
  result.ok = (result.refused == 0U) && (result.unidentified == 0U) &&
              (result.duplicateRefs == 0U) && (result.caseCollisions == 0U);

  char message[192] = {};
  std::snprintf(message, sizeof(message),
                "asset catalog: %zu registered, %zu already known, %zu "
                "skipped, %zu refused under '%s'",
                result.registered, result.alreadyKnown, result.skipped,
                result.refused, mountPrefix);
  core::log_message(core::LogLevel::Info, "assets", message);
  if (!result.ok) {
    char failure[320] = {};
    std::snprintf(failure, sizeof(failure),
                  "asset catalog: '%s' did not index cleanly: %zu refused, "
                  "%zu without an identity, %zu claiming a duplicate, %zu "
                  "colliding only by case. Every offending path is named "
                  "above.",
                  mountPrefix, result.refused, result.unidentified,
                  result.duplicateRefs, result.caseCollisions);
    core::log_message(core::LogLevel::Error, "assets", failure);
  }
  return result;
}

} // namespace engine::content
