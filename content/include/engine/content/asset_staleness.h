// Public cross-module entry point for the runtime cooked-asset trust
// checks: the CAS tables and the .cookmeta and .cookstamp readers stay
// implemented in the module-private asset_stale_check.cpp; this header is
// the one public declaration so every cooked-asset consumer (the
// renderer's mesh loader, runtime .skel/.anim loaders) routes through the
// same once-per-asset check.

#pragma once

namespace engine::content {

/// Logs a once-per-asset warning when the cooked file's recorded source,
/// or any file its cook stamp records as a dependency (a glTF's external
/// buffers and images), changed after the last cook, naming the changed
/// dependency. Silent for a file that cannot be read -- a shipped build
/// carries no sources -- and for an asset with no sidecar or stamp.
void warn_if_cooked_asset_stale(const char *cookedPath) noexcept;

/// Validates the cooked asset's generation against its .cookstamp output
/// manifest before a load accepts it: every essential output
/// the stamp certifies must exist with matching content bytes, so a torn
/// or mixed cook (new mesh beside an old sidecar, or the reverse) is
/// rejected instead of silently loaded. Presentation outputs under
/// .thumbnails/ only warn. A stamp declaring a newer schema than
/// cook_contract.h's, or a TOOL_VERSION other than its, is rejected: it
/// certifies outputs of a format or import semantics this build was not
/// cooked against. A missing stamp or a pre-manifest schema is accepted
/// after a once-per-asset notice — never-certified content (hand-placed
/// or legacy) stays loadable. Verdicts are cached per path and stamp
/// content, so a recook, which rewrites the stamp, is checked afresh;
/// safe from the streaming worker.
bool cooked_asset_generation_ok(const char *cookedPath) noexcept;

/// Clears the once-per-asset warning memory and the per-session
/// generation verdicts; a run's teardown calls it so a later run in the
/// same process re-checks every asset, and tests call it between cases.
void reset_cooked_asset_stale_warnings() noexcept;

} // namespace engine::content
