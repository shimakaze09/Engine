// Declares the shared fixed-capacity whole-file reader for the Engine core.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Outcome of read_whole_file. Absent is the ordinary fresh-profile case (no
/// file at the path); everything else that stops the read is a distinct
/// fault, because callers that persist authored or settings data must not
/// treat "could not read the stored file" as "no stored file" — writing
/// defaults back over a file that is still good on disk is exactly the loss
/// the authored-data rules exist to prevent.
enum class FileReadResult : std::uint8_t {
  Ok,
  /// The path names nothing (ENOENT). Never a fault: first runs and fresh
  /// profiles land here and keep saving normally.
  Absent,
  /// The file exists but could not be opened or fully read — permissions, a
  /// sharing violation, descriptor exhaustion, a mid-read I/O error, or
  /// invalid arguments. The bytes on disk may still be good; the caller
  /// decides whether writing over them is safe.
  Unreadable,
  /// The file holds more than capacity - 1 bytes, so it cannot round-trip
  /// through the caller's fixed buffer; the stored bytes are left untouched.
  TooLarge,
};

/// Reads the whole file at `path` into `out` (NUL-terminated, so at most
/// capacity - 1 content bytes), reporting *outSize when non-null. Fixed
/// capacity and no allocation: every caller sits on a noexcept persist or
/// cold-load path. A read that fails part-way reports Unreadable rather
/// than a shorter file, so a damaged read can never be mistaken for a
/// complete short document; Unreadable outranks TooLarge when both apply,
/// because a failing stream's extra-byte probe is itself unreliable.
FileReadResult read_whole_file(const char *path, char *out,
                               std::size_t capacity,
                               std::size_t *outSize) noexcept;

/// What a file on disk holds, compared rather than read: whether it
/// exists, its size, and a 64-bit FNV-1a of its bytes. An editor keeps the
/// fingerprint of the document it loaded or last wrote, and compares it
/// before a save, so a file changed on disk since then is not silently
/// written over. The content hash, not the modification time, decides:
/// file systems keep times too coarsely, and a checkout can restore one.
struct FileFingerprint final {
  bool exists = false;
  std::uint64_t size = 0U;
  std::uint64_t hash = 0U;

  bool operator==(const FileFingerprint &) const noexcept = default;
};

/// Fingerprints the file at `path`, streaming it through a small fixed
/// buffer. Ok with the fingerprint; Absent with a fingerprint whose
/// `exists` is false; Unreadable, with `out` left zeroed, when the file is
/// there but could not be read whole.
FileReadResult file_fingerprint(const char *path,
                                FileFingerprint *out) noexcept;

} // namespace engine::core
