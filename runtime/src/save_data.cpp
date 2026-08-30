// Implements the single-slot game save over the platform save directory:
// bounded JSON in, save.json on disk, with explicit-directory variants
// for tests.

#include "engine/runtime/save_data.h"

#include <cstdio>
#include <cstring>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::runtime {

namespace {

constexpr const char *kSaveFileName = "save.json";

/// Builds "<directory>/save.json"; false when the path would truncate.
bool build_save_path(const char *directory, char *out,
                     std::size_t capacity) noexcept {
  const int written =
      std::snprintf(out, capacity, "%s/%s", directory, kSaveFileName);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
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

  // The save directory does not exist before a profile's first save, and
  // a directory created here has its own entry synced — the save's
  // durability would otherwise rest on a directory that might not
  // survive the same power loss.
  if (!core::create_directories_durably(directory)) {
    core::log_message(core::LogLevel::Error, "save",
                      "failed to create the save directory");
    return false;
  }
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

  // The shared reader keeps the distinction PR #319 fixed here: a read
  // that fails part-way is Unreadable, never a successful empty load the
  // caller would overwrite on the next write. Absent stays silent — no
  // save yet is the ordinary first-run case.
  std::size_t read = 0U;
  const core::FileReadResult result =
      core::read_whole_file(path, out, capacity, &read);
  if (result == core::FileReadResult::TooLarge) {
    core::log_message(core::LogLevel::Error, "save",
                      "save file exceeds the read capacity");
    return false;
  }
  if (result == core::FileReadResult::Unreadable) {
    core::log_message(core::LogLevel::Error, "save",
                      "failed to read the save file");
    return false;
  }
  if (result != core::FileReadResult::Ok) {
    return false;
  }
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
