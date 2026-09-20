// Implements the mount walk behind register_mounted_assets: a recursive
// directory iteration over one mount's OS root that classifies each file
// by the asset type table and registers the runtime forms under their
// virtual paths. Cold path: filesystem iteration and its allocations are
// acceptable here and nowhere on a frame.

#include "engine/content/asset_catalog.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "engine/content/asset_metadata.h"
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

  // Named in the one legacy-suffix warning below, so the author has a
  // concrete file to look at rather than only a count.
  std::string firstLegacy{};
  const char *legacyReplacement = nullptr;

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
    if (classification.legacy) {
      ++result.legacyNamed;
      if (firstLegacy.empty()) {
        firstLegacy = generic;
        legacyReplacement = asset_legacy_replacement_suffix(generic.c_str());
      }
    }
  }

  char message[192] = {};
  std::snprintf(message, sizeof(message),
                "asset catalog: %zu registered, %zu already known, %zu "
                "skipped, %zu refused under '%s'",
                result.registered, result.alreadyKnown, result.skipped,
                result.refused, mountPrefix);
  core::log_message(core::LogLevel::Info, "assets", message);
  // One line for the whole walk rather than one per file: a project
  // authored before the rename would otherwise flood the log with a
  // warning per asset, and the author's next step is the same either way.
  if (result.legacyNamed > 0U) {
    char legacyMessage[320] = {};
    std::snprintf(legacyMessage, sizeof(legacyMessage),
                  "asset catalog: %zu asset(s) under '%s' carry a superseded "
                  "suffix and were catalogued anyway; rename them to end in "
                  "'%s' (for example '%s'). The old names stop classifying "
                  "after this revision.",
                  result.legacyNamed, mountPrefix,
                  (legacyReplacement != nullptr) ? legacyReplacement : "?",
                  firstLegacy.c_str());
    core::log_message(core::LogLevel::Warning, "assets", legacyMessage);
  }
  return result;
}

} // namespace engine::content
