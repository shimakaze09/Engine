// Windows implementation of the platform layer's OS services that do not
// use SDL: environment lookup, OS random bytes, process memory, parent
// console attach, the temp and save-base directories, and the no-op persist
// hook. CMake selects this file for Windows targets and platform_os_posix.cpp
// for every other target.

// rand_s (platform_random_bytes) is only declared when this is defined
// before the CRT headers.
#if !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "engine/core/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// After windows.h, whose types it uses.
#include <psapi.h>

#include "platform_internal.h"

namespace engine::core {

namespace platform_detail {

/// Builds the requested runtime data for save base.
bool build_save_base(char *outBuffer, std::size_t bufferCapacity) noexcept {
  char value[kPlatformPathMax] = {};
  if (non_empty_env("APPDATA", value, sizeof(value))) {
    return copy_normalized_path(value, outBuffer, bufferCapacity);
  }
  if (non_empty_env("USERPROFILE", value, sizeof(value))) {
    if (!copy_normalized_path(value, outBuffer, bufferCapacity)) {
      return false;
    }
    return append_path_segment(outBuffer, bufferCapacity, "AppData") &&
           append_path_segment(outBuffer, bufferCapacity, "Roaming");
  }
  return false;
}

} // namespace platform_detail

using platform_detail::copy_normalized_path;
using platform_detail::kPlatformPathMax;
using platform_detail::validate_path_output;

bool non_empty_env(const char *name, char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((name == nullptr) || (name[0] == '\0')) {
    return false;
  }
  // The API reports the length the value needs when the buffer is too
  // small, and 0 when the variable is unset; an empty value fits and
  // returns 0 too, which is the same answer here.
  const DWORD length =
      GetEnvironmentVariableA(name, out, static_cast<DWORD>(capacity));
  if ((length == 0U) || (length >= capacity)) {
    out[0] = '\0';
    return false;
  }
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

  // rand_s draws from the OS CSPRNG and needs no extra import library,
  // unlike BCryptGenRandom. It yields four bytes at a time.
  std::size_t written = 0U;
  while (written < size) {
    unsigned int value = 0U;
    if (rand_s(&value) != 0) {
      return false;
    }
    const std::size_t chunk =
        ((size - written) < sizeof(value)) ? (size - written) : sizeof(value);
    std::memcpy(bytes + written, &value, chunk);
    written += chunk;
  }
  return true;
}

std::size_t process_memory_bytes() noexcept {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                           sizeof(pmc)) == 0) {
    return 0U;
  }
  return static_cast<std::size_t>(pmc.WorkingSetSize);
}

bool platform_attach_parent_console() noexcept {
  // A console-subsystem build already has one; only a GUI one attaches.
  if (GetConsoleWindow() != nullptr) {
    return false;
  }
  // A stream the parent redirected (a pipe, a file) already goes where the
  // parent wants it; pointing it at the console would lose it.
  const auto redirected = [](DWORD which) noexcept {
    const HANDLE handle = GetStdHandle(which);
    return (handle != nullptr) && (handle != INVALID_HANDLE_VALUE) &&
           (GetFileType(handle) != FILE_TYPE_UNKNOWN);
  };
  const bool outRedirected = redirected(STD_OUTPUT_HANDLE);
  const bool errRedirected = redirected(STD_ERROR_HANDLE);
  if (outRedirected && errRedirected) {
    return false;
  }
  if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
    return false;
  }
  std::FILE *stream = nullptr;
  if (!outRedirected) {
    static_cast<void>(freopen_s(&stream, "CONOUT$", "w", stdout));
  }
  if (!errRedirected) {
    static_cast<void>(freopen_s(&stream, "CONOUT$", "w", stderr));
  }
  return true;
}

bool platform_persist_after_write(const char *path) noexcept {
  static_cast<void>(path);
  return false;
}

bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity)) {
    return false;
  }

  char tempPath[kPlatformPathMax] = {};
  const DWORD length =
      GetTempPathA(static_cast<DWORD>(sizeof(tempPath)), tempPath);
  if ((length == 0U) || (length >= sizeof(tempPath))) {
    return false;
  }
  return copy_normalized_path(tempPath, outBuffer, bufferCapacity);
}

} // namespace engine::core
