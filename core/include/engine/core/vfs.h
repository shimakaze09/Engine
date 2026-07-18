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

/// True when the virtual path resolves to an existing file.
bool vfs_file_exists(const char *virtualPath) noexcept;

// Read entire file into a heap-allocated buffer. Caller must call vfs_free().
bool vfs_read_binary(const char *virtualPath,
                     void **outData,
                     std::size_t *outSize) noexcept;

// Read entire text file into a null-terminated heap buffer. Caller must call
// vfs_free().
bool vfs_read_text(const char *virtualPath,
                   char **outText,
                   std::size_t *outSize) noexcept;

/// Writes bytes to the resolved path; false on IO failure.
bool vfs_write_binary(const char *virtualPath,
                      const void *data,
                      std::size_t size) noexcept;

/// Writes text to the resolved path; false on IO failure.
bool vfs_write_text(const char *virtualPath,
                    const char *text,
                    std::size_t size) noexcept;

// Free a buffer returned by vfs_read_binary or vfs_read_text.
void vfs_free(void *buffer) noexcept;

// Return file modification time (platform epoch ticks), or 0 on failure.
std::int64_t vfs_file_mtime(const char *virtualPath) noexcept;

// Resolve a virtual path to the underlying OS path. Returns false if the
// virtual prefix is not mounted or the buffer is too small.
bool vfs_resolve_os_path(const char *virtualPath,
                         char *outBuffer,
                         std::size_t bufferCapacity) noexcept;

} // namespace engine::core
