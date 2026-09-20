// Declares the Assets panel's import-settings cache: the selected
// source's authored ".meta" sidecar is read and parsed once per
// selection, or after the panel rewrites it, instead of once per drawn
// frame.
//
// The authored sidecar is the only place import settings live. The cook
// reads them from there and writes them nowhere, so the panel edits the
// same file the cook will read; editing the cooked record instead would
// let an author change a setting and watch the next cook ignore it.

#pragma once

#include <cstdint>

#include "engine/content/asset_metadata.h"

namespace engine::editor {

/// One source's authored import settings as the panel last read them.
struct ImportSettingsDocument final {
  enum class State : std::uint8_t {
    /// No sidecar beside the asset: it has not been imported yet.
    Missing,
    /// Present but unreadable, or larger than a sidecar can be. The bytes
    /// on disk may still be good, so the panel must not write over them.
    Unreadable,
    /// Read but not a sidecar this build understands.
    Malformed,
    /// Read, with the settings below. `hasSettings` is false for a source
    /// that carries an identity but no settings of its own, which cooks
    /// at the defaults; the panel may still write settings onto it.
    Valid
  };
  State state = State::Missing;
  bool hasSettings = false;
  content::MeshImportSettings settings{};
};

/// Returns the authored sidecar for `assetPath`, reading the file only
/// when the path differs from the previous call's or the cache was
/// invalidated; nullptr for a null or empty path. The pointer stays
/// valid until the next call.
const ImportSettingsDocument *
import_settings_for_asset(const char *assetPath) noexcept;

/// Writes `settings` into the asset's authored sidecar, keeping its
/// identity. False when the asset has no readable sidecar to edit —
/// never invents one, since a sidecar the editor minted would give the
/// asset an identity nobody imported. Invalidates the cache either way.
bool save_import_settings(const char *assetPath,
                          const content::MeshImportSettings &settings) noexcept;

/// Drops the cached document so the next call re-reads it (after the panel
/// rewrote the sidecar, or a recook replaced it).
void invalidate_import_settings_cache() noexcept;

/// Sidecar reads performed since startup; tests pin one read per selection
/// change with the delta between calls.
std::uint64_t import_settings_read_count() noexcept;

} // namespace engine::editor
