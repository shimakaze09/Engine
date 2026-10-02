// POSIX implementation of the platform layer's OS services that do not use
// SDL: environment lookup, OS random bytes, process memory, the temp and
// save-base directories, and the web persistence flush. CMake selects this
// file for every non-Windows target (Linux, macOS, Android, iOS and
// Emscripten) and platform_os_windows.cpp for Windows.

#include "engine/core/platform.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <unistd.h>
#endif

#if defined(ENGINE_PLATFORM_WEB)
#include <emscripten.h>
#endif

#include "platform_internal.h"

namespace engine::core {

using platform_detail::append_path_segment;
using platform_detail::copy_normalized_path;
using platform_detail::kPlatformPathMax;
using platform_detail::validate_path_output;

namespace {

#if defined(ENGINE_PLATFORM_WEB)
/// The page's IndexedDB-backed mount (core/web/persistent_storage.js); a
/// save anywhere else lives in memory and is gone on the next load.
constexpr const char *kWebPersistentRoot = "/persistent";
#endif

} // namespace

namespace platform_detail {

/// Builds the requested runtime data for save base.
bool build_save_base(char *outBuffer, std::size_t bufferCapacity) noexcept {
#if defined(ENGINE_PLATFORM_WEB)
  return copy_normalized_path(kWebPersistentRoot, outBuffer, bufferCapacity);
#else
  char value[kPlatformPathMax] = {};
#if defined(__APPLE__)
  if (!non_empty_env("HOME", value, sizeof(value))) {
    return false;
  }
  if (!copy_normalized_path(value, outBuffer, bufferCapacity)) {
    return false;
  }
  return append_path_segment(outBuffer, bufferCapacity, "Library") &&
         append_path_segment(outBuffer, bufferCapacity, "Application Support");
#else
  if (non_empty_env("XDG_DATA_HOME", value, sizeof(value))) {
    return copy_normalized_path(value, outBuffer, bufferCapacity);
  }
  if (!non_empty_env("HOME", value, sizeof(value))) {
    return false;
  }
  if (!copy_normalized_path(value, outBuffer, bufferCapacity)) {
    return false;
  }
  return append_path_segment(outBuffer, bufferCapacity, ".local") &&
         append_path_segment(outBuffer, bufferCapacity, "share");
#endif
#endif
}

} // namespace platform_detail

bool non_empty_env(const char *name, char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((name == nullptr) || (name[0] == '\0')) {
    return false;
  }
  const char *value = std::getenv(name);
  if ((value == nullptr) || (value[0] == '\0')) {
    return false;
  }
  const std::size_t length = std::strlen(value);
  if (length >= capacity) {
    return false;
  }
  std::memcpy(out, value, length + 1U);
  return true;
}

bool platform_random_bytes(void *out, std::size_t size) noexcept {
  if (out == nullptr) {
    return false;
  }
  if (size == 0U) {
    return true;
  }
  auto *bytes = static_cast<unsigned char *>(out);

#if defined(__APPLE__)
  arc4random_buf(bytes, size);
  return true;
#elif defined(__linux__)
  std::size_t written = 0U;
  while (written < size) {
    const ssize_t got = getrandom(bytes + written, size - written, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    written += static_cast<std::size_t>(got);
  }
  if (written == size) {
    return true;
  }
  // getrandom is unavailable before Linux 3.17 and can be blocked by a
  // sandbox; the device is the long-standing fallback.
  FILE *device = std::fopen("/dev/urandom", "rb");
  if (device == nullptr) {
    return false;
  }
  const std::size_t read = std::fread(bytes, 1U, size, device);
  static_cast<void>(std::fclose(device));
  return read == size;
#else
  static_cast<void>(bytes);
  return false;
#endif
}

std::size_t process_memory_bytes() noexcept {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  const kern_return_t result =
      task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count);
  if (result != KERN_SUCCESS) {
    return 0U;
  }
  return static_cast<std::size_t>(info.resident_size);
#elif defined(__linux__)
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) {
    return 0U;
  }

  FILE *fp = std::fopen("/proc/self/statm", "r");
  if (fp == nullptr) {
    return 0U;
  }

  unsigned long long totalPages = 0ULL;
  unsigned long long residentPages = 0ULL;
  const int scanned = std::fscanf(fp, "%llu %llu", &totalPages, &residentPages);
  std::fclose(fp);
  if (scanned != 2) {
    return 0U;
  }

  return static_cast<std::size_t>(residentPages *
                                  static_cast<unsigned long long>(pageSize));
#else
  return 0U;
#endif
}

bool platform_attach_parent_console() noexcept { return false; }

bool platform_persist_after_write(const char *path) noexcept {
#if defined(ENGINE_PLATFORM_WEB)
  const std::size_t rootLength = std::strlen(kWebPersistentRoot);
  if ((path == nullptr) ||
      (std::strncmp(path, kWebPersistentRoot, rootLength) != 0) ||
      (path[rootLength] != '/')) {
    return false;
  }
  // FS lives on the page's main thread; a write committed on a worker
  // queues the flush there instead of touching FS from the worker.
  MAIN_THREAD_ASYNC_EM_ASM({
    if (Module['enginePersist']) {
      Module['enginePersist']();
    }
  });
  return true;
#else
  static_cast<void>(path);
  return false;
#endif
}

bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity)) {
    return false;
  }

  char tempPath[kPlatformPathMax] = {};
  const char *candidates[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
  for (const char *candidate : candidates) {
    if (non_empty_env(candidate, tempPath, sizeof(tempPath))) {
      return copy_normalized_path(tempPath, outBuffer, bufferCapacity);
    }
  }
  return copy_normalized_path("/tmp", outBuffer, bufferCapacity);
}

} // namespace engine::core
