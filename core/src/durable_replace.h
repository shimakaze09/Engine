// Declares the durable-replacement protocol every authored-file commit
// runs (audit #338): the staged payload is flushed, synced and closed,
// renamed over the destination, and the directory holding that entry is
// synced, so the rename itself survives power loss instead of only the
// bytes behind it. The filesystem operations are injected rather than
// called directly so the fault branches — a directory that cannot be
// opened, a directory sync that fails after the rename already
// committed — are exercised on the same function the production path
// runs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace engine::core::detail {

/// Outcome of one replacement. The two middle values both mean the
/// destination now holds the new payload and the previous file is gone,
/// so neither may be reported to a caller as a failed save.
enum class ReplaceOutcome : std::uint8_t {
  /// Renamed, and the directory entry was synced: durable.
  Durable = 0,
  /// Renamed, but the directory entry could not be proven durable
  /// because opening or syncing the containing directory failed. The
  /// replacement stands; only its resistance to power loss is degraded,
  /// so the caller reports success and the protocol's user logs the
  /// degradation rather than swallowing it.
  ReplacedNotDurable,
  /// Renamed on a platform that exposes no directory-sync primitive, so
  /// the entry's durability is whatever the filesystem provides on its
  /// own. Windows takes this path: its durable-rename equivalent
  /// (MOVEFILE_WRITE_THROUGH / ReplaceFile) is not implemented here and
  /// stays open scope on audit #338.
  ReplacedDurabilityUnavailable,
  /// The destination was never touched and the temporary was removed.
  Failed,
};

/// Directory handle held only between DirectoryOpen and DirectoryClose.
using DirectoryHandle = int;
/// Returned by open_parent_directory when the directory could not be
/// opened; the replacement stands but is not proven durable.
inline constexpr DirectoryHandle kInvalidDirectoryHandle = -1;
/// Returned by open_parent_directory when the platform exposes no
/// directory-sync primitive, so no open is attempted at all.
inline constexpr DirectoryHandle kDirectoryDurabilityUnavailable = -2;

/// The filesystem operations the protocol issues. Production passes
/// production_replace_ops(); a test passes a table that records each
/// step and fails whichever one it is proving. Every member must be
/// non-null.
struct ReplaceOps {
  /// Pushes buffered payload bytes to the operating system.
  bool (*flush_file)(std::FILE *file) noexcept;
  /// Forces the flushed payload to storage.
  bool (*sync_file)(std::FILE *file) noexcept;
  /// Closes the staged file; runs even after an earlier step failed.
  bool (*close_file)(std::FILE *file) noexcept;
  /// Atomically replaces the destination with the staged temporary.
  bool (*rename_file)(const char *from, const char *to) noexcept;
  /// Opens the directory containing filePath for syncing, or returns
  /// kInvalidDirectoryHandle / kDirectoryDurabilityUnavailable.
  DirectoryHandle (*open_parent_directory)(const char *filePath) noexcept;
  /// Forces the directory's entries to storage.
  bool (*sync_directory)(DirectoryHandle handle) noexcept;
  /// Releases a handle from open_parent_directory.
  void (*close_directory)(DirectoryHandle handle) noexcept;
  /// Discards the staged temporary after a failed replacement.
  bool (*remove_file)(const char *path) noexcept;
};

/// The operations the production commit path runs: buffered-file flush,
/// fsync (Windows: _commit), close, std::filesystem::rename, and — on
/// POSIX — an O_RDONLY open plus fsync of the containing directory.
const ReplaceOps &production_replace_ops() noexcept;

/// Runs the protocol over an open staged file. The file is closed in
/// every outcome, and a failure before the rename removes the temporary
/// and leaves the destination untouched. A null argument reports Failed
/// without touching the filesystem.
ReplaceOutcome durable_replace(std::FILE *file, const char *tempPath,
                               const char *destinationPath,
                               const ReplaceOps &ops) noexcept;

/// Copies the directory part of filePath into dst — "." when the path
/// names a bare file and "/" when it sits at the root. False (with dst
/// left empty) when the arguments are null or the result does not fit.
bool parent_directory_of(char *dst, std::size_t dstCapacity,
                         const char *filePath) noexcept;

} // namespace engine::core::detail
