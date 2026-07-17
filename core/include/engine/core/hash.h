// FNV-1a hash primitives shared engine-wide (replaces per-module copies of
// the constants and loops in event bus, world name lookup, asset ids, shader
// variant keys, and shadow cache keys).

#pragma once

#include <cstdint>

namespace engine::core {

inline constexpr std::uint32_t kFnv1a32Offset = 2166136261U;
inline constexpr std::uint32_t kFnv1a32Prime = 16777619U;
inline constexpr std::uint64_t kFnv1a64Offset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

/// Appends one byte to a 32-bit FNV-1a hash.
constexpr std::uint32_t fnv1a_32_append(std::uint32_t hash,
                                        std::uint8_t byte) noexcept {
  hash ^= static_cast<std::uint32_t>(byte);
  return hash * kFnv1a32Prime;
}

/// 32-bit FNV-1a of a null-terminated string; null hashes like "".
constexpr std::uint32_t fnv1a_32(const char *text) noexcept {
  std::uint32_t hash = kFnv1a32Offset;
  if (text != nullptr) {
    for (; *text != '\0'; ++text) {
      hash = fnv1a_32_append(hash, static_cast<std::uint8_t>(*text));
    }
  }
  return hash;
}

/// Appends one byte to a 64-bit FNV-1a hash.
constexpr std::uint64_t fnv1a_64_append(std::uint64_t hash,
                                        std::uint8_t byte) noexcept {
  hash ^= static_cast<std::uint64_t>(byte);
  return hash * kFnv1a64Prime;
}

/// 64-bit FNV-1a of a null-terminated string; null hashes like "".
constexpr std::uint64_t fnv1a_64(const char *text) noexcept {
  std::uint64_t hash = kFnv1a64Offset;
  if (text != nullptr) {
    for (; *text != '\0'; ++text) {
      hash = fnv1a_64_append(hash, static_cast<std::uint8_t>(*text));
    }
  }
  return hash;
}

/// Folds one whole 64-bit value into an FNV-1a-style hash in a single step.
/// NOTE: word-wise, not byte-classic FNV — used for in-memory cache keys
/// where speed matters and the exact byte sequence does not.
constexpr std::uint64_t fnv1a_64_append_u64(std::uint64_t hash,
                                            std::uint64_t value) noexcept {
  hash ^= value;
  return hash * kFnv1a64Prime;
}

} // namespace engine::core
