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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
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

// Member state is committed only after every validation and the open
// succeed, so a refused begin can never arm cleanup with a truncated
// path that aliases the destination or an unrelated file.
bool AtomicFileWriter::begin(const char *destinationPath) noexcept {
  if ((destinationPath == nullptr) || (m_file != nullptr)) {
    return false;
  }

  char destination[sizeof(m_destinationPath)] = {};
  const int destinationFormatted =
      std::snprintf(destination, sizeof(destination), "%s", destinationPath);
  if ((destinationFormatted <= 0) ||
      (static_cast<std::size_t>(destinationFormatted) >=
       sizeof(destination))) {
    return false;
  }

  const std::uint32_t serial =
      g_tempSerial.fetch_add(1U, std::memory_order_relaxed);
  char temp[sizeof(m_tempPath)] = {};
  const int tempFormatted =
      std::snprintf(temp, sizeof(temp), "%s.new.%lu.%u", destinationPath,
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

  // The replacement already happened, so the save is not reportable as a
  // failure — only its power-loss resistance is degraded, and that must
  // not pass silently.
  if (outcome == detail::ReplaceOutcome::ReplacedNotDurable) {
    char message[1152] = {};
    std::snprintf(message, sizeof(message),
                  "wrote '%s' but could not sync its directory entry: the "
                  "file is in place and may not survive power loss",
                  m_destinationPath);
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
    std::error_code removeError{};
    std::filesystem::remove(m_tempPath, removeError);
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
  // that must not pass silently.
  if (outcome == detail::CreateDirectoryOutcome::CreatedNotDurable) {
    char message[1152] = {};
    std::snprintf(message, sizeof(message),
                  "created '%s' but could not sync its directory entry: the "
                  "directory is in place and may not survive power loss",
                  directoryPath);
    log_message(LogLevel::Error, "core.atomic_file", message);
  }
  return true;
}

} // namespace engine::core
