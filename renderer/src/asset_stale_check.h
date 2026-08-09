// Runtime cooked-asset staleness diagnostic (issue #81): compares the
// source content hash recorded in a cooked asset's .meta.json sidecar
// against the current source bytes and warns once per asset when they
// diverge, so editing a source without recooking no longer ships stale
// geometry silently. Missing sidecar or source (shipped builds) is silent.

#pragma once

namespace engine::renderer {

/// Logs a once-per-asset warning when the cooked file's recorded source
/// changed after the last cook; silent when no sidecar/source is present.
void warn_if_cooked_asset_stale(const char *cookedPath) noexcept;

/// Clears the once-per-asset warning memory (tests only).
void reset_cooked_asset_stale_warnings() noexcept;

} // namespace engine::renderer
