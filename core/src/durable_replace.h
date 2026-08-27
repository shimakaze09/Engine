// Declares the durable-replacement protocol every authored-file commit
// runs (audit #338): the staged payload is flushed, synced and closed,
// renamed over the destination, and the directory holding that entry is
// synced, so the rename itself survives power loss instead of only the
// bytes behind it. Its companion (audit #357) covers the step before —
// creating the directory the commit writes into, syncing the parent that
// now carries each newly created entry, so the file's durability rests
// on a directory whose own existence is durable too. The filesystem
// operations are injected rather than called directly so the fault
// branches — a directory that cannot be opened, a directory sync that
// fails after the rename already committed, a segment that cannot be
// created — are exercised on the same functions the production path
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

/// Outcome of creating one directory segment.
enum class MakeDirectoryOutcome : std::uint8_t {
  /// The segment did not exist and now does, so its entry in the parent
  /// is new and that parent needs syncing.
  Created = 0,
  /// A directory was already there; nothing was added to the parent.
  AlreadyExists,
  /// The segment could not be created, or the name is taken by
  /// something that is not a directory.
  Failed,
};

/// Outcome of creating a whole directory path. Only Failed means the
/// destination directory is absent; the rest all leave it in place and
/// differ in how well its own entry is proven to survive power loss.
enum class CreateDirectoryOutcome : std::uint8_t {
  /// Every segment was created and each new entry's parent was synced.
  Durable = 0,
  /// Nothing needed creating, so nothing was synced: the path was
  /// already there, and whatever durability it had it keeps.
  AlreadyExists,
  /// Segments were created, but at least one parent could not be opened
  /// or synced. The directory stands; only its resistance to power loss
  /// is degraded, so the caller proceeds and logs the degradation.
  CreatedNotDurable,
  /// Segments were created on a platform that exposes no directory-sync
  /// primitive, so their entries' durability is whatever the filesystem
  /// provides on its own. Windows takes this path, as it does for the
  /// rename (issue #358).
  CreatedDurabilityUnavailable,
  /// A segment could not be created, so the destination directory does
  /// not exist and no commit into it can succeed.
  Failed,
};

/// The filesystem operations the protocol issues. Production passes
/// production_replace_ops(); a test passes a table that records each
/// step and fails whichever one it is proving. Every member must be
/// non-null.
struct ReplaceOps {
  /// Creates one directory segment, distinguishing a directory it
  /// created — whose new entry the parent must sync — from one that was
  /// already there.
  MakeDirectoryOutcome (*make_directory)(const char *path) noexcept;
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

/// The operations the production commit path runs: mkdir (Windows:
/// _mkdir), buffered-file flush, fsync (Windows: _commit), close,
/// std::filesystem::rename, and — on POSIX — an O_RDONLY open plus fsync
/// of the containing directory.
const ReplaceOps &production_replace_ops() noexcept;

/// Creates every missing segment of directoryPath, syncing the parent
/// directory that carries each newly created entry before moving on, so
/// the whole path from an already-durable ancestor down to the
/// destination is durable before a file is committed into it (audit
/// #357). Existing segments cost no sync. Creation stops at the first
/// segment that fails, leaving the segments already created in place.
///
/// A segment ending in ':' is a drive designator, not a directory, and
/// is stepped over rather than created; a leading or repeated separator
/// names no segment and is likewise skipped. A null, empty, or
/// over-long path reports Failed without touching the filesystem.
CreateDirectoryOutcome
durable_create_directories(const char *directoryPath,
                           const ReplaceOps &ops) noexcept;

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
///
/// Resolves POSIX path shapes, which is all the protocol asks of it
/// today: only the POSIX branch of open_parent_directory calls it. It
/// splits on the platform's separators but does not model Windows
/// roots, so the Windows implementation (issue #358) must supply its
/// own handling for at least these shapes rather than inherit this one:
/// "C:/x" yields "C:", the drive's current directory rather than its
/// root; "C:x" has no separator and yields "."  — the current directory
/// of a possibly different drive; and a UNC "\\server\share\x" yields
/// "\\server\share", which is not a syncable directory handle.
bool parent_directory_of(char *dst, std::size_t dstCapacity,
                         const char *filePath) noexcept;

} // namespace engine::core::detail
