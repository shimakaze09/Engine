// Declares the atomic file-commit helper authored-file saves route
// through so an interrupted or failed write can never destroy the
// previous valid file.

#pragma once

#include <cstddef>

namespace engine::core {

/// Writes the payload to a uniquely named sibling temporary file (flushed,
/// synced, and closed with every step checked) and atomically renames it
/// over the destination; on any failure the previous destination file is
/// left intact and the temporary is removed. Concurrent writers never
/// share a temporary; concurrent commits to the same destination resolve
/// as last-writer-wins.
bool atomic_write_file(const char *path, const void *data,
                       std::size_t size) noexcept;

} // namespace engine::core
