// Implements the single-slot game save over the project's data directory:
// JSON up to the save ceiling in, save.json on disk, with explicit-directory
// variants for tests.

#include "engine/runtime/save_data.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/project_data.h"

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

/// Says once per process that a save.json from before saves were scoped
/// per project sits in the shared directory. Nothing in it names the
/// project that wrote it, so it is neither read nor replaced: the player
/// moves it into the project directory the log names if it is theirs.
void note_unattributed_legacy_save() noexcept {
  static bool noted = false;
  if (noted) {
    return;
  }
  char sharedDir[1024] = {};
  char legacyPath[1100] = {};
  if (!core::platform_get_save_dir(sharedDir, sizeof(sharedDir)) ||
      !build_save_path(sharedDir, legacyPath, sizeof(legacyPath))) {
    return;
  }
  char probe[1] = {};
  std::size_t size = 0U;
  if (core::read_whole_file(legacyPath, probe, sizeof(probe), &size) ==
      core::FileReadResult::Absent) {
    return;
  }
  noted = true;
  char message[1300] = {};
  std::snprintf(message, sizeof(message),
                "%.1100s predates per-project saves and names no project; "
                "it is left untouched and not loaded",
                legacyPath);
  core::log_message(core::LogLevel::Info, "save", message);
}

/// The directory whose slot is held, empty when none is: one slot per
/// project, and only the slot a load found unusable.
std::array<char, 1024> g_heldDirectory{};

bool held(const char *directory) noexcept {
  return (g_heldDirectory[0] != '\0') &&
         (std::strcmp(g_heldDirectory.data(), directory) == 0);
}

} // namespace

void hold_game_save_in(const char *directory) noexcept {
  if (directory == nullptr) {
    return;
  }
  const int written = std::snprintf(g_heldDirectory.data(),
                                    g_heldDirectory.size(), "%s", directory);
  if ((written < 0) ||
      (static_cast<std::size_t>(written) >= g_heldDirectory.size())) {
    // A directory too long to remember is one build_save_path refuses
    // too, so nothing can be written there anyway.
    g_heldDirectory[0] = '\0';
  }
}

bool game_save_held_in(const char *directory) noexcept {
  return (directory != nullptr) && held(directory);
}

bool discard_game_save_in(const char *directory) noexcept {
  if (directory == nullptr) {
    return false;
  }
  char path[1024] = {};
  if (!build_save_path(directory, path, sizeof(path))) {
    return false;
  }
  std::error_code ec{};
  if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
    if (held(directory)) {
      g_heldDirectory[0] = '\0';
    }
    return !ec;
  }
  char target[1100] = {};
  for (int n = 1; n <= 999; ++n) {
    std::snprintf(target, sizeof(target), "%s.discarded-%d", path, n);
    std::error_code probe{};
    if (std::filesystem::exists(std::filesystem::path(target), probe) ||
        probe) {
      continue;
    }
    std::filesystem::rename(std::filesystem::path(path),
                            std::filesystem::path(target), ec);
    if (ec) {
      break;
    }
    if (held(directory)) {
      g_heldDirectory[0] = '\0';
    }
    char message[1200] = {};
    std::snprintf(message, sizeof(message),
                  "the save was moved aside to %.1100s; the next save starts "
                  "a new file",
                  target);
    core::log_message(core::LogLevel::Info, "save", message);
    return true;
  }
  core::log_message(core::LogLevel::Error, "save",
                    "the save could not be moved aside; it is kept and new "
                    "saves are still refused");
  return false;
}

bool save_game_data_to(const char *directory, const char *json,
                       std::size_t length) noexcept {
  if ((directory == nullptr) || (json == nullptr)) {
    return false;
  }
  if (length > kMaxSaveDataBytes) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "save of %zu bytes exceeds the %zu-byte ceiling; the "
                  "previous save is unchanged",
                  length, kMaxSaveDataBytes);
    core::log_message(core::LogLevel::Error, "save", message);
    return false;
  }

  char path[1024] = {};
  if (!build_save_path(directory, path, sizeof(path))) {
    core::log_message(core::LogLevel::Error, "save",
                      "save path exceeds the buffer");
    return false;
  }
  if (held(directory)) {
    core::log_message(core::LogLevel::Error, "save",
                      "save refused: the save on disk could not be loaded "
                      "and may be the only copy of the player's progress; it "
                      "is kept until it is discarded (engine.discard_save in "
                      "Lua), which moves it aside");
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

SaveReadResult read_game_data_from(const char *directory, char *out,
                                   std::size_t capacity,
                                   std::size_t *outLength) noexcept {
  if (outLength != nullptr) {
    *outLength = 0U;
  }
  if ((directory == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return SaveReadResult::Unreadable;
  }

  char path[1024] = {};
  if (!build_save_path(directory, path, sizeof(path))) {
    return SaveReadResult::Unreadable;
  }

  // A read that fails part-way is Unreadable, never a successful empty
  // load the caller would overwrite on the next write. Absent stays
  // silent — no save yet is the ordinary first-run case.
  std::size_t read = 0U;
  const core::FileReadResult result =
      core::read_whole_file(path, out, capacity, &read);
  if (result == core::FileReadResult::Absent) {
    return SaveReadResult::Absent;
  }
  if (result == core::FileReadResult::TooLarge) {
    core::log_message(core::LogLevel::Error, "save",
                      "save file exceeds the read capacity");
    return SaveReadResult::Unreadable;
  }
  if (result != core::FileReadResult::Ok) {
    core::log_message(core::LogLevel::Error, "save",
                      "failed to read the save file");
    return SaveReadResult::Unreadable;
  }
  if (outLength != nullptr) {
    *outLength = read;
  }
  return SaveReadResult::Ok;
}

bool load_game_data_from(const char *directory, char *out, std::size_t capacity,
                         std::size_t *outLength) noexcept {
  return read_game_data_from(directory, out, capacity, outLength) ==
         SaveReadResult::Ok;
}

bool save_game_data(const char *json, std::size_t length) noexcept {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    core::log_message(core::LogLevel::Error, "save",
                      "project save directory unavailable");
    return false;
  }
  return save_game_data_to(directory, json, length);
}

SaveReadResult read_game_data(char *out, std::size_t capacity,
                              std::size_t *outLength) noexcept {
  if (outLength != nullptr) {
    *outLength = 0U;
  }
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    return SaveReadResult::Absent;
  }
  const SaveReadResult result =
      read_game_data_from(directory, out, capacity, outLength);
  if (result == SaveReadResult::Absent) {
    note_unattributed_legacy_save();
  }
  return result;
}

bool load_game_data(char *out, std::size_t capacity,
                    std::size_t *outLength) noexcept {
  return read_game_data(out, capacity, outLength) == SaveReadResult::Ok;
}

void hold_game_save() noexcept {
  char directory[1024] = {};
  if (core::project_data_dir(directory, sizeof(directory))) {
    hold_game_save_in(directory);
  }
}

bool discard_game_save() noexcept {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    core::log_message(core::LogLevel::Error, "save",
                      "project save directory unavailable");
    return false;
  }
  return discard_game_save_in(directory);
}

} // namespace engine::runtime
