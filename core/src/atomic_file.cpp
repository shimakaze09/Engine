// Implements the atomic authored-file write: the payload lands in a
// sibling temporary file that is flushed, synced, and closed with every
// step checked, then atomically renamed over the destination, so an
// interrupted save, a full disk, or a failed close can never destroy the
// previous valid file. AtomicFileWriter streams the same protocol in
// checked chunks for payloads too large to double-buffer.

#include "engine/core/atomic_file.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <io.h>
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

bool AtomicFileWriter::begin(const char *destinationPath) noexcept {
  if ((destinationPath == nullptr) || (m_file != nullptr)) {
    return false;
  }

  const int destinationFormatted =
      std::snprintf(m_destinationPath, sizeof(m_destinationPath), "%s",
                    destinationPath);
  if ((destinationFormatted <= 0) ||
      (static_cast<std::size_t>(destinationFormatted) >=
       sizeof(m_destinationPath))) {
    return false;
  }

  const std::uint32_t serial =
      g_tempSerial.fetch_add(1U, std::memory_order_relaxed);
  const int tempFormatted =
      std::snprintf(m_tempPath, sizeof(m_tempPath), "%s.new.%lu.%u",
                    destinationPath, current_process_id(), serial);
  if ((tempFormatted <= 0) ||
      (static_cast<std::size_t>(tempFormatted) >= sizeof(m_tempPath))) {
    return false;
  }

#ifdef _WIN32
  if (fopen_s(&m_file, m_tempPath, "wb") != 0) {
    m_file = nullptr;
  }
#else
  m_file = std::fopen(m_tempPath, "wb");
#endif
  return m_file != nullptr;
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

  bool ok = std::fflush(m_file) == 0;
#ifdef _WIN32
  ok = ok && (_commit(_fileno(m_file)) == 0);
#else
  ok = ok && (fsync(fileno(m_file)) == 0);
#endif
  ok = (std::fclose(m_file) == 0) && ok;
  m_file = nullptr;

  if (ok) {
    std::error_code renameError{};
    std::filesystem::rename(m_tempPath, m_destinationPath, renameError);
    ok = !renameError;
  }
  if (!ok) {
    std::error_code removeError{};
    std::filesystem::remove(m_tempPath, removeError);
  }
  return ok;
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
}

bool atomic_write_file(const char *path, const void *data,
                       std::size_t size) noexcept {
  if ((path == nullptr) || (data == nullptr) || (size == 0U)) {
    return false;
  }

  AtomicFileWriter writer{};
  return writer.begin(path) && writer.write(data, size) && writer.commit();
}

} // namespace engine::core
