// Implements the per-project user data directory: keeps the project's
// GUID, or a 64-bit FNV-1a hash of its absolute content root, and composes
// <platform save dir>/projects/<32 hex GUID digits or 16 hex hash digits>
// on request.

#include "engine/core/project_data.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::core {

namespace {

#if defined(_WIN32)
constexpr std::size_t kAbsolutePathMax = 32768U;
#else
constexpr std::size_t kAbsolutePathMax = PATH_MAX;
#endif

// The identity is published before the flag, so a reader that sees the
// flag set also sees the identity it names. g_projectLow and g_projectByGuid
// matter only for a GUID; a root-named project keeps its hash in
// g_projectId alone.
std::atomic<std::uint64_t> g_projectId{0U};
std::atomic<std::uint64_t> g_projectLow{0U};
std::atomic<bool> g_projectByGuid{false};
std::atomic<bool> g_projectSet{false};

/// Resolves `path` to the absolute text the identity hashes. On Windows
/// the separators become '/' and ASCII letters lower case, so two
/// spellings of one directory hash alike.
bool resolve_identity(const char *path, char *out,
                      std::size_t capacity) noexcept {
#if defined(_WIN32)
  if (_fullpath(out, path, capacity) == nullptr) {
    return false;
  }
  for (char *c = out; *c != '\0'; ++c) {
    if (*c == '\\') {
      *c = '/';
    } else if ((*c >= 'A') && (*c <= 'Z')) {
      *c = static_cast<char>(*c - 'A' + 'a');
    }
  }
  return true;
#else
  static_cast<void>(capacity);
  return realpath(path, out) != nullptr;
#endif
}

} // namespace

bool set_project_data_root(const char *projectRoot) noexcept {
  clear_project_data_root();
  if ((projectRoot == nullptr) || (projectRoot[0] == '\0')) {
    log_message(LogLevel::Error, "project",
                "project data root is empty; per-project data is refused");
    return false;
  }
  static char resolved[kAbsolutePathMax] = {};
  if (!resolve_identity(projectRoot, resolved, sizeof(resolved))) {
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "cannot resolve project root '%.160s'; per-project data "
                  "is refused",
                  projectRoot);
    log_message(LogLevel::Error, "project", message);
    return false;
  }
  g_projectId.store(fnv1a_64(resolved), std::memory_order_relaxed);
  g_projectSet.store(true, std::memory_order_release);

  char directory[1024] = {};
  if (project_data_dir(directory, sizeof(directory))) {
    // A log line may truncate; the directory itself never does.
    char message[1400] = {};
    std::snprintf(message, sizeof(message),
                  "project data for %.600s lives in %.700s", resolved,
                  directory);
    log_message(LogLevel::Info, "project", message);
  }
  return true;
}

bool set_project_data_guid(const AssetGuid &guid) noexcept {
  clear_project_data_root();
  if (!asset_guid_is_valid(guid)) {
    log_message(LogLevel::Error, "project",
                "project GUID is nil; per-project data is refused");
    return false;
  }
  g_projectId.store(guid.high, std::memory_order_relaxed);
  g_projectLow.store(guid.low, std::memory_order_relaxed);
  g_projectByGuid.store(true, std::memory_order_relaxed);
  g_projectSet.store(true, std::memory_order_release);

  char directory[1024] = {};
  if (project_data_dir(directory, sizeof(directory))) {
    char message[1100] = {};
    std::snprintf(message, sizeof(message), "project data lives in %.1000s",
                  directory);
    log_message(LogLevel::Info, "project", message);
  }
  return true;
}

bool project_data_named() noexcept {
  return g_projectSet.load(std::memory_order_acquire);
}

void clear_project_data_root() noexcept {
  g_projectSet.store(false, std::memory_order_release);
  g_projectId.store(0U, std::memory_order_relaxed);
  g_projectLow.store(0U, std::memory_order_relaxed);
  g_projectByGuid.store(false, std::memory_order_relaxed);
}

bool project_data_dir(char *outBuffer, std::size_t bufferCapacity) noexcept {
  if ((outBuffer == nullptr) || (bufferCapacity == 0U)) {
    return false;
  }
  outBuffer[0] = '\0';
  if (!g_projectSet.load(std::memory_order_acquire)) {
    log_message(LogLevel::Error, "project",
                "no project is named; per-project data is refused");
    return false;
  }
  const std::uint64_t id = g_projectId.load(std::memory_order_relaxed);
  const std::uint64_t low = g_projectLow.load(std::memory_order_relaxed);
  const bool byGuid = g_projectByGuid.load(std::memory_order_relaxed);

  char saveDir[1024] = {};
  if (!platform_get_save_dir(saveDir, sizeof(saveDir))) {
    return false;
  }
  const int written =
      byGuid ? std::snprintf(outBuffer, bufferCapacity,
                             "%s/projects/%016llx%016llx", saveDir,
                             static_cast<unsigned long long>(id),
                             static_cast<unsigned long long>(low))
             : std::snprintf(outBuffer, bufferCapacity, "%s/projects/%016llx",
                             saveDir, static_cast<unsigned long long>(id));
  if ((written <= 0) || (static_cast<std::size_t>(written) >= bufferCapacity)) {
    outBuffer[0] = '\0';
    return false;
  }
  return true;
}

} // namespace engine::core
