// Implements the durable-replacement protocol, its directory-creation
// companion, and the production syscall table behind both. The protocols
// order the steps; the table supplies the platform primitives, including
// the POSIX parent-directory fsync that makes a rename — and a freshly
// created directory's own entry — durable. Every primitive takes the
// caller's fixed char buffer straight to the platform call: the table
// runs inside noexcept commit and abort paths, and a std::filesystem::path
// conversion would allocate there, turning an allocation failure into
// process termination under the no-exception build.

#include "durable_replace.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace engine::core::detail {

namespace {

/// Reports whether c separates path segments on this platform. POSIX
/// treats only '/' as a separator — a backslash is an ordinary character
/// in a POSIX filename — while Windows accepts either.
bool is_path_separator(char c) noexcept {
#ifdef _WIN32
  return (c == '/') || (c == '\\');
#else
  return c == '/';
#endif
}

/// Flushes the C stream's buffered bytes to the operating system.
bool production_flush_file(std::FILE *file) noexcept {
  return std::fflush(file) == 0;
}

/// Forces the payload behind the stream to storage.
bool production_sync_file(std::FILE *file) noexcept {
#ifdef _WIN32
  return _commit(_fileno(file)) == 0;
#else
  // EINTR leaves the sync incomplete rather than failed, so retry
  // instead of reporting a durability failure the caller cannot act on.
  int result = 0;
  do {
    result = fsync(fileno(file));
  } while ((result != 0) && (errno == EINTR));
  return result == 0;
#endif
}

/// Closes the staged stream.
bool production_close_file(std::FILE *file) noexcept {
  return std::fclose(file) == 0;
}

/// Replaces the destination with the staged temporary in one step. The
/// temporary is a sibling of the destination, so the move never crosses
/// a volume: on Windows the replace-existing move is therefore always the
/// atomic same-volume rename, and no copy fallback is requested because a
/// copy could leave the destination half-written.
///
/// MOVEFILE_WRITE_THROUGH holds the call until the move is on the disk,
/// which is what makes the new directory entry durable without the
/// parent-directory sync POSIX needs. Its documented caveat — that the
/// flush is guaranteed for a move performed as a copy and delete — does
/// not narrow the guarantee here: MOVEFILE_COPY_ALLOWED is deliberately
/// absent, so this is always the same-volume rename the general
/// statement covers.
bool production_rename_file(const char *from, const char *to) noexcept {
#ifdef _WIN32
  return MoveFileExA(from, to,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return ::rename(from, to) == 0;
#endif
}

/// Opens the directory that holds filePath so its entries can be
/// synced. A Windows directory handle requires FILE_FLAG_BACKUP_SEMANTICS,
/// and FlushFileBuffers requires write access on the handle it is given,
/// so the open asks for GENERIC_WRITE: a read-only handle opens and then
/// refuses the flush, which would report a degradation the filesystem
/// never had. Sharing stays wide so holding the handle never blocks
/// another writer working in the same directory. A directory the caller
/// may not write — a drive root, a protected system path — refuses the
/// open, and the protocol reports that as a degraded outcome rather than
/// a failed save.
DirectoryHandle production_open_parent_directory(const char *filePath) noexcept {
  char directory[1024] = {};
  if (!parent_directory_of(directory, sizeof(directory), filePath)) {
    return kInvalidDirectoryHandle;
  }

#ifdef _WIN32
  const HANDLE handle =
      CreateFileA(directory, GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return kInvalidDirectoryHandle;
  }
  return reinterpret_cast<DirectoryHandle>(handle);
#else
  int fd = -1;
  do {
    fd = ::open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  } while ((fd < 0) && (errno == EINTR));
  return (fd < 0) ? kInvalidDirectoryHandle : static_cast<DirectoryHandle>(fd);
#endif
}

/// Forces the directory's entries — the renamed destination among them
/// — to storage. A network filesystem may refuse the request outright,
/// which is reported as a degradation rather than retried or ignored.
bool production_sync_directory(DirectoryHandle handle) noexcept {
#ifdef _WIN32
  return FlushFileBuffers(reinterpret_cast<HANDLE>(handle)) != 0;
#else
  int result = 0;
  do {
    result = fsync(static_cast<int>(handle));
  } while ((result != 0) && (errno == EINTR));
  return result == 0;
#endif
}

/// Releases the directory handle.
void production_close_directory(DirectoryHandle handle) noexcept {
#ifdef _WIN32
  static_cast<void>(CloseHandle(reinterpret_cast<HANDLE>(handle)));
#else
  static_cast<void>(::close(static_cast<int>(handle)));
#endif
}

/// Reports whether the path names an existing directory, so a name
/// already taken by a file is refused instead of being mistaken for a
/// directory that was already there. Uses stat rather than
/// std::filesystem so the check allocates nothing.
bool path_is_directory(const char *path) noexcept {
#ifdef _WIN32
  struct _stat info = {};
  return (_stat(path, &info) == 0) && ((info.st_mode & _S_IFDIR) != 0);
#else
  struct stat info = {};
  return (::stat(path, &info) == 0) && S_ISDIR(info.st_mode);
#endif
}

/// Creates one directory segment. EEXIST alone does not mean success:
/// mkdir reports it for a plain file of the same name too, so the kind
/// of the existing entry decides.
MakeDirectoryOutcome production_make_directory(const char *path) noexcept {
#ifdef _WIN32
  const int result = _mkdir(path);
#else
  int result = 0;
  do {
    result = ::mkdir(path, 0755);
  } while ((result != 0) && (errno == EINTR));
#endif
  if (result == 0) {
    return MakeDirectoryOutcome::Created;
  }
  if (errno == EEXIST) {
    return path_is_directory(path) ? MakeDirectoryOutcome::AlreadyExists
                                   : MakeDirectoryOutcome::Failed;
  }
  return MakeDirectoryOutcome::Failed;
}

/// Discards the staged temporary; false when nothing was removed, an
/// absent path included.
bool production_remove_file(const char *path) noexcept {
#ifdef _WIN32
  return DeleteFileA(path) != 0;
#else
  return ::unlink(path) == 0;
#endif
}

} // namespace

bool parent_directory_of(char *dst, std::size_t dstCapacity,
                         const char *filePath) noexcept {
  if ((dst == nullptr) || (dstCapacity == 0U)) {
    return false;
  }
  dst[0] = '\0';
  if (filePath == nullptr) {
    return false;
  }

  std::size_t separator = 0U;
  bool found = false;
  for (std::size_t i = 0U; filePath[i] != '\0'; ++i) {
    if (is_path_separator(filePath[i])) {
      separator = i;
      found = true;
    }
  }

#ifdef _WIN32
  // "C:/x" holds its entry in the drive's root. Copying the prefix would
  // yield "C:", which names that drive's current directory instead — a
  // different directory, whose sync would prove nothing about this entry
  // while still reporting success.
  if (found && (separator == 2U) && (filePath[1] == ':')) {
    if (dstCapacity < 4U) {
      return false;
    }
    dst[0] = filePath[0];
    dst[1] = ':';
    dst[2] = filePath[2];
    dst[3] = '\0';
    return true;
  }
  // "C:x" is drive-relative with no separator: the entry lands in that
  // drive's current directory, which "." would only name when C: is also
  // the current drive.
  if (!found && (filePath[0] != '\0') && (filePath[1] == ':')) {
    if (dstCapacity < 3U) {
      return false;
    }
    dst[0] = filePath[0];
    dst[1] = ':';
    dst[2] = '\0';
    return true;
  }
#endif

  if (!found) {
    // A bare file name is created in the working directory, which is
    // the directory whose entry must be synced.
    if (dstCapacity < 2U) {
      return false;
    }
    dst[0] = '.';
    dst[1] = '\0';
    return true;
  }

  // A separator at index 0 leaves no directory name to copy: the entry
  // lives in the root directory, which is that separator itself.
  const std::size_t length = (separator == 0U) ? 1U : separator;
  if (length >= dstCapacity) {
    return false;
  }
  std::memcpy(dst, filePath, length);
  dst[length] = '\0';
  return true;
}

const ReplaceOps &production_replace_ops() noexcept {
  static const ReplaceOps ops{&production_make_directory,
                              &production_flush_file,
                              &production_sync_file,
                              &production_close_file,
                              &production_rename_file,
                              &production_open_parent_directory,
                              &production_sync_directory,
                              &production_close_directory,
                              &production_remove_file,
#ifdef _WIN32
                              true};
#else
                              false};
#endif
  return ops;
}

