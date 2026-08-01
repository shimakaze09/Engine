// Implements the atomic authored-file write: the payload lands in a
// sibling temporary file that is flushed, synced, and closed with every
// step checked, then atomically renamed over the destination, so an
// interrupted save, a full disk, or a failed close can never destroy the
// previous valid file.

#include "engine/core/atomic_file.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace engine::core {

bool atomic_write_file(const char *path, const void *data,
                       std::size_t size) noexcept {
  if ((path == nullptr) || (data == nullptr) || (size == 0U)) {
    return false;
  }

  char tempPath[1024];
  const int formatted =
      std::snprintf(tempPath, sizeof(tempPath), "%s.new", path);
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
