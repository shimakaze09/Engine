#include "engine/core/vfs.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <new>

#include "engine/core/logging.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace engine::core {

namespace {

constexpr std::size_t kMaxMounts = 16U;
constexpr std::size_t kMaxPrefixLength = 64U;
constexpr std::size_t kMaxOsPathLength = 260U;
constexpr std::size_t kMaxResolvedPathLength = 512U;

struct MountEntry final {
  char prefix[kMaxPrefixLength] = {};
  char osPath[kMaxOsPathLength] = {};
  std::size_t prefixLength = 0U;
  bool active = false;
};

std::array<MountEntry, kMaxMounts> g_mounts{};
bool g_vfsInitialized = false;

// Normalize a path in-place: backslash → forward slash, strip trailing slash.
void normalize_path(char *path, std::size_t length) noexcept {
  for (std::size_t i = 0U; i < length; ++i) {
    if (path[i] == '\\') {
      path[i] = '/';
    }
  }
  // Strip trailing slash.
  if ((length > 0U) && (path[length - 1U] == '/')) {
    path[length - 1U] = '\0';
  }
}

// Resolve virtualPath to an OS path using the longest-prefix match.
// Returns the number of characters written (excluding null) or 0 on failure.
std::size_t resolve(const char *virtualPath, char *outBuffer,
                    std::size_t capacity) noexcept {
  if ((virtualPath == nullptr) || (outBuffer == nullptr) || (capacity == 0U)) {
    return 0U;
  }

  // Normalize virtualPath into a local copy.
  char normalizedVirtual[kMaxResolvedPathLength] = {};
  const std::size_t vpLen = std::strlen(virtualPath);
  if (vpLen >= kMaxResolvedPathLength) {
    return 0U;
  }
  std::memcpy(normalizedVirtual, virtualPath, vpLen + 1U);
  normalize_path(normalizedVirtual, vpLen);

  // Find longest-prefix match.
  const MountEntry *bestMount = nullptr;
  std::size_t bestPrefixLen = 0U;

  for (const auto &entry : g_mounts) {
    if (!entry.active) {
      continue;
    }
    if (entry.prefixLength > vpLen) {
      continue;
    }
    // Prefix must match exactly up to its length.
    if (std::strncmp(normalizedVirtual, entry.prefix, entry.prefixLength) !=
        0) {
      continue;
    }
    // After the prefix, the next char must be '/' or end of string.
    if ((entry.prefixLength < vpLen) &&
        (normalizedVirtual[entry.prefixLength] != '/')) {
      continue;
    }
    if (entry.prefixLength > bestPrefixLen) {
      bestPrefixLen = entry.prefixLength;
      bestMount = &entry;
    }
  }

  if (bestMount == nullptr) {
    return 0U;
  }

  // Build OS path: mount.osPath + "/" + remainder.
  const char *remainder = normalizedVirtual + bestPrefixLen;
  // Skip the leading '/' in remainder.
  if ((remainder[0] == '/') && (remainder[1] != '\0')) {
    ++remainder;
  } else if (remainder[0] == '/') {
    remainder = "";
  }

  const std::size_t osLen = std::strlen(bestMount->osPath);
  const std::size_t remLen = std::strlen(remainder);
  const bool needSlash = (remLen > 0U);
  const std::size_t totalLen = osLen + (needSlash ? 1U : 0U) + remLen;

  if ((totalLen + 1U) > capacity) {
    return 0U;
  }

  std::memcpy(outBuffer, bestMount->osPath, osLen);
  if (needSlash) {
    outBuffer[osLen] = '/';
    std::memcpy(outBuffer + osLen + 1U, remainder, remLen);
  }
  outBuffer[totalLen] = '\0';
  return totalLen;
}

} // namespace

bool initialize_vfs() noexcept {
  if (g_vfsInitialized) {
    return true;
  }
  for (auto &entry : g_mounts) {
    entry = MountEntry{};
  }
  g_vfsInitialized = true;
  log_message(LogLevel::Info, "vfs", "VFS initialized");
  return true;
}

void shutdown_vfs() noexcept {
  if (!g_vfsInitialized) {
    return;
  }
  for (auto &entry : g_mounts) {
    entry = MountEntry{};
  }
  g_vfsInitialized = false;
}

bool mount(const char *virtualPrefix, const char *osDirectoryPath) noexcept {
  if ((virtualPrefix == nullptr) || (osDirectoryPath == nullptr)) {
    return false;
  }
  const std::size_t prefixLen = std::strlen(virtualPrefix);
  const std::size_t osLen = std::strlen(osDirectoryPath);
  if ((prefixLen == 0U) || (prefixLen >= kMaxPrefixLength) || (osLen == 0U) ||
      (osLen >= kMaxOsPathLength)) {
    return false;
  }

  // Check for existing mount with same prefix; update if found.
  for (auto &entry : g_mounts) {
    if (entry.active && (std::strcmp(entry.prefix, virtualPrefix) == 0)) {
      std::memcpy(entry.osPath, osDirectoryPath, osLen + 1U);
      normalize_path(entry.osPath, osLen);
      normalize_path(entry.prefix, prefixLen);
      log_message(LogLevel::Info, "vfs", "remounted prefix");
      return true;
    }
  }

  // Find free slot.
  for (auto &entry : g_mounts) {
    if (!entry.active) {
      std::memcpy(entry.prefix, virtualPrefix, prefixLen + 1U);
      normalize_path(entry.prefix, prefixLen);
      entry.prefixLength = std::strlen(entry.prefix);
      std::memcpy(entry.osPath, osDirectoryPath, osLen + 1U);
      normalize_path(entry.osPath, osLen);
      entry.active = true;
      log_message(LogLevel::Info, "vfs", "mounted prefix");
      return true;
    }
  }

  log_message(LogLevel::Error, "vfs", "mount table full");
  return false;
}

