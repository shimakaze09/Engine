// Implements the atomic authored-file write: the payload lands in a
// sibling temporary file, and commit hands that staged file to the
// durable-replacement protocol (durable_replace.h), so an interrupted
// save, a full disk, or a failed close can never destroy the previous
// valid file and the completed rename is itself made durable.
// AtomicFileWriter streams the same protocol in checked chunks for
// payloads too large to double-buffer.

#include "engine/core/atomic_file.h"

#include "durable_replace.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace engine::core {

namespace {

/// Per-call temporary suffix counter so concurrent writers to the same
/// destination never share a temporary; the final rename still resolves
/// concurrent commits as last-writer-wins on the destination.
std::atomic<std::uint32_t> g_tempSerial{0U};

/// Process id for the temporary-file suffix.
unsigned long current_process_id() noexcept {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

} // namespace

AtomicFileWriter::~AtomicFileWriter() noexcept { abort(); }

namespace {

/// Follows a symlinked destination to the file it names (a bounded
/// chain, relative targets resolved against the link's directory), so
/// the replacement lands on that file and the link survives. A
/// destination that is not a link resolves to itself; a chain deeper
/// than eight hops is refused. Fixed buffers only: begin() is noexcept
/// and must not allocate. Windows has no readlink and a symlink there
/// needs a handle-based query, so the destination is used as given and
/// a symlinked destination is replaced by a file.
bool resolve_symlinked_destination(const char *destinationPath, char *out,
                                   std::size_t outCapacity) noexcept {
  const std::size_t givenLength = std::strlen(destinationPath);
  if (givenLength >= outCapacity) {
    return false;
  }
  std::memcpy(out, destinationPath, givenLength + 1U);
#ifndef _WIN32
  char target[1024] = {};
  for (int hop = 0; hop < 8; ++hop) {
    const ssize_t targetLength = ::readlink(out, target, sizeof(target) - 1U);
    if (targetLength < 0) {
      return true; // not a link (or not readable): the path stands
    }
    target[targetLength] = '\0';
    if (target[0] == '/') {
      if (static_cast<std::size_t>(targetLength) >= outCapacity) {
        return false;
      }
      std::memcpy(out, target, static_cast<std::size_t>(targetLength) + 1U);
      continue;
    }
    const char *slash = std::strrchr(out, '/');
    const std::size_t directoryLength =
        (slash != nullptr) ? static_cast<std::size_t>(slash - out) + 1U : 0U;
    char joined[1024] = {};
    const int written =
        std::snprintf(joined, sizeof(joined), "%.*s%s",
                      static_cast<int>(directoryLength), out, target);
    if ((written <= 0) || (static_cast<std::size_t>(written) >= sizeof(joined)) ||
        (static_cast<std::size_t>(written) >= outCapacity)) {
      return false;
    }
    std::memcpy(out, joined, static_cast<std::size_t>(written) + 1U);
  }
  return ::readlink(out, target, sizeof(target) - 1U) < 0;
#else
  return true;
#endif
}

/// Carries the destination's permission bits onto the staged temporary
/// before any byte is written, so a file the author restricted (chmod
/// 600) comes back restricted. Best effort: a destination that
/// does not exist yet takes the process default, and Windows has no
/// equivalent bits on the temporary.
void inherit_destination_mode(const char *destination, std::FILE *file) noexcept {
#ifdef _WIN32
  static_cast<void>(destination);
  static_cast<void>(file);
#else
  struct stat info{};
  if ((::stat(destination, &info) == 0) && S_ISREG(info.st_mode)) {
    static_cast<void>(::fchmod(fileno(file), info.st_mode & 07777U));
  }
#endif
}

} // namespace

// Member state is committed only after every validation and the open
// succeed, so a refused begin can never arm cleanup with a truncated
// path that aliases the destination or an unrelated file.
bool AtomicFileWriter::begin(const char *destinationPath) noexcept {
  if ((destinationPath == nullptr) || (m_file != nullptr)) {
    return false;
  }

  char destination[sizeof(m_destinationPath)] = {};
  if (!resolve_symlinked_destination(destinationPath, destination,
                                     sizeof(destination))) {
    return false;
  }

  const std::uint32_t serial =
      g_tempSerial.fetch_add(1U, std::memory_order_relaxed);
  char temp[sizeof(m_tempPath)] = {};
  const int tempFormatted =
      std::snprintf(temp, sizeof(temp), "%s.new.%lu.%u", destination,
                    current_process_id(), serial);
  if ((tempFormatted <= 0) ||
      (static_cast<std::size_t>(tempFormatted) >= sizeof(temp))) {
    return false;
  }

  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, temp, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(temp, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  inherit_destination_mode(destination, file);

  std::memcpy(m_destinationPath, destination, sizeof(m_destinationPath));
  std::memcpy(m_tempPath, temp, sizeof(m_tempPath));
  m_file = file;
  return true;
}

bool AtomicFileWriter::write(const void *data, std::size_t sizeBytes) noexcept {
  if ((m_file == nullptr) || (data == nullptr)) {
    return false;
  }
  if (sizeBytes == 0U) {
    return true;
  }
  if (std::fwrite(data, 1U, sizeBytes, m_file) != sizeBytes) {
    abort();
    return false;
  }
  return true;
}

bool AtomicFileWriter::commit() noexcept {
  if (m_file == nullptr) {
    return false;
  }

  std::FILE *file = m_file;
  m_file = nullptr;
  const detail::ReplaceOutcome outcome = detail::durable_replace(
      file, m_tempPath, m_destinationPath, detail::production_replace_ops());
  // On the web the file system has no directory sync; a save outlives the
  // page through the flush the platform schedules for its mount instead.
  const bool persistedByPlatform =
      (outcome != detail::ReplaceOutcome::Failed) &&
      platform_persist_after_write(m_destinationPath);

  // The replacement already happened, so the save is not reportable as a
  // failure — only its power-loss resistance is degraded, and that must
  // not pass silently. Both degraded outcomes are reported: a platform
  // that offers no directory-sync primitive leaves the entry exactly as
  // undurable as one whose sync failed, and saying nothing would make
  // that the one degradation the log never shows.
  if ((outcome == detail::ReplaceOutcome::ReplacedNotDurable) ||
      ((outcome == detail::ReplaceOutcome::ReplacedDurabilityUnavailable) &&
       !persistedByPlatform)) {
    const char *reason =
        (outcome == detail::ReplaceOutcome::ReplacedNotDurable)
            ? "could not sync its directory entry"
            : "has no directory-sync primitive on this platform";
    char message[1152] = {};
    std::snprintf(message, sizeof(message),
                  "wrote '%s' but %s: the file is in place and may not "
                  "survive power loss",
                  m_destinationPath, reason);
    log_message(LogLevel::Error, "core.atomic_file", message);
  }

  m_tempPath[0] = '\0';
  m_destinationPath[0] = '\0';
  return outcome != detail::ReplaceOutcome::Failed;
}

void AtomicFileWriter::abort() noexcept {
  if (m_file != nullptr) {
    static_cast<void>(std::fclose(m_file));
    m_file = nullptr;
  }
  if (m_tempPath[0] != '\0') {
    // The same non-allocating primitive the commit protocol discards its
    // temporary with; abort runs from the destructor, where a failed
    // removal has no caller to report to and the destination is
    // untouched either way.
    static_cast<void>(
        detail::production_replace_ops().remove_file(m_tempPath));
    m_tempPath[0] = '\0';
  }
  m_destinationPath[0] = '\0';
}

bool atomic_write_file(const char *path, const void *data,
                       std::size_t size) noexcept {
  if ((path == nullptr) || (data == nullptr) || (size == 0U)) {
    return false;
  }

  AtomicFileWriter writer{};
  return writer.begin(path) && writer.write(data, size) && writer.commit();
}

bool create_directories_durably(const char *directoryPath) noexcept {
  const detail::CreateDirectoryOutcome outcome =
      detail::durable_create_directories(directoryPath,
                                         detail::production_replace_ops());

  if (outcome == detail::CreateDirectoryOutcome::Failed) {
    char message[1152] = {};
    std::snprintf(message, sizeof(message),
                  "could not create the directory '%s'; nothing can be saved "
                  "into it",
                  (directoryPath != nullptr) ? directoryPath : "(null)");
    log_message(LogLevel::Error, "core.atomic_file", message);
    return false;
  }

  // The directory is in place, so this is not reportable as a failure —
  // only the power-loss resistance of its own entry is degraded, and
  // that must not pass silently. Both degraded outcomes are reported,
  // for the reason the commit path reports both.
  if ((outcome == detail::CreateDirectoryOutcome::CreatedNotDurable) ||
      (outcome == detail::CreateDirectoryOutcome::CreatedDurabilityUnavailable)) {
    const char *reason =
        (outcome == detail::CreateDirectoryOutcome::CreatedNotDurable)
            ? "could not sync its directory entry"
            : "has no directory-sync primitive on this platform";
    char message[1152] = {};
    std::snprintf(message, sizeof(message),
                  "created '%s' but %s: the directory is in place and may "
                  "not survive power loss",
                  directoryPath, reason);
    log_message(LogLevel::Error, "core.atomic_file", message);
  }
  return true;
}

} // namespace engine::core
