// Implements the atomic authored-file write: the payload lands in a
// sibling temporary file that is flushed, synced, and closed with every
// step checked, then atomically renamed over the destination, so an
// interrupted save, a full disk, or a failed close can never destroy the
// previous valid file.

#include "engine/core/atomic_file.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace engine::core {

namespace {

/// Per-call temporary suffix counter so concurrent writers to the same
/// destination never share a temporary; the final rename still resolves
/// concurrent commits as last-writer-wins on the destination.
std::atomic<std::uint32_t> g_tempSerial{0U};

/// Process id for the temporary-file suffix.
unsigned long current_process_id() noexcept {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

} // namespace

bool atomic_write_file(const char *path, const void *data,
                       std::size_t size) noexcept {
  if ((path == nullptr) || (data == nullptr) || (size == 0U)) {
    return false;
  }

  const std::uint32_t serial =
      g_tempSerial.fetch_add(1U, std::memory_order_relaxed);
  char tempPath[1024];
  const int formatted =
      std::snprintf(tempPath, sizeof(tempPath), "%s.new.%lu.%u", path,
                    current_process_id(), serial);
  if ((formatted <= 0) ||
      (static_cast<std::size_t>(formatted) >= sizeof(tempPath))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, tempPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(tempPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  bool ok = std::fwrite(data, 1U, size, file) == size;
  ok = ok && (std::fflush(file) == 0);
#ifdef _WIN32
  ok = ok && (_commit(_fileno(file)) == 0);
#else
  ok = ok && (fsync(fileno(file)) == 0);
#endif
  ok = (std::fclose(file) == 0) && ok;

  if (ok) {
    std::error_code renameError{};
    std::filesystem::rename(tempPath, path, renameError);
    ok = !renameError;
  }
  if (!ok) {
    std::error_code removeError{};
    std::filesystem::remove(tempPath, removeError);
  }
  return ok;
}

} // namespace engine::core
