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
/// identity is that source's GUID plus the local id naming it among the
/// source's outputs. One glTF here yields a mesh, a skeleton and three
/// clips, which is exactly the case a bare GUID cannot express.
///
/// Returns a nil ref when no sidecar answers: the asset is still
/// catalogued and still reachable by path, it just has no persistent
/// identity until it is imported.
AssetRef resolve_authored_ref(const std::filesystem::path &osPath,
                              const AssetClassification &classification)
    noexcept {
  AssetSidecar sidecar{};

  if (classification.source) {
    if (read_asset_sidecar(osPath.string().c_str(), &sidecar) !=
        SidecarReadResult::Ok) {
      return AssetRef{};
    }
    return asset_ref_primary(sidecar.guid);
  }

  // A cooked output is named "<source stem>.<rest>", where the rest is
  // everything the source produced it as: "mesh", "skel", "walk.anim".
  // The producing source is found by trying every type's source suffixes
  // against progressively shorter stems, longest first, so "a.b.mesh"
  // prefers a source named "a.b" over one named "a".
  const std::string filename = osPath.filename().string();
  const std::filesystem::path directory = osPath.parent_path();
  std::size_t dot = filename.rfind('.');
  while (dot != std::string::npos) {
    const std::string stem = filename.substr(0U, dot);
    if (stem.empty()) {
      break;
    }
    // Every type's source suffixes, not just the classified type's: a
    // ".anim" classifies as Animation, which is Derived and so has no
    // source suffix of its own, yet the clip really was produced by a
    // ".gltf" on the Mesh row.
    for (std::size_t typeIndex = 0U; typeIndex < kAssetTypeCount;
         ++typeIndex) {
      const AssetTypeDescriptor &row =
          asset_type_descriptor(static_cast<AssetTypeTag>(typeIndex));
      for (std::size_t i = 0U; i < row.sourceSuffixCount; ++i) {
        const std::filesystem::path candidate =
            directory / (stem + row.sourceSuffixes[i]);
        std::error_code ec{};
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
          continue;
        }
        if (read_asset_sidecar(candidate.string().c_str(), &sidecar) !=
            SidecarReadResult::Ok) {
          return AssetRef{};
        }
        // The rest of the name, cooked suffix included: "mesh" and
        // "skel" of one source are two assets, so dropping the suffix
        // here would collapse them onto one reference.
        return AssetRef{sidecar.guid,
                        asset_local_id(filename.substr(dot + 1U).c_str())};
      }
    }
    if (dot == 0U) {
      break;
    }
    dot = filename.rfind('.', dot - 1U);
  }
  return AssetRef{};
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
    metadata.ref = resolve_authored_ref(entry.path(), classification);
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
  }

  char message[192] = {};
  std::snprintf(message, sizeof(message),
                "asset catalog: %zu registered, %zu already known, %zu "
                "skipped, %zu refused under '%s'",
                result.registered, result.alreadyKnown, result.skipped,
                result.refused, mountPrefix);
  core::log_message(core::LogLevel::Info, "assets", message);
  return result;
}

} // namespace engine::content
