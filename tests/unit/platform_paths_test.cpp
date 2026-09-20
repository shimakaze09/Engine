// Verifies platform paths test behavior for the Engine test suite.

#include <cstddef>
#include <cstdlib>
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

bool set_env(const char *name, const char *value) noexcept {
#if defined(_WIN32)
  return _putenv_s(name, value) == 0;
#else
  return setenv(name, value, 1) == 0;
#endif
}

/// Two lookups on one thread keep their own bytes: the first result is
/// still its variable's value after the second call. Unset, empty and
/// too-long values answer false with the buffer emptied.
bool test_env_values_do_not_alias() noexcept {
  if (!set_env("ENGINE_TEST_ENV_FIRST", "first-value") ||
      !set_env("ENGINE_TEST_ENV_SECOND", "second-value") ||
      !set_env("ENGINE_TEST_ENV_EMPTY", "")) {
    return false;
  }
  char first[64] = {};
  char second[64] = {};
  if (!non_empty_env("ENGINE_TEST_ENV_FIRST", first, sizeof(first)) ||
      !non_empty_env("ENGINE_TEST_ENV_SECOND", second, sizeof(second))) {
    return false;
  }
  if ((std::strcmp(first, "first-value") != 0) ||
      (std::strcmp(second, "second-value") != 0)) {
    return false;
  }

  char scratch[64] = {'x', '\0'};
  if (non_empty_env("ENGINE_TEST_ENV_UNSET_NEVER_DEFINED", scratch,
                    sizeof(scratch)) ||
      (scratch[0] != '\0')) {
    return false;
  }
  scratch[0] = 'x';
  if (non_empty_env("ENGINE_TEST_ENV_EMPTY", scratch, sizeof(scratch)) ||
      (scratch[0] != '\0')) {
    return false;
  }
  char tiny[4] = {'x', '\0'};
  if (non_empty_env("ENGINE_TEST_ENV_FIRST", tiny, sizeof(tiny)) ||
      (tiny[0] != '\0')) {
    return false;
  }
  if (non_empty_env(nullptr, scratch, sizeof(scratch)) ||
      non_empty_env("ENGINE_TEST_ENV_FIRST", nullptr, 0U) ||
      non_empty_env("", scratch, sizeof(scratch))) {
    return false;
  }
  return true;
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
  if (!test_env_values_do_not_alias()) {
    return 6;
  }
  return 0;
}
