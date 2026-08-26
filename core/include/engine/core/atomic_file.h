// Declares the atomic file-commit helper authored-file saves route
// through so an interrupted or failed write can never destroy the
// previous valid file.

#pragma once

#include <cstddef>
#include <cstdio>

namespace engine::core {

/// Streams one atomic replacement in checked chunks, so large payloads
/// commit without being double-buffered in memory (review item 9):
/// begin opens a uniquely named sibling temporary, write appends checked
/// chunks (any failure aborts and removes the temporary), commit
/// flushes, syncs, closes, and atomically renames over the destination.
/// abort — also run by the destructor — discards the staged temporary
/// and leaves the destination untouched. One destination per writer at
/// a time; concurrent commits resolve as last-writer-wins.
class AtomicFileWriter final {
public:
  AtomicFileWriter() noexcept = default;
  ~AtomicFileWriter() noexcept;

  AtomicFileWriter(const AtomicFileWriter &) = delete;
  AtomicFileWriter &operator=(const AtomicFileWriter &) = delete;
  AtomicFileWriter(AtomicFileWriter &&) = delete;
  AtomicFileWriter &operator=(AtomicFileWriter &&) = delete;

  /// Opens the staged temporary beside destinationPath; false when a
  /// stage is already open or the paths do not fit.
  bool begin(const char *destinationPath) noexcept;
  /// Appends one checked chunk; a short write aborts the stage.
  bool write(const void *data, std::size_t sizeBytes) noexcept;
  /// Flush + sync + close + atomic rename, then — where the platform
  /// exposes a directory-sync primitive — a parent-directory sync, so
  /// the new directory entry is durable and not just the bytes behind
  /// it. Windows exposes no such primitive and its durable-rename
  /// equivalent is not yet implemented (issue #358), so the entry's
  /// durability there is whatever the filesystem provides on its own.
  /// False leaves the previous destination intact with the temporary
  /// removed. True once the rename has committed: a failure to sync the
  /// directory afterwards is logged as an error rather than reported
  /// here, because the destination already holds the new payload and
  /// the caller must not treat its old file as intact.
  bool commit() noexcept;
  /// Discards the staged temporary without touching the destination.
  void abort() noexcept;

private:
  std::FILE *m_file = nullptr;
  char m_tempPath[1024] = {};
  char m_destinationPath[1024] = {};
};

/// Writes the payload to a uniquely named sibling temporary file (flushed,
/// synced, and closed with every step checked), atomically renames it
/// over the destination, and — where the platform exposes a
/// directory-sync primitive, which Windows does not (issue #358) — syncs
/// the containing directory so the entry survives power loss; on any
/// failure before the rename the previous destination file is left
/// intact and the temporary is removed. Concurrent writers never share a
/// temporary; concurrent commits to the same destination resolve as
/// last-writer-wins.
bool atomic_write_file(const char *path, const void *data,
                       std::size_t size) noexcept;

} // namespace engine::core
