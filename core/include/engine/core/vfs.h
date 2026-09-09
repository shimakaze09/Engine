// Declares vfs types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Initializes the owning system for vfs.
bool initialize_vfs() noexcept;
/// Shuts down the owning system for vfs.
void shutdown_vfs() noexcept;

// Mount a virtual prefix to an OS directory path.
// Example: mount("assets", "d:/dev/Engine/assets")
// Virtual path "assets/main.lua" resolves to "d:/dev/Engine/assets/main.lua".
bool mount(const char *virtualPrefix, const char *osDirectoryPath) noexcept;
/// Removes a mount; false when the prefix is unknown.
bool unmount(const char *virtualPrefix) noexcept;

/// True when a script-supplied path stays inside the VFS jail: non-empty,
/// relative, forward slashes only, no drive designator, no ".." segment.
bool vfs_path_is_jailed(const char *virtualPath) noexcept;

/// True when the virtual path resolves to an existing file.
bool vfs_file_exists(const char *virtualPath) noexcept;

/// Size in bytes of the regular file the virtual path resolves to; false
/// when the path does not resolve or names something other than a regular
/// file. Answers from file metadata without opening the file, so a caller
/// can refuse an oversized input before any of it is read.
bool vfs_file_size(const char *virtualPath, std::uint64_t *outSize) noexcept;

// Read entire file into a heap-allocated buffer. Caller must call vfs_free().
bool vfs_read_binary(const char *virtualPath,
                     void **outData,
                     std::size_t *outSize) noexcept;

/// Outcome of a bounded read: the one status a caller acts on differently
/// from a plain failure is TooLarge, which names a file that exists and
/// reads fine but exceeds the caller's budget.
enum class VfsReadStatus : std::uint8_t {
  Ok,
  Unresolved, // the path does not resolve or the file cannot be opened
  TooLarge,   // the file exceeds maxBytes; nothing was allocated
  IoError,    // the size query, allocation, or read failed
};

/// Reads the whole file into a heap-allocated buffer only when it holds
/// at most maxBytes. The size is taken from the open handle the read then
/// consumes, so the bound applies to the bytes actually allocated and
/// read, not to metadata that could change before the read opens the
/// file. On TooLarge, *outSize carries the measured size (for diagnostics)
/// and *outData stays null. Caller must call vfs_free() on success.
VfsReadStatus vfs_read_binary_bounded(const char *virtualPath,
                                      std::uint64_t maxBytes, void **outData,
                                      std::size_t *outSize) noexcept;

// Read entire text file into a null-terminated heap buffer. Caller must call
// vfs_free().
bool vfs_read_text(const char *virtualPath,
                   char **outText,
                   std::size_t *outSize) noexcept;

/// Atomically replaces the resolved path via a staged sibling write;
/// false on any IO failure (incl. close-flush), previous file kept.
bool vfs_write_binary(const char *virtualPath,
                      const void *data,
                      std::size_t size) noexcept;

/// Atomically replaces the resolved path with text; same contract as
/// vfs_write_binary.
bool vfs_write_text(const char *virtualPath,
                    const char *text,
                    std::size_t size) noexcept;

// Free a buffer returned by vfs_read_binary or vfs_read_text.
void vfs_free(void *buffer) noexcept;

// Return the file's modification time in nanoseconds since the Unix epoch
// on every platform, or 0 on failure. Full platform precision (nanoseconds
// on POSIX, 100 ns on Windows), so two writes inside one second compare
// unequal; values are for change detection, not for display.
std::int64_t vfs_file_mtime(const char *virtualPath) noexcept;

// The same modification time for an OS path that never went through a
// mount (the script watcher's cwd-relative chunks); vfs_file_mtime is this
// after resolution, so every watcher in the tree shares one reading.
std::int64_t file_mtime_ns(const char *osPath) noexcept;

// Resolve a virtual path to the underlying OS path. Returns false if the
// virtual prefix is not mounted or the buffer is too small.
bool vfs_resolve_os_path(const char *virtualPath,
                         char *outBuffer,
                         std::size_t bufferCapacity) noexcept;

} // namespace engine::core
