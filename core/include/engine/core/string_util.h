// Bounded C-string copy shared engine-wide so every module truncates and
// terminates identically (replaces five per-module reimplementations).

#pragma once

#include <cstddef>

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

} // namespace engine::core
