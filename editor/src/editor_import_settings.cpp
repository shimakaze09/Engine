// Implements the Assets panel's import-settings cache over the authored
// sidecar reader and writer in content, so the panel and the cook agree
// on where settings live by construction rather than by convention.

#include "editor_import_settings.h"

#include <cstring>

#include "engine/content/asset_sidecar.h"

namespace engine::editor {

namespace {

/// The last asset read, its document, and the read counter the tests
/// use to pin one read per selection rather than one per frame.
struct ImportSettingsCache final {
  char assetPath[1024] = {};
  ImportSettingsDocument document{};
  bool valid = false;
  std::uint64_t reads = 0ULL;
};

ImportSettingsCache g_cache{};

/// Maps a sidecar read outcome onto the panel's document state.
ImportSettingsDocument::State
state_for(content::SidecarReadResult result) noexcept {
  switch (result) {
  case content::SidecarReadResult::Ok:
    return ImportSettingsDocument::State::Valid;
  case content::SidecarReadResult::Absent:
    return ImportSettingsDocument::State::Missing;
  case content::SidecarReadResult::Unreadable:
    return ImportSettingsDocument::State::Unreadable;
  case content::SidecarReadResult::Malformed:
    return ImportSettingsDocument::State::Malformed;
  }
  return ImportSettingsDocument::State::Malformed;
}

void read_into_cache(const char *assetPath) noexcept {
  ++g_cache.reads;
  g_cache.document = ImportSettingsDocument{};

  content::AssetSidecar sidecar{};
  const content::SidecarReadResult result =
      content::read_asset_sidecar(assetPath, &sidecar);
  g_cache.document.state = state_for(result);
  if (result != content::SidecarReadResult::Ok) {
    return;
  }
  g_cache.document.hasSettings = sidecar.hasMeshImport;
  g_cache.document.settings = sidecar.meshImport;
}

} // namespace

const ImportSettingsDocument *
import_settings_for_asset(const char *assetPath) noexcept {
  if ((assetPath == nullptr) || (assetPath[0] == '\0')) {
    return nullptr;
  }
  const std::size_t length = std::strlen(assetPath);
  if (length >= sizeof(g_cache.assetPath)) {
    return nullptr;
  }

  if (g_cache.valid && (std::strcmp(g_cache.assetPath, assetPath) == 0)) {
    return &g_cache.document;
  }

  std::memcpy(g_cache.assetPath, assetPath, length + 1U);
  read_into_cache(assetPath);
  g_cache.valid = true;
  return &g_cache.document;
}

bool save_import_settings(
    const char *assetPath,
    const content::MeshImportSettings &settings) noexcept {
  if ((assetPath == nullptr) || (assetPath[0] == '\0')) {
    return false;
  }

  // Read first, so the write carries the asset's existing identity
  // forward. A sidecar that is absent or will not read is left alone:
  // minting one here would give the asset an identity nobody imported,
  // and overwriting one would replace an identity references point at.
  content::AssetSidecar sidecar{};
  if (content::read_asset_sidecar(assetPath, &sidecar) !=
      content::SidecarReadResult::Ok) {
    invalidate_import_settings_cache();
    return false;
  }
  sidecar.hasMeshImport = true;
  sidecar.meshImport = settings;

  const bool written = content::write_asset_sidecar(assetPath, sidecar);
  // Whether or not the write landed, the next frame re-reads the sidecar
  // as it is on disk.
  invalidate_import_settings_cache();
  return written;
}

void invalidate_import_settings_cache() noexcept { g_cache.valid = false; }

std::uint64_t import_settings_read_count() noexcept { return g_cache.reads; }

} // namespace engine::editor
