// Bounded C-string copies shared engine-wide so every module truncates,
// refuses, and terminates identically. copy_string is the truncating form
// for display text and log/scratch buffers; copy_string_strict is the
// refusing form for identity-bearing fields (names that are hashed or
// looked up, asset and script paths), where a truncated copy would
// silently address something other than what the caller named.

#pragma once

#include <cstddef>
#include <cstring>

namespace engine::core {

/// Copies src into dst, truncating to dstCapacity - 1 characters and always
/// null-terminating. A null dst or zero capacity is a no-op; a null src
/// yields an empty string.
inline void copy_string(char *dst, std::size_t dstCapacity,
                        const char *src) noexcept {
  if ((dst == nullptr) || (dstCapacity == 0U)) {
    return;
  }

  std::size_t i = 0U;
  if (src != nullptr) {
    for (; ((i + 1U) < dstCapacity) && (src[i] != '\0'); ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

/// Copies src into dst only when it fits whole (a terminator within the
/// first dstCapacity bytes of src); otherwise leaves dst as an empty string
/// and returns false, so the caller can refuse the input instead of
/// committing a cut copy. Never reads more than dstCapacity bytes of src,
/// so a same-sized fixed array that lost its terminator is refused rather
/// than scanned past its end. A null src copies as an empty string and
/// succeeds; a null dst or zero capacity fails.
inline bool copy_string_strict(char *dst, std::size_t dstCapacity,
                               const char *src) noexcept {
  if ((dst == nullptr) || (dstCapacity == 0U)) {
    return false;
  }
  dst[0] = '\0';
  if (src == nullptr) {
    return true;
  }
  for (std::size_t i = 0U; i < dstCapacity; ++i) {
    if (src[i] == '\0') {
      std::memcpy(dst, src, i + 1U);
      return true;
    }
  }
  return false;
}

} // namespace engine::core
