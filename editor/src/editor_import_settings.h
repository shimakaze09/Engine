// Declares the Assets panel's import-settings sidecar cache: the
// selected asset's .meta is read and parsed once per selection, or
// after the panel rewrites it, instead of once per drawn frame.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::editor {

/// One asset's import sidecar as the panel last read it.
struct ImportSettingsDocument final {
  enum class State : std::uint8_t {
    Missing,    // no <asset>.meta beside the asset
    Unreadable, // present but empty, oversized or not readable
    Malformed,  // read but not a JSON object
    Valid
  };
  State state = State::Missing;
  int meshIndex = 0;
  int primitiveIndex = 0;
  float scaleFactor = 1.0F;
  int upAxis = 1;
  bool generateNormals = false;
  /// The sidecar text as read, NUL-terminated: the base the panel splices
  /// an edited importSettings block into. Documents past this are refused.
  static constexpr std::size_t kMaxDocumentBytes = 64U * 1024U;
  char document[kMaxDocumentBytes + 1U] = {};
  std::size_t documentLength = 0U;
};

/// Returns the sidecar document for `<assetPath>.meta`, reading the
/// file only when assetPath differs from the previous call's or the cache
/// was invalidated; nullptr for a null or empty path. The pointer stays
/// valid until the next call.
const ImportSettingsDocument *
import_settings_for_asset(const char *assetPath) noexcept;

/// Drops the cached document so the next call re-reads it (after the panel
/// rewrote the sidecar, or a recook replaced it).
void invalidate_import_settings_cache() noexcept;

/// Sidecar reads performed since startup; tests pin one read per selection
/// change with the delta between calls.
std::uint64_t import_settings_read_count() noexcept;

} // namespace engine::editor
