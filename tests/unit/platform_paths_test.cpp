// Verifies platform paths test behavior for the Engine test suite.

#include <cstddef>
#include <cstring>

#include "engine/core/platform.h"

using namespace engine::core;

namespace {

/// Returns whether is normalized directory path.
bool is_normalized_directory_path(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return false;
  }

  if (std::strchr(path, '\\') != nullptr) {
    return false;
  }

  const std::size_t length = std::strlen(path);
  if ((length > 1U) && (path[length - 1U] == '/')) {
    return (length == 3U) && (path[1] == ':');
  }
  return true;
}

bool test_rejects_invalid_buffers() noexcept {
  if (platform_get_temp_dir(nullptr, 0U)) {
    return false;
  }

  char tiny[2] = {'x', 'y'};
  if (platform_get_temp_dir(tiny, sizeof(tiny))) {
    return false;
  }
  return tiny[0] == '\0';
}

bool test_temp_dir() noexcept {
  char path[1024] = {};
  if (!platform_get_temp_dir(path, sizeof(path))) {
    return false;
  }
  return is_normalized_directory_path(path);
}

bool test_app_dir() noexcept {
  char path[1024] = {};
  if (!platform_get_app_dir(path, sizeof(path))) {
    return false;
  }
  return is_normalized_directory_path(path);
}

bool test_save_dir() noexcept {
  char path[1024] = {};
  if (!platform_get_save_dir("EngineTestOrg", "EngineTestApp", path,
                             sizeof(path))) {
    return false;
  }
  if (!is_normalized_directory_path(path)) {
    return false;
  }
  return std::strstr(path, "EngineTestApp") != nullptr;
}

bool test_default_save_dir() noexcept {
  char path[1024] = {};
  if (!platform_get_save_dir(path, sizeof(path))) {
    return false;
  }
  return is_normalized_directory_path(path);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!test_rejects_invalid_buffers()) {
    return 1;
  }
  if (!test_temp_dir()) {
    return 2;
  }
  if (!test_app_dir()) {
    return 3;
  }
  if (!test_save_dir()) {
    return 4;
  }
  if (!test_default_save_dir()) {
    return 5;
  }
  return 0;
}
