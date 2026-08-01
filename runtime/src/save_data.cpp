// Implements the single-slot game save over the platform save directory:
// bounded JSON in, save.json on disk, with explicit-directory variants
// for tests.

#include "engine/runtime/save_data.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::runtime {

namespace {

constexpr const char *kSaveFileName = "save.json";

/// Creates every missing segment of the directory path (existing
/// segments are fine; only the final result is checked by the write).
void create_directory_recursive(const char *directory) noexcept {
  char partial[1024] = {};
  const std::size_t length = std::strlen(directory);
  if (length >= sizeof(partial)) {
    return;
  }
  for (std::size_t i = 0U; i <= length; ++i) {
    const char c = directory[i];
    if ((c == '/') || (c == '\\') || (c == '\0')) {
      if ((i > 0U) && (partial[i - 1U] != ':')) {
#ifdef _WIN32
        static_cast<void>(_mkdir(partial));
#else
        static_cast<void>(mkdir(partial, 0755));
#endif
      }
      if (c == '\0') {
        break;
      }
    }
    partial[i] = c;
  }
}

/// Builds "<directory>/save.json"; false when the path would truncate.
bool build_save_path(const char *directory, char *out,
                     std::size_t capacity) noexcept {
  const int written =
      std::snprintf(out, capacity, "%s/%s", directory, kSaveFileName);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Opens the save file, portably across CRTs.
std::FILE *open_save_file(const char *path, const char *mode) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, mode) != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, mode);
#endif
  return file;
}

} // namespace

bool save_game_data_to(const char *directory, const char *json,
                       std::size_t length) noexcept {
  if ((directory == nullptr) || (json == nullptr) ||
      (length > kMaxSaveDataBytes)) {
    return false;
  }

  char path[1024] = {};
  if (!build_save_path(directory, path, sizeof(path))) {
    core::log_message(core::LogLevel::Error, "save",
                      "save path exceeds the buffer");
    return false;
  }

  create_directory_recursive(directory);
  if (!core::atomic_write_file(path, json, length)) {
    core::log_message(core::LogLevel::Error, "save",
                      "failed to write save file");
    return false;
  }
  return true;
}

bool load_game_data_from(const char *directory, char *out,
                         std::size_t capacity,
                         std::size_t *outLength) noexcept {
  if ((directory == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return false;
  }
  if (outLength != nullptr) {
    *outLength = 0U;
  }

  char path[1024] = {};
  if (!build_save_path(directory, path, sizeof(path))) {
    return false;
  }

  std::FILE *file = open_save_file(path, "rb");
  if (file == nullptr) {
    return false;
  }

  const std::size_t read = std::fread(out, 1U, capacity - 1U, file);
  const bool overflow = std::fgetc(file) != EOF;
  std::fclose(file);
  if (overflow) {
    core::log_message(core::LogLevel::Error, "save",
                      "save file exceeds the read capacity");
    return false;
  }
  out[read] = '\0';
  if (outLength != nullptr) {
    *outLength = read;
  }
  return true;
}

bool save_game_data(const char *json, std::size_t length) noexcept {
  char directory[1024] = {};
  if (!core::platform_get_save_dir(directory, sizeof(directory))) {
    core::log_message(core::LogLevel::Error, "save",
                      "platform save directory unavailable");
    return false;
  }
  return save_game_data_to(directory, json, length);
}

bool load_game_data(char *out, std::size_t capacity,
                    std::size_t *outLength) noexcept {
  char directory[1024] = {};
  if (!core::platform_get_save_dir(directory, sizeof(directory))) {
    return false;
  }
  return load_game_data_from(directory, out, capacity, outLength);
}

} // namespace engine::runtime
