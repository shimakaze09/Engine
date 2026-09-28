// Implements the editor's project data files: the per-kind subdirectory
// and the timestamped name that never replaces an existing file.

#include "editor_project_files.h"

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace engine::editor {

namespace {

constexpr int kMaxNameSuffix = 99;

} // namespace

// Paths are UTF-8, which every executable's narrow code page is (on
// Windows through its manifest), so a path converts as it stands.
bool project_file_exists(const char *path, void *) noexcept {
  std::error_code ec{};
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

std::tm local_time_now() noexcept {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  return local;
}

bool next_timestamped_path(const char *directory, const char *stem,
                           const char *extension, const std::tm &time,
                           ProjectFileExistsFn exists, void *userData,
                           char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((directory == nullptr) || (stem == nullptr) || (extension == nullptr) ||
      (exists == nullptr)) {
    return false;
  }
  char stamp[32] = {};
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
                time.tm_hour, time.tm_min, time.tm_sec);
  for (int suffix = 1; suffix <= kMaxNameSuffix; ++suffix) {
    char suffixText[8] = {};
    if (suffix > 1) {
      std::snprintf(suffixText, sizeof(suffixText), "-%d", suffix);
    }
    const int written = std::snprintf(out, capacity, "%s/%s-%s%s%s", directory,
                                      stem, stamp, suffixText, extension);
    if ((written < 0) || (static_cast<std::size_t>(written) >= capacity)) {
      out[0] = '\0';
      return false;
    }
    if (!exists(out, userData)) {
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

bool project_data_subdirectory(const char *name, char *out,
                               std::size_t capacity) noexcept {
  if ((name == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return false;
  }
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    return false; // project_data_dir logged why
  }
  const int written = std::snprintf(out, capacity, "%s/%s", directory, name);
  if ((written < 0) || (static_cast<std::size_t>(written) >= capacity)) {
    out[0] = '\0';
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "the project data path is too long for %s/", name);
    core::log_message(core::LogLevel::Error, "editor", message);
    return false;
  }
  if (!core::create_directories_durably(out)) {
    char message[1100] = {};
    std::snprintf(message, sizeof(message), "cannot create %s", out);
    core::log_message(core::LogLevel::Error, "editor", message);
    out[0] = '\0';
    return false;
  }
  return true;
}

} // namespace engine::editor
