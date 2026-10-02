// Implements the shared fixed-capacity file readers for the Engine core:
// whole files, file prefixes and content fingerprints.

#include "engine/core/file_read.h"

#include <cerrno>
#include <cstdio>

#include "engine/core/hash.h"

namespace engine::core {

namespace {

/// Portable read-mode open that reports why an open failed: only ENOENT is
/// an absence; every other failure — permissions, a sharing violation,
/// descriptor exhaustion — is a fault, because the file may still be good
/// on disk. The CRT reports which: POSIX through errno, fopen_s through its
/// errno_t return.
std::FILE *open_file_for_read(const char *path,
                              FileReadResult *outFailure) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  const errno_t openError = fopen_s(&file, path, "rb");
  if (openError != 0) {
    file = nullptr;
    *outFailure = (openError == ENOENT) ? FileReadResult::Absent
                                        : FileReadResult::Unreadable;
  }
#else
  errno = 0;
  file = std::fopen(path, "rb");
  if (file == nullptr) {
    *outFailure =
        (errno == ENOENT) ? FileReadResult::Absent : FileReadResult::Unreadable;
  }
#endif
  return file;
}

} // namespace

FileReadResult read_whole_file(const char *path, char *out,
                               std::size_t capacity,
                               std::size_t *outSize) noexcept {
  if ((path == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return FileReadResult::Unreadable;
  }

  FileReadResult openFailure = FileReadResult::Absent;
  std::FILE *file = open_file_for_read(path, &openFailure);
  if (file == nullptr) {
    return openFailure;
  }

  const std::size_t readCount = std::fread(out, 1U, capacity - 1U, file);
  // The extra-byte probe distinguishes a file that exactly fills the buffer
  // from one that overflows it; a stream error also returns EOF from fgetc,
  // which is why the ferror check below outranks the overflow flag.
  const bool overflow = std::fgetc(file) != EOF;
  const bool hitError = std::ferror(file) != 0;
  static_cast<void>(std::fclose(file));
  if (hitError) {
    return FileReadResult::Unreadable;
  }
  if (overflow) {
    return FileReadResult::TooLarge;
  }
  out[readCount] = '\0';
  if (outSize != nullptr) {
    *outSize = readCount;
  }
  return FileReadResult::Ok;
}

FileReadResult read_file_prefix(const char *path, char *out,
                                std::size_t capacity,
                                std::size_t *outSize) noexcept {
  if ((path == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return FileReadResult::Unreadable;
  }
  FileReadResult openFailure = FileReadResult::Absent;
  std::FILE *file = open_file_for_read(path, &openFailure);
  if (file == nullptr) {
    return openFailure;
  }
  const std::size_t readCount = std::fread(out, 1U, capacity - 1U, file);
  const bool hitError = std::ferror(file) != 0;
  static_cast<void>(std::fclose(file));
  if (hitError) {
    return FileReadResult::Unreadable;
  }
  out[readCount] = '\0';
  if (outSize != nullptr) {
    *outSize = readCount;
  }
  return FileReadResult::Ok;
}

FileReadResult file_fingerprint(const char *path,
                                FileFingerprint *out) noexcept {
  if ((path == nullptr) || (out == nullptr)) {
    return FileReadResult::Unreadable;
  }
  *out = FileFingerprint{};
  FileReadResult openFailure = FileReadResult::Absent;
  std::FILE *file = open_file_for_read(path, &openFailure);
  if (file == nullptr) {
    return openFailure;
  }
  std::uint64_t hash = kFnv1a64Offset;
  std::uint64_t size = 0U;
  unsigned char chunk[4096];
  for (;;) {
    const std::size_t count = std::fread(chunk, 1U, sizeof(chunk), file);
    for (std::size_t i = 0U; i < count; ++i) {
      hash = fnv1a_64_append(hash, chunk[i]);
    }
    size += count;
    if (count < sizeof(chunk)) {
      break;
    }
  }
  const bool hitError = std::ferror(file) != 0;
  static_cast<void>(std::fclose(file));
  if (hitError) {
    return FileReadResult::Unreadable;
  }
  out->exists = true;
  out->size = size;
  out->hash = hash;
  return FileReadResult::Ok;
}

} // namespace engine::core
