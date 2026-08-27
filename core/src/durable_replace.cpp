// Implements the durable-replacement protocol and the production
// syscall table behind it (audit #338). The protocol orders the steps;
// the table supplies the platform primitives, including the POSIX
// parent-directory fsync that makes the rename itself durable.

#include "durable_replace.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace engine::core::detail {

namespace {

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

/// Replaces the destination with the staged temporary in one step.
bool production_rename_file(const char *from, const char *to) noexcept {
  std::error_code renameError{};
  std::filesystem::rename(from, to, renameError);
  return !renameError;
}

/// Opens the directory that holds filePath so its entries can be
/// synced; on Windows no such primitive exists, so the protocol is told
/// durability is unavailable rather than being handed a fake success.
DirectoryHandle production_open_parent_directory(const char *filePath) noexcept {
#ifdef _WIN32
  static_cast<void>(filePath);
  return kDirectoryDurabilityUnavailable;
#else
  char directory[1024] = {};
  if (!parent_directory_of(directory, sizeof(directory), filePath)) {
    return kInvalidDirectoryHandle;
  }

  int fd = -1;
  do {
    fd = ::open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  } while ((fd < 0) && (errno == EINTR));
  return (fd < 0) ? kInvalidDirectoryHandle : static_cast<DirectoryHandle>(fd);
#endif
}

/// Forces the directory's entries — the renamed destination among them
/// — to storage.
bool production_sync_directory(DirectoryHandle handle) noexcept {
#ifdef _WIN32
  static_cast<void>(handle);
  return false;
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
  static_cast<void>(handle);
#else
  static_cast<void>(::close(static_cast<int>(handle)));
#endif
}

/// Discards the staged temporary.
bool production_remove_file(const char *path) noexcept {
  std::error_code removeError{};
  return std::filesystem::remove(path, removeError) && !removeError;
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
    const bool isSeparator = (filePath[i] == '/')
#ifdef _WIN32
                             || (filePath[i] == '\\')
#endif
        ;
    if (isSeparator) {
      separator = i;
      found = true;
    }
  }

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
  static const ReplaceOps ops{
      &production_flush_file,           &production_sync_file,
      &production_close_file,           &production_rename_file,
      &production_open_parent_directory, &production_sync_directory,
      &production_close_directory,      &production_remove_file};
  return ops;
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