bool unmount(const char *virtualPrefix) noexcept {
  if (virtualPrefix == nullptr) {
    return false;
  }
  for (auto &entry : g_mounts) {
    if (entry.active && (std::strcmp(entry.prefix, virtualPrefix) == 0)) {
      entry = MountEntry{};
      return true;
    }
  }
  return false;
}

bool vfs_file_exists(const char *virtualPath) noexcept {
  char osPath[kMaxResolvedPathLength] = {};
  if (resolve(virtualPath, osPath, sizeof(osPath)) == 0U) {
    return false;
  }
#if defined(_WIN32)
  const DWORD attrs = GetFileAttributesA(osPath);
  return (attrs != INVALID_FILE_ATTRIBUTES) &&
         ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0U);
#else
  struct stat st{};
  return (stat(osPath, &st) == 0) && ((st.st_mode & S_IFREG) != 0);
#endif
}

bool vfs_read_binary(const char *virtualPath, void **outData,
                     std::size_t *outSize) noexcept {
  if ((outData == nullptr) || (outSize == nullptr)) {
    return false;
  }

  char osPath[kMaxResolvedPathLength] = {};
  if (resolve(virtualPath, osPath, sizeof(osPath)) == 0U) {
    return false;
  }

  FILE *file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, osPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(osPath, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long fileSize = std::ftell(file);
  if (fileSize < 0) {
    std::fclose(file);
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }

  const auto size = static_cast<std::size_t>(fileSize);
  auto *buffer = new (std::nothrow) std::byte[size];
  if (buffer == nullptr) {
    std::fclose(file);
    return false;
  }

  if (size > 0U) {
    const std::size_t bytesRead = std::fread(buffer, 1U, size, file);
    if (bytesRead != size) {
      delete[] buffer;
      std::fclose(file);
      return false;
    }
  }

  std::fclose(file);
  *outData = buffer;
  *outSize = size;
  return true;
}

bool vfs_read_text(const char *virtualPath, char **outText,
                   std::size_t *outSize) noexcept {
  if ((outText == nullptr) || (outSize == nullptr)) {
    return false;
  }

  void *rawData = nullptr;
  std::size_t rawSize = 0U;
  if (!vfs_read_binary(virtualPath, &rawData, &rawSize)) {
    return false;
  }

  // Re-allocate with null terminator using the same allocation type used by
  // vfs_read_binary so vfs_free() always matches allocation/deallocation.
  auto *textBufferBytes = new (std::nothrow) std::byte[rawSize + 1U];
  if (textBufferBytes == nullptr) {
    vfs_free(rawData);
    return false;
  }

  auto *textBuffer = reinterpret_cast<char *>(textBufferBytes);

  std::memcpy(textBuffer, rawData, rawSize);
  textBuffer[rawSize] = '\0';
  vfs_free(rawData);

  *outText = textBuffer;
  *outSize = rawSize;
  return true;
}

bool vfs_write_binary(const char *virtualPath, const void *data,
                      std::size_t size) noexcept {
  if ((data == nullptr) && (size > 0U)) {
    return false;
  }

  char osPath[kMaxResolvedPathLength] = {};
  if (resolve(virtualPath, osPath, sizeof(osPath)) == 0U) {
    return false;
  }

  FILE *file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, osPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(osPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  if (size > 0U) {
    const std::size_t written = std::fwrite(data, 1U, size, file);
    if (written != size) {
      std::fclose(file);
      return false;
    }
  }

  std::fclose(file);
  return true;
}

bool vfs_write_text(const char *virtualPath, const char *text,
                    std::size_t size) noexcept {
  return vfs_write_binary(virtualPath, text, size);
}

void vfs_free(void *buffer) noexcept {
  delete[] static_cast<std::byte *>(buffer);
}

std::int64_t vfs_file_mtime(const char *virtualPath) noexcept {
  char osPath[kMaxResolvedPathLength] = {};
  if (resolve(virtualPath, osPath, sizeof(osPath)) == 0U) {
    return 0;
  }

#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExA(osPath, GetFileExInfoStandard, &data) == 0) {
    return 0;
  }
  LARGE_INTEGER li{};
  li.LowPart = data.ftLastWriteTime.dwLowDateTime;
  li.HighPart = static_cast<LONG>(data.ftLastWriteTime.dwHighDateTime);
  return li.QuadPart;
#else
  struct stat st{};
  if (stat(osPath, &st) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(st.st_mtime);
#endif
}

bool vfs_resolve_os_path(const char *virtualPath, char *outBuffer,
                         std::size_t bufferCapacity) noexcept {
  return resolve(virtualPath, outBuffer, bufferCapacity) > 0U;
}

} // namespace engine::core
