// Declares how the import settings that apply to an asset are found: its
// own sidecar's block when it has one, else the block for its type in the
// nearest enclosing folder's sidecar inside the project, else the
// defaults. A folder's settings so configure a whole subtree at once, and
// an asset overrides them by carrying a block of its own, whole.
//
// Unity and Godot configure importers per asset and leave subtrees to
// presets or scripts; here a folder configures its subtree directly, so a
// folder's sidecar holds one block per asset type and the nearest one
// wins. An asset's own block replaces the folder's rather
// than merging field by field: what the author sees in the asset's block
// is everything it gets, and an editor save writes the block whole.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/content/asset_import_settings.h"
#include "engine/content/asset_metadata.h"

namespace engine::content {

/// Where the settings an asset gets came from.
enum class ImportSettingsOrigin : std::uint8_t {
  /// No block anywhere: the type's defaults.
  Defaults,
  /// The asset's own sidecar.
  Asset,
  /// An enclosing folder's sidecar; ResolvedImportSettings::folder names it.
  Folder,
};

/// The settings that apply to one asset.
struct ResolvedImportSettings final {
  static constexpr std::size_t kMaxFolderPath = 1024U;

  ImportSettingsKind kind = ImportSettingsKind::None;
  ImportSettingsOrigin origin = ImportSettingsOrigin::Defaults;
  /// The OS path of the folder whose block applies, when origin is Folder;
  /// empty otherwise.
  char folder[kMaxFolderPath] = {};
  /// When resolution fails, the OS path of the asset or folder whose
  /// ".meta" would not read; empty otherwise.
  char unreadable[kMaxFolderPath] = {};
  /// The block of the asset's kind; the others stay at their defaults.
  MeshImportSettings mesh{};
  TextureImportSettings texture{};
  AudioImportSettings audio{};
  /// The newest write time, in file_mtime_ns units, of the sidecars the
  /// settings were resolved through: the asset's own and every folder's
  /// read on the way up. Any of them changing can change the settings, so
  /// a loader that reloads on an edit watches this.
  std::int64_t newestSidecarWriteTime = 0;
};

/// Resolves the settings for the asset at `assetOsPath` into `*out`.
/// Folders are walked from the asset's own up to, not including, the
/// project root (the first directory holding a ".project" file), or to the
/// top of the filesystem outside a project. False, with `*out` at the
/// defaults, `unreadable` naming it and the problem logged by the sidecar
/// reader, when a sidecar on the way is unreadable or malformed: settings
/// are never guessed past a document that will not read.
bool resolve_import_settings(const char *assetOsPath,
                             ResolvedImportSettings *out) noexcept;

/// The newest write time, in file_mtime_ns units, of every sidecar whose
/// change could change the asset's settings: its own and each enclosing
/// folder's up to the project root. Reads no document, so a hot-reload
/// poll can watch settings for the price of a few stats; 0 when none
/// exists.
std::int64_t import_settings_write_time(const char *assetOsPath) noexcept;

} // namespace engine::content