CreateDirectoryOutcome
durable_create_directories(const char *directoryPath,
                           const ReplaceOps &ops) noexcept {
  if (directoryPath == nullptr) {
    return CreateDirectoryOutcome::Failed;
  }
  char partial[1024] = {};
  const std::size_t length = std::strlen(directoryPath);
  if ((length == 0U) || (length >= sizeof(partial))) {
    return CreateDirectoryOutcome::Failed;
  }

  bool createdAny = false;
  bool notDurable = false;
  bool durabilityUnavailable = false;

  // A Windows UNC path opens with two separators, and its first two
  // segments are the server and the share. Neither is a directory any
  // process can create: the server segment alone is not even a complete
  // path, so mkdir refuses it with something other than EEXIST and the
  // walk would report the whole path unreachable. The share root is the
  // already-existing ancestor the walk starts from, exactly as a drive
  // designator is on a local path.
  //
  // The two other prefixes that open with two separators are excluded by
  // their first segment: "\\?\" (extended length) and "\\.\" (device).
  // Their second segment is a real directory, so skipping it would step
  // over a segment that has to be created. They are not supported here
  // and still report Failed, which is the honest answer; silently not
  // creating part of the path would not be.
  std::size_t rootSegmentsToSkip = 0U;
#ifdef _WIN32
  if ((length > 2U) && is_path_separator(directoryPath[0]) &&
      is_path_separator(directoryPath[1]) && (directoryPath[2] != '?') &&
      (directoryPath[2] != '.')) {
    rootSegmentsToSkip = 2U;
  }
#endif

  for (std::size_t i = 0U; i <= length; ++i) {
    const char c = directoryPath[i];
    // The separator set is the one parent_directory_of resolves against,
    // so a character that ends a segment here also ends one there. They
    // must agree: this walk hands that function the very prefixes it
    // builds, and on POSIX a backslash is part of a name rather than a
    // boundary.
    const bool boundary = is_path_separator(c) || (c == '\0');
    if (boundary) {
      const char previous = (i > 0U) ? directoryPath[i - 1U] : '\0';
      // Index 0 names no segment (a leading separator is the root), a
      // repeated separator repeats the segment just handled, and a
      // drive designator is not a directory that can be created.
      bool namesSegment =
          (i > 0U) && (previous != ':') && !is_path_separator(previous);
      if (namesSegment && (rootSegmentsToSkip > 0U)) {
        --rootSegmentsToSkip;
        namesSegment = false;
      }
      if (namesSegment) {
        partial[i] = '\0';
        const MakeDirectoryOutcome made = ops.make_directory(partial);
        if (made == MakeDirectoryOutcome::Failed) {
          return CreateDirectoryOutcome::Failed;
        }
        if (made == MakeDirectoryOutcome::Created) {
          createdAny = true;
          // The new entry lives in this segment's parent, so that is the
          // directory whose entries must reach storage — syncing the
          // segment itself would prove only that its (empty) contents
          // are durable, not that the segment exists at all.
          const DirectoryHandle parent = ops.open_parent_directory(partial);
          if (parent == kDirectoryDurabilityUnavailable) {
            durabilityUnavailable = true;
          } else if (parent == kInvalidDirectoryHandle) {
            notDurable = true;
          } else {
            if (!ops.sync_directory(parent)) {
              notDurable = true;
            }
            ops.close_directory(parent);
          }
        }
      }
      if (c == '\0') {
        break;
      }
    }
    partial[i] = c;
  }

  if (!createdAny) {
    return CreateDirectoryOutcome::AlreadyExists;
  }
  // A sync that was attempted and failed is the more specific report, so
  // it outranks a platform that never offered the primitive at all.
  if (notDurable) {
    return CreateDirectoryOutcome::CreatedNotDurable;
  }
  if (durabilityUnavailable) {
    return CreateDirectoryOutcome::CreatedDurabilityUnavailable;
  }
  return CreateDirectoryOutcome::Durable;
}

