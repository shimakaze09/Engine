// Implements import settings resolution: the asset's own block, else the
// nearest enclosing folder's block for its type inside the project, else
// the defaults.

#include "engine/content/import_settings_resolve.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/content/asset_sidecar.h"
#include "engine/content/project_document.h"
#include "engine/core/vfs.h"

namespace engine::content {
namespace {

/// Deeper than any project nests folders; bounds the walk on a path whose
/// parents never end.
constexpr int kMaxFolderDepth = 64;

/// True when `directory` holds a project document: the project root,
/// past which no folder's settings apply.
bool is_project_root(const std::filesystem::path &directory) noexcept {
  std::error_code ec{};
  std::filesystem::directory_iterator it(directory, ec);
  const std::filesystem::directory_iterator end{};
  for (; !ec && (it != end); it.increment(ec)) {
    std::error_code kindEc{};
    if (it->is_regular_file(kindEc) && !kindEc &&
        (it->path().extension() == kProjectFileExtension)) {
      return true;
    }
  }
  return false;
}

/// Copies the block of `kind` from `sidecar` into `*out`; false when the
/// sidecar has none (what was copied is then the defaults).
bool take_block(const AssetSidecar &sidecar, ImportSettingsKind kind,
                ResolvedImportSettings *out) noexcept {
  switch (kind) {
  case ImportSettingsKind::Mesh:
    out->mesh = sidecar.meshImport;
    return sidecar.hasMeshImport;
  case ImportSettingsKind::Texture:
    out->texture = sidecar.textureImport;
    return sidecar.hasTextureImport;
  case ImportSettingsKind::Audio:
    out->audio = sidecar.audioImport;
    return sidecar.hasAudioImport;
  case ImportSettingsKind::None:
    break;
  }
  return false;
}

/// Notes the write time of the sidecar beside `osPath`.
void note_sidecar_time(const char *osPath,
                       ResolvedImportSettings *out) noexcept {
  char sidecarPath[1024] = {};
  if (asset_sidecar_path(osPath, sidecarPath, sizeof(sidecarPath))) {
    const std::int64_t time = core::file_mtime_ns(sidecarPath);
    if (time > out->newestSidecarWriteTime) {
      out->newestSidecarWriteTime = time;
    }
  }
}

/// Calls `visit(folderOsPath)` for each folder enclosing `assetOsPath`,
/// nearest first, up to (not including) the project root; stops early when
/// `visit` returns false.
template <typename Visit>
void walk_settings_folders(const char *assetOsPath, Visit visit) noexcept {
  std::filesystem::path directory =
      std::filesystem::path(assetOsPath).parent_path();
  for (int depth = 0; (depth < kMaxFolderDepth) && !directory.empty();
       ++depth) {
    if (is_project_root(directory) || !visit(directory.string())) {
      return;
    }
    std::filesystem::path parent = directory.parent_path();
    if (parent == directory) {
      return;
    }
    directory = std::move(parent);
  }
}

/// Puts `*out` back at the defaults for `kind`, keeping the sidecar write
/// time noted so far: a reloader still sees a later fix to the file.
void reset_to_defaults(ImportSettingsKind kind,
                       ResolvedImportSettings *out) noexcept {
  const std::int64_t time = out->newestSidecarWriteTime;
  *out = ResolvedImportSettings{};
  out->kind = kind;
  out->newestSidecarWriteTime = time;
}

} // namespace

bool resolve_import_settings(const char *assetOsPath,
                             ResolvedImportSettings *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  *out = ResolvedImportSettings{};
  if ((assetOsPath == nullptr) || (assetOsPath[0] == '\0')) {
    return true;
  }
  const ImportSettingsKind kind =
      import_settings_kind(classify_asset_path(assetOsPath).tag);
  out->kind = kind;
  if (kind == ImportSettingsKind::None) {
    return true;
  }

  AssetSidecar sidecar{};
  note_sidecar_time(assetOsPath, out);
  switch (read_asset_sidecar(assetOsPath, &sidecar)) {
  case SidecarReadResult::Ok:
    if (take_block(sidecar, kind, out)) {
      out->origin = ImportSettingsOrigin::Asset;
      return true;
    }
    break;
  case SidecarReadResult::Absent:
    break;
  case SidecarReadResult::Unreadable:
  case SidecarReadResult::Malformed:
    reset_to_defaults(kind, out);
    std::snprintf(out->unreadable, sizeof(out->unreadable), "%s",
                  assetOsPath);
    return false;
  }

  bool found = false;
  walk_settings_folders(assetOsPath, [&](const std::string &folderPath) {
    AssetSidecar folder{};
    note_sidecar_time(folderPath.c_str(), out);
    switch (read_asset_sidecar(folderPath.c_str(), &folder)) {
    case SidecarReadResult::Ok:
      if (folder.folder && take_block(folder, kind, out)) {
        out->origin = ImportSettingsOrigin::Folder;
        std::snprintf(out->folder, sizeof(out->folder), "%s",
                      folderPath.c_str());
        found = true;
        return false;
      }
      return true;
    case SidecarReadResult::Absent:
      return true;
    case SidecarReadResult::Unreadable:
    case SidecarReadResult::Malformed:
      std::snprintf(out->unreadable, sizeof(out->unreadable), "%s",
                    folderPath.c_str());
      return false;
    }
    return true;
  });
  if (found) {
    return true;
  }
  // No block anywhere, or a sidecar on the way that would not read: the
  // type's defaults.
  char unreadable[sizeof(out->unreadable)] = {};
  std::memcpy(unreadable, out->unreadable, sizeof(unreadable));
  reset_to_defaults(kind, out);
  std::memcpy(out->unreadable, unreadable, sizeof(unreadable));
  return unreadable[0] == '\0';
}

std::int64_t import_settings_write_time(const char *assetOsPath) noexcept {
  ResolvedImportSettings times{};
  if ((assetOsPath == nullptr) || (assetOsPath[0] == '\0')) {
    return 0;
  }
  note_sidecar_time(assetOsPath, &times);
  walk_settings_folders(assetOsPath, [&](const std::string &folderPath) {
    note_sidecar_time(folderPath.c_str(), &times);
    return true;
  });
  return times.newestSidecarWriteTime;
}

} // namespace engine::content
