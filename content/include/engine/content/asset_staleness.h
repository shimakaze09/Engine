// Public cross-module entry point for the runtime cooked-asset staleness
// diagnostic (issue #81, extended to skeleton/animation callers by #91): the
// CAS table and .meta.json sidecar reader stay implemented in the module-
// private asset_stale_check.cpp; this header is the one public declaration
// so every cooked-asset consumer (renderer mesh/texture loaders, runtime
// .skel/.anim loaders) routes through the same once-per-asset check.

#pragma once

namespace engine::content {

/// Logs a once-per-asset warning when the cooked file's recorded source
/// changed after the last cook; silent when no sidecar/source is present.
void warn_if_cooked_asset_stale(const char *cookedPath) noexcept;

/// Clears the once-per-asset warning memory (tests only).
void reset_cooked_asset_stale_warnings() noexcept;

} // namespace engine::content