ReplaceOutcome durable_replace(std::FILE *file, const char *tempPath,
                               const char *destinationPath,
                               const ReplaceOps &ops) noexcept {
  if ((file == nullptr) || (tempPath == nullptr) ||
      (destinationPath == nullptr)) {
    return ReplaceOutcome::Failed;
  }

  bool staged = ops.flush_file(file);
  if (staged) {
    staged = ops.sync_file(file);
  }
  // The close runs even after a failed flush or sync: the descriptor is
  // released either way, and its own failure still fails the commit.
  staged = ops.close_file(file) && staged;

  if (!staged) {
    static_cast<void>(ops.remove_file(tempPath));
    return ReplaceOutcome::Failed;
  }

  if (!ops.rename_file(tempPath, destinationPath)) {
    static_cast<void>(ops.remove_file(tempPath));
    return ReplaceOutcome::Failed;
  }

  // Past this point the destination already holds the new payload and
  // the previous file is gone, so no later failure may report Failed —
  // the caller would take it as "the old file is still intact".
  if (ops.rename_is_durable) {
    // The rename put the entry on storage itself, so there is nothing
    // left for a parent sync to establish. Syncing anyway would turn a
    // filesystem that refuses the request — a network share does — into
    // a reported degradation of a replacement that is already durable.
    return ReplaceOutcome::Durable;
  }

  const DirectoryHandle directory = ops.open_parent_directory(destinationPath);
  if (directory == kDirectoryDurabilityUnavailable) {
    return ReplaceOutcome::ReplacedDurabilityUnavailable;
  }
  if (directory == kInvalidDirectoryHandle) {
    return ReplaceOutcome::ReplacedNotDurable;
  }

  const bool synced = ops.sync_directory(directory);
  ops.close_directory(directory);
  return synced ? ReplaceOutcome::Durable : ReplaceOutcome::ReplacedNotDurable;
}

} // namespace engine::core::detail
