// Implements the editor's cached content-browser asset index: cold
// filesystem walk, extension/content-sniff classification, and
// change-driven filter caching (issue #157).

#include "editor_asset_index.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/engine.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor.asset_index";

// Mirrors the old draw_asset_tree bound: keeps the walk loop-free even on
// filesystems with linked or absurdly deep directory layouts.
constexpr std::size_t kMaxAssetTreeDepth = 32U;

std::vector<AssetIndexEntry> g_index{};
std::uint64_t g_generation = 0ULL;
bool g_built = false;
// OS path the index was built from; "" folder in AssetFilterState/
// asset_index_child_folders means this root, since entry.folder stores the
// real root path (not an empty sentinel) for top-level files.
std::string g_rootOsPath{};

/// Resolves the "" == index root sentinel to the real root OS path.
const char *resolve_folder(const char *requested) noexcept {
  return ((requested == nullptr) || (requested[0] == '\0'))
             ? g_rootOsPath.c_str()
             : requested;
}

/// True when `path` (case-sensitive) ends with `suffix`.
bool has_suffix(const char *path, const char *suffix) noexcept {
  const std::size_t pathLen = std::strlen(path);
  const std::size_t suffixLen = std::strlen(suffix);
  if (suffixLen > pathLen) {
    return false;
  }
  return std::strcmp(path + (pathLen - suffixLen), suffix) == 0;
}

/// Lowercases an ASCII extension in place for case-insensitive comparison.
void lower_ascii(char *text) noexcept {
  for (char *c = text; *c != '\0'; ++c) {
    *c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(*c)));
  }
}

/// Reads a whole small file into a fixed buffer; false on overflow or I/O
/// failure. Used only to sniff ambiguous ".json" files during the cold
/// index rebuild, never per frame.
bool read_small_file(const char *path, char *out, std::size_t capacity,
                     std::size_t *outSize) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t readCount = std::fread(out, 1U, capacity - 1U, file);
  const bool overflow = std::fgetc(file) != EOF;
  const bool hitError = std::ferror(file) != 0;
  std::fclose(file);
  if (overflow || hitError) {
    return false;
  }
  out[readCount] = '\0';
  if (outSize != nullptr) {
    *outSize = readCount;
  }
  return true;
}

/// Distinguishes scene/material/animation-controller ".json" documents by
/// their top-level keys (schemas already documented on save_scene,
/// load_material_asset, and the .animctrl loader respectively).
AssetKind classify_json_by_content(const char *osPath) noexcept {
  char buffer[16U * 1024U] = {};
  std::size_t size = 0U;
  if (!read_small_file(osPath, buffer, sizeof(buffer), &size)) {
    return AssetKind::Other;
  }
  core::JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    return AssetKind::Other;
  }
  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return AssetKind::Other;
  }
  if (parser.get_object_field(*root, "entities") != nullptr) {
    return AssetKind::Scene;
  }
  if ((parser.get_object_field(*root, "states") != nullptr) &&
      (parser.get_object_field(*root, "clips") != nullptr)) {
    return AssetKind::AnimationController;
  }
  if ((parser.get_object_field(*root, "albedo") != nullptr) ||
      (parser.get_object_field(*root, "roughness") != nullptr) ||
      (parser.get_object_field(*root, "metallic") != nullptr) ||
      (parser.get_object_field(*root, "parent") != nullptr)) {
    return AssetKind::Material;
  }
  return AssetKind::Other;
}

/// True for sidecar/internal files the browser hides from authors
/// (import metadata, cook bookkeeping, and their cache directory).
bool is_hidden_from_index(const std::filesystem::path &path) noexcept {
  const std::string generic = path.generic_string();
  if (generic.find("/.thumbnails/") != std::string::npos) {
    return true;
  }
  const std::string filename = path.filename().string();
  return has_suffix(filename.c_str(), ".meta.json") ||
         has_suffix(filename.c_str(), ".cookstamp") ||
         has_suffix(filename.c_str(), ".checksum");
}

/// Builds the VFS virtual path ("<mount>/<relative>") for an indexed entry;
/// leaves outPath empty when the entry falls outside the configured root.
void make_virtual_path(const std::filesystem::path &entryPath,
                       const std::filesystem::path &root, char *outPath,
                       std::size_t capacity) noexcept {
  outPath[0] = '\0';
  std::error_code ec{};
  const std::filesystem::path relative =
      std::filesystem::relative(entryPath, root, ec);
  if (ec || relative.empty() || (*relative.begin() == "..")) {
    return;
  }
  const std::string generic = relative.generic_string();
  std::snprintf(outPath, capacity, "%s/%s", active_config().assetMount,
               generic.c_str());
}

