// Implements the mount walk behind register_mounted_assets: a recursive
// directory iteration over one mount's OS root that classifies each file
// by the asset type table and registers the runtime forms under their
// virtual paths. Cold path: filesystem iteration and its allocations are
// acceptable here and nowhere on a frame.

#include "engine/content/asset_catalog.h"

#include <cstdio>
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

/// One registered file, kept for the identity validation the walk runs
/// once it has seen the whole mount.
struct RegisteredEntry final {
  std::string virtualPath{};
  AssetRef ref{};
};

/// Names every path in every set of entries that share an AssetRef, and
/// returns how many entries were involved. Never picks a winner: a
/// duplicate identity is an error to repair, and choosing between them
/// would silently rebind references somebody already wrote.
std::size_t report_duplicate_refs(
    const std::vector<RegisteredEntry> &entries) noexcept {
  std::size_t offenders = 0U;
  for (std::size_t i = 0U; i < entries.size(); ++i) {
    if (!asset_ref_is_valid(entries[i].ref)) {
      continue;
    }
    bool first = true;
    std::size_t inThisSet = 0U;
    for (std::size_t j = 0U; j < entries.size(); ++j) {
      if ((j == i) || !(entries[j].ref == entries[i].ref)) {
        continue;
      }
      if (j < i) {
        // Already reported as part of an earlier entry's set.
        first = false;
        break;
      }
      ++inThisSet;
    }
    if (!first || (inThisSet == 0U)) {
      continue;
    }
    char guidText[kAssetGuidTextLength + 1U] = {};
    static_cast<void>(
        format_asset_guid(entries[i].ref.guid, guidText, sizeof(guidText)));
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "asset catalog: %s (local id %016llx) is claimed by more "
                  "than one asset; repair the duplicate rather than letting "
                  "references resolve to whichever indexed last",
                  guidText,
                  static_cast<unsigned long long>(entries[i].ref.localId));
    core::log_message(core::LogLevel::Error, "assets", message);
    for (const RegisteredEntry &entry : entries) {
      if (entry.ref == entries[i].ref) {
        core::log_path_diagnostic(core::LogLevel::Error, "assets",
                                  entry.virtualPath.c_str(),
                                  "asset catalog: claims that identity");
        ++offenders;
      }
    }
  }
  return offenders;
}

/// Names every path that differs from another only by letter case, and
/// returns how many were involved.
std::size_t report_case_collisions(
    const std::vector<RegisteredEntry> &entries) noexcept {
  const auto folded = [](const std::string &text) noexcept {
    std::string lowered = text;
    for (char &ch : lowered) {
      if ((ch >= 'A') && (ch <= 'Z')) {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
    return lowered;
  };
  std::size_t offenders = 0U;
  for (std::size_t i = 0U; i < entries.size(); ++i) {
    const std::string lowered = folded(entries[i].virtualPath);
    bool first = true;
    std::size_t matches = 0U;
    for (std::size_t j = 0U; j < entries.size(); ++j) {
      if ((j == i) || (folded(entries[j].virtualPath) != lowered)) {
        continue;
      }
      if (j < i) {
        first = false;
        break;
      }
      ++matches;
    }
    if (!first || (matches == 0U)) {
      continue;
    }
    for (const RegisteredEntry &entry : entries) {
      if (folded(entry.virtualPath) != lowered) {
        continue;
      }
      core::log_path_diagnostic(
          core::LogLevel::Error, "assets", entry.virtualPath.c_str(),
          "asset catalog: differs from another asset only by letter case, "
          "so this project is one file on Windows and macOS and two on "
          "Linux; rename one of them");
      ++offenders;
    }
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

MountRegistration register_mounted_assets(MetadataStore *store,
                                          const char *mountPrefix,
                                          const char *osRoot) noexcept {
  MountRegistration result{};
  if ((store == nullptr) || (mountPrefix == nullptr) ||
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
  // Heap, because the index is larger than a Windows thread's whole
  // default stack; this walk is cold filesystem work that already
  // allocates, and the index is freed with the walk rather than held for
  // the process's life.
  const auto provenance = std::make_unique<ProvenanceIndex>();
  static_cast<void>(build_provenance_index(osRoot, provenance.get()));

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
    if (find_asset_metadata(store, metadata.assetId) != nullptr) {
      ++result.alreadyKnown;
      continue;
    }
    if (!register_asset_metadata(store, metadata)) {
      core::log_path_diagnostic(core::LogLevel::Warning, "assets",
                                metadata.filePath.data(),
                                "asset catalog: the metadata table is full; "
                                "this asset is not catalogued");
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
  result.ok = (result.unidentified == 0U) && (result.duplicateRefs == 0U) &&
              (result.caseCollisions == 0U);

  char message[192] = {};
  std::snprintf(message, sizeof(message),
                "asset catalog: %zu registered, %zu already known, %zu "
                "skipped, %zu refused under '%s'",
                result.registered, result.alreadyKnown, result.skipped,
                result.refused, mountPrefix);
  core::log_message(core::LogLevel::Info, "assets", message);
  if (!result.ok) {
    char failure[256] = {};
    std::snprintf(failure, sizeof(failure),
                  "asset catalog: '%s' did not index cleanly: %zu without an "
                  "identity, %zu claiming a duplicate, %zu colliding only by "
                  "case. Every offending path is named above.",
                  mountPrefix, result.unidentified, result.duplicateRefs,
                  result.caseCollisions);
    core::log_message(core::LogLevel::Error, "assets", failure);
  }
  return result;
}

} // namespace engine::content
