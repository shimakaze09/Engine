// Implements `asset_packer --init-meta <dir>`: the one-off migration that
// gives every authored asset and folder under a tree its persistent
// identity. It exists because identity has to start somewhere — a
// project that predates the sidecar has assets with no GUID, and no
// ordinary read, load or cook path is allowed to invent one.
//
// It is a migration transaction, not a repair loop: it only ever creates
// a sidecar that is absent, never rewrites or replaces one that is
// already there, and it stops on a sidecar it cannot read rather than
// overwriting bytes that may still be good.

#include "packer_shared.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "engine/content/asset_identity.h"
#include "engine/content/asset_sidecar.h"
#include "engine/content/asset_type_table.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"

namespace {

namespace ct = engine::content;

/// What the migration did, for its summary line and its exit code.
struct InitMetaResult final {
  std::size_t created = 0U;
  std::size_t alreadyIdentified = 0U;
  std::size_t skipped = 0U;
  std::size_t failed = 0U;
};

/// True for a path the sidecar scheme gives an identity to: the authored
/// form of a source type. A cooked or derived output is regenerable and
/// owns no identity of its own — it belongs to the source that produced
/// it.
bool needs_identity(const std::filesystem::path &path) {
  const std::string generic = path.generic_string();
  const ct::AssetClassification classification =
      ct::classify_asset_path(generic.c_str());
  if (classification.tag == ct::AssetTypeTag::Unknown) {
    return false;
  }
  const ct::AssetTypeDescriptor &row =
      ct::asset_type_descriptor(classification.tag);
  return (row.policy == ct::AssetSourcePolicy::Source) ||
         ((row.policy == ct::AssetSourcePolicy::Cooked) &&
          classification.source);
}

/// Thumbnail caches, dot-directories and the sidecars themselves are
/// never given identities.
bool is_ignored(const std::filesystem::path &relative) {
  for (const std::filesystem::path &part : relative) {
    const std::string name = part.string();
    if (!name.empty() && (name[0] == '.')) {
      return true;
    }
  }
  return false;
}

/// Carries a mesh source's authored settings over from the cooked record
/// that used to hold them, so the migration does not reset what somebody
/// already tuned. Best effort: a source with no cooked output yet simply
/// starts at the defaults.
bool recover_settings_from_cooked(const std::filesystem::path &source,
                                  ct::MeshImportSettings *outSettings) {
  std::filesystem::path cooked = source;
  cooked.replace_extension(".mesh.cookmeta");
  static char buffer[64U * 1024U] = {};
  std::size_t size = 0U;
  if (engine::core::read_whole_file(cooked.string().c_str(), buffer,
                                    sizeof(buffer), &size) !=
      engine::core::FileReadResult::Ok) {
    return false;
  }
  engine::core::JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    return false;
  }
  const engine::core::JsonValue *root = parser.root();
  if (root == nullptr) {
    return false;
  }
  engine::core::JsonValue settings{};
  if (!parser.get_object_field(*root, "importSettings", &settings)) {
    return false;
  }
  engine::core::JsonValue field{};
  std::int64_t wide = 0;
  if (parser.get_object_field(settings, "meshIndex", &field) &&
      parser.as_int64(field, &wide)) {
    outSettings->meshIndex = static_cast<std::int32_t>(wide);
  }
  if (parser.get_object_field(settings, "primitiveIndex", &field) &&
      parser.as_int64(field, &wide)) {
    outSettings->primitiveIndex = static_cast<std::int32_t>(wide);
  }
  if (parser.get_object_field(settings, "upAxis", &field) &&
      parser.as_int64(field, &wide)) {
    outSettings->upAxis = static_cast<std::int32_t>(wide);
  }
  float scale = 1.0F;
  if (parser.get_object_field(settings, "scaleFactor", &field) &&
      parser.as_float(field, &scale)) {
    outSettings->scaleFactor = scale;
  }
  bool normals = false;
  if (parser.get_object_field(settings, "generateNormals", &field) &&
      parser.as_bool(field, &normals)) {
    outSettings->generateNormals = normals;
  }
  return true;
}