/// Recursively appends indexable files under `dir` into g_index.
void walk_directory(const std::filesystem::path &dir,
                    const std::filesystem::path &root,
                    std::size_t depth) noexcept {
  if (depth >= kMaxAssetTreeDepth) {
    return;
  }
  std::error_code ec{};
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    std::error_code symlinkEc{};
    const bool isSymlink = entry.is_symlink(symlinkEc) && !symlinkEc;
    if (isSymlink) {
      continue;
    }
    std::error_code kindEc{};
    if (entry.is_directory(kindEc) && !kindEc) {
      // Never descend into the generated thumbnail cache directories.
      if (entry.path().filename() != ".thumbnails") {
        walk_directory(entry.path(), root, depth + 1U);
      }
      continue;
    }
    if (!entry.is_regular_file(kindEc) || kindEc) {
      continue;
    }
    if (is_hidden_from_index(entry.path())) {
      continue;
    }

    AssetIndexEntry indexed{};
    const std::string osPathStr = entry.path().string();
    std::snprintf(indexed.osPath, sizeof(indexed.osPath), "%s",
                 osPathStr.c_str());
    const std::string folderStr = entry.path().parent_path().generic_string();
    std::snprintf(indexed.folder, sizeof(indexed.folder), "%s",
                 folderStr.c_str());
    const std::string nameStr = entry.path().filename().string();
    std::snprintf(indexed.name, sizeof(indexed.name), "%s", nameStr.c_str());
    make_virtual_path(entry.path(), root, indexed.virtualPath,
                      sizeof(indexed.virtualPath));
    indexed.kind = classify_asset_kind(indexed.osPath);

    std::error_code thumbEc{};
    const std::filesystem::path thumbPath =
        entry.path().parent_path() / ".thumbnails" /
        (nameStr + ".png");
    indexed.hasThumbnail =
        std::filesystem::is_regular_file(thumbPath, thumbEc) && !thumbEc;

    g_index.push_back(indexed);
  }
}

/// Case-insensitive substring search (ASCII).
bool contains_ci(const char *haystack, const char *needle) noexcept {
  if (needle[0] == '\0') {
    return true;
  }
  std::string h(haystack);
  std::string n(needle);
  lower_ascii(h.data());
  lower_ascii(n.data());
  return h.find(n) != std::string::npos;
}

} // namespace

bool AssetFilterState::operator==(const AssetFilterState &other) const noexcept {
  return (std::strcmp(query, other.query) == 0) &&
         (typeMask == other.typeMask) &&
         (std::strcmp(folder, other.folder) == 0) &&
         (flatSearch == other.flatSearch);
}

AssetKind classify_asset_kind(const char *osPath) noexcept {
  char lowerPath[kMaxAssetIndexPath] = {};
  std::snprintf(lowerPath, sizeof(lowerPath), "%s", osPath);
  lower_ascii(lowerPath);

  if (has_suffix(lowerPath, ".animctrl.json")) {
    return AssetKind::AnimationController;
  }
  if (has_suffix(lowerPath, ".mesh")) {
    return AssetKind::Mesh;
  }
  if (has_suffix(lowerPath, ".png") || has_suffix(lowerPath, ".jpg") ||
      has_suffix(lowerPath, ".jpeg") || has_suffix(lowerPath, ".tga") ||
      has_suffix(lowerPath, ".dds") || has_suffix(lowerPath, ".ktx2")) {
    return AssetKind::Texture;
  }
  if (has_suffix(lowerPath, ".lua")) {
    return AssetKind::Script;
  }
  if (has_suffix(lowerPath, ".wav") || has_suffix(lowerPath, ".ogg") ||
      has_suffix(lowerPath, ".mp3")) {
    return AssetKind::Sound;
  }
  if (has_suffix(lowerPath, ".anim") || has_suffix(lowerPath, ".skel")) {
    return AssetKind::Animation;
  }
  if (has_suffix(lowerPath, ".json")) {
    return classify_json_by_content(osPath);
  }
  return AssetKind::Other;
}

