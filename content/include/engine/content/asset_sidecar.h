// Declares the authored source-side sidecar: the file that owns an
// asset's persistent identity. "<asset>.meta" sits beside the asset it
// names, the same convention Unity uses, and a folder gets one too.
//
// The sidecar is authored data, not derived. It is committed, a human may
// edit it, and it is written only inside an explicit import, migration or
// Duplicate transaction — never by a read, a load or a cook. The cook's
// own record is ".cookmeta" and is regenerated freely; nothing there is
// identity.
//
// Reading reports why it failed rather than returning a default, because
// the three failures need different handling: an asset with no sidecar
// yet is waiting for an import, an unreadable one must not be overwritten
// with a fresh identity, and a malformed one is a repair job. Silently
// generating an identity for any of them would rename the asset behind
// the author's back and break every reference to it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/content/asset_identity.h"

namespace engine::content {

/// Schema of the sidecar document. Bumped when its fields change; a
/// reader refuses a version it does not know rather than guessing.
inline constexpr std::uint32_t kAssetSidecarSchemaVersion = 1U;

/// Longest sidecar document this reader accepts. A sidecar holds an
/// identity and a handful of settings; anything larger is not one.
inline constexpr std::size_t kMaxAssetSidecarBytes = 64U * 1024U;

/// The authored sidecar's contents.
struct AssetSidecar final {
  std::uint32_t schemaVersion = kAssetSidecarSchemaVersion;
  /// The asset's persistent identity. Survives rename, move, a case-only
  /// rename and every content edit.
  AssetGuid guid{};
  /// True for a directory's sidecar. A folder has an identity so it can
  /// be renamed without breaking what hangs off it, and so per-folder
  /// import settings have somewhere to live.
  bool folder = false;
};

/// Why a sidecar read did not produce a sidecar.
enum class SidecarReadResult : std::uint8_t {
  Ok,
  /// Nothing at the path. The ordinary state of an asset that has not
  /// been imported yet; never a fault by itself.
  Absent,
  /// Present but could not be read, or larger than a sidecar can be. The
  /// bytes on disk may still be good, so the caller must not write over
  /// them.
  Unreadable,
  /// Read, but not a sidecar this schema understands: bad JSON, a missing
  /// or malformed guid, or a schema version from the future. A repair
  /// job, never a reason to mint a new identity.
  Malformed,
};

/// Builds "<assetOsPath>.meta" into `out`; false with `out` emptied when
/// the argument is null or the result does not fit whole.
bool asset_sidecar_path(const char *assetOsPath, char *out,
                        std::size_t capacity) noexcept;

/// Reads the sidecar beside `assetOsPath`. `*out` is left untouched for
/// every result but Ok.
SidecarReadResult read_asset_sidecar(const char *assetOsPath,
                                     AssetSidecar *out) noexcept;

/// Writes the sidecar beside `assetOsPath` through a staged atomic
/// replacement, so an interrupted write leaves the previous identity
/// intact rather than a truncated file. Refuses a nil guid: a sidecar
/// that names no identity is worse than none, because a reader would
/// take it as authoritative.
///
/// Call this ONLY from an import, migration or Duplicate transaction.
bool write_asset_sidecar(const char *assetOsPath,
                         const AssetSidecar &sidecar) noexcept;

} // namespace engine::content