/// Gives one path an identity when it has none. Never touches a sidecar
/// that already exists.
void identify(const std::filesystem::path &path, bool isFolder,
              InitMetaResult *result) {
  const std::string osPath = path.string();
  ct::AssetSidecar existing{};
  switch (ct::read_asset_sidecar(osPath.c_str(), &existing)) {
  case ct::SidecarReadResult::Ok:
    ++result->alreadyIdentified;
    return;
  case ct::SidecarReadResult::Unreadable:
  case ct::SidecarReadResult::Malformed:
    // Refusing here is the point: replacing a sidecar that exists but
    // will not read would hand the asset a new identity and silently
    // break every reference to it. The diagnostic has already named it.
    std::fprintf(stderr,
                 "error: %s has a sidecar that cannot be read; repair or "
                 "delete it rather than letting the migration mint a new "
                 "identity\n",
                 osPath.c_str());
    ++result->failed;
    return;
  case ct::SidecarReadResult::Absent:
    break;
  }

  ct::AssetSidecar sidecar{};
  sidecar.guid = ct::generate_asset_guid();
  if (!ct::asset_guid_is_valid(sidecar.guid)) {
    ++result->failed;
    return;
  }
  sidecar.folder = isFolder;
  if (!isFolder) {
    ct::MeshImportSettings settings{};
    if (recover_settings_from_cooked(path, &settings)) {
      sidecar.hasMeshImport = true;
      sidecar.meshImport = settings;
    }
  }
  if (!ct::write_asset_sidecar(osPath.c_str(), sidecar)) {
    ++result->failed;
    return;
  }
  ++result->created;
}

} // namespace

int run_init_meta(int argc, char **argv) {
  const char *root = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--init-meta") == 0) {
      if ((i + 1) >= argc) {
        std::fprintf(stderr, "error: --init-meta needs a directory\n");
        return 1;
      }
      root = argv[i + 1];
      break;
    }
  }
  if (root == nullptr) {
    return 1;
  }

  std::error_code ec{};
  const std::filesystem::path rootPath(root);
  if (!std::filesystem::is_directory(rootPath, ec) || ec) {
    std::fprintf(stderr, "error: --init-meta: not a directory: %s\n", root);
    return 1;
  }

  InitMetaResult result{};
  // Collected first and sorted, so two machines running the migration
  // over the same tree visit it in the same order and the diff of what
  // they produce differs only in the GUIDs themselves.
  std::vector<std::filesystem::path> folders{};
  std::vector<std::filesystem::path> files{};
  for (std::filesystem::recursive_directory_iterator
           it(rootPath, std::filesystem::directory_options::skip_permission_denied,
              ec),
       end{};
       !ec && (it != end); it.increment(ec)) {
    const std::filesystem::path relative =
        std::filesystem::relative(it->path(), rootPath, ec);
    if (ec || relative.empty() || is_ignored(relative)) {
      ec.clear();
      continue;
    }
    std::error_code kindEc{};
    if (it->is_directory(kindEc) && !kindEc) {
      folders.push_back(it->path());
      continue;
    }
    if (!it->is_regular_file(kindEc) || kindEc) {
      continue;
    }
    if (!needs_identity(it->path())) {
      ++result.skipped;
      continue;
    }
    files.push_back(it->path());
  }
  if (ec) {
    std::fprintf(stderr, "error: --init-meta: could not walk %s\n", root);
    return 1;
  }

  std::sort(folders.begin(), folders.end());
  std::sort(files.begin(), files.end());
  for (const std::filesystem::path &folder : folders) {
    identify(folder, true, &result);
  }
  for (const std::filesystem::path &file : files) {
    identify(file, false, &result);
  }

  std::printf("--init-meta %s: %zu identified, %zu already had one, "
              "%zu not identity-bearing, %zu failed\n",
              root, result.created, result.alreadyIdentified, result.skipped,
              result.failed);
  return (result.failed == 0U) ? 0 : 1;
}