const char *asset_kind_label(AssetKind kind) noexcept {
  switch (kind) {
  case AssetKind::Mesh:
    return "Mesh";
  case AssetKind::Texture:
    return "Texture";
  case AssetKind::Material:
    return "Material";
  case AssetKind::Script:
    return "Script";
  case AssetKind::Scene:
    return "Scene";
  case AssetKind::Animation:
    return "Animation";
  case AssetKind::AnimationController:
    return "Anim Controller";
  case AssetKind::Sound:
    return "Sound";
  case AssetKind::Other:
  default:
    return "Other";
  }
}

bool rebuild_asset_index() noexcept {
  g_index.clear();
  g_built = true;
  ++g_generation;

  const std::filesystem::path root(active_config().editorAssetRoot);
  g_rootOsPath = root.generic_string();
  std::error_code ec{};
  if (!std::filesystem::is_directory(root, ec) || ec) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "asset root not found; index is empty");
    return false;
  }

  walk_directory(root, root, 0U);
  return true;
}

std::size_t asset_index_count() noexcept { return g_index.size(); }

const AssetIndexEntry *asset_index_entry(std::size_t index) noexcept {
  if (index >= g_index.size()) {
    return nullptr;
  }
  return &g_index[index];
}

std::uint64_t asset_index_generation() noexcept { return g_generation; }

bool asset_index_built() noexcept { return g_built; }

bool asset_entry_matches_filter(const AssetIndexEntry &entry,
                                const AssetFilterState &filter) noexcept {
  if ((filter.typeMask & asset_kind_bit(entry.kind)) == 0U) {
    return false;
  }
  if (!filter.flatSearch &&
      (std::strcmp(entry.folder, resolve_folder(filter.folder)) != 0)) {
    return false;
  }
  if (filter.query[0] == '\0') {
    return true;
  }
  return contains_ci(entry.name, filter.query) ||
         contains_ci(entry.virtualPath, filter.query);
}

bool refresh_asset_filter_cache(const AssetFilterState &filter,
                                AssetFilterCache *cache) noexcept {
  if (cache == nullptr) {
    return false;
  }
  if (cache->valid && (cache->appliedGeneration == g_generation) &&
      (cache->appliedFilter == filter)) {
    return false;
  }

  cache->matches.clear();
  cache->matches.reserve(g_index.size());
  for (std::size_t i = 0U; i < g_index.size(); ++i) {
    if (asset_entry_matches_filter(g_index[i], filter)) {
      cache->matches.push_back(i);
    }
  }
  cache->appliedGeneration = g_generation;
  cache->appliedFilter = filter;
  cache->valid = true;
  return true;
}

bool refresh_child_folder_cache(const char *folder,
                                AssetChildFolderCache *cache) noexcept {
  if (cache == nullptr) {
    return false;
  }
  const char *requested = (folder != nullptr) ? folder : "";
  if (cache->valid && (cache->appliedGeneration == g_generation) &&
      (std::strcmp(cache->appliedFolder, requested) == 0)) {
    return false;
  }

  const std::filesystem::path parent(resolve_folder(requested));
  cache->children.clear();
  for (const AssetIndexEntry &entry : g_index) {
    const std::filesystem::path entryFolder(entry.folder);
    if (entryFolder == parent) {
      continue; // direct child file, not a subfolder.
    }
    std::error_code ec{};
    const std::filesystem::path relative =
        std::filesystem::relative(entryFolder, parent, ec);
    if (ec || relative.empty() || (*relative.begin() == "..")) {
      continue;
    }
    const std::string childName = (*relative.begin()).string();
    std::string childStr = (parent / childName).string();

    if (std::find(cache->children.begin(), cache->children.end(),
                  childStr) != cache->children.end()) {
      continue;
    }
    cache->children.push_back(std::move(childStr));
  }

  std::sort(cache->children.begin(), cache->children.end());
  cache->appliedGeneration = g_generation;
  std::snprintf(cache->appliedFolder, sizeof(cache->appliedFolder), "%s",
               requested);
  cache->valid = true;
  return true;
}

AssetOpenAction resolve_asset_open_action(AssetKind kind) noexcept {
  switch (kind) {
  case AssetKind::Mesh:
    return AssetOpenAction::SpawnMesh;
  case AssetKind::Scene:
    return AssetOpenAction::OpenScene;
  case AssetKind::Material:
    return AssetOpenAction::EditMaterial;
  case AssetKind::Texture:
  case AssetKind::Script:
  case AssetKind::Animation:
  case AssetKind::AnimationController:
  case AssetKind::Sound:
  case AssetKind::Other:
  default:
    return AssetOpenAction::SelectOnly;
  }
}

} // namespace engine::editor
