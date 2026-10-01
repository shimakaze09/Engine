// Bounded C-string copies shared engine-wide so every module truncates,
// refuses, and terminates identically. copy_string is the truncating form
// for display text and log/scratch buffers; copy_string_strict is the
// refusing form for identity-bearing fields (names that are hashed or
// looked up, asset and script paths), where a truncated copy would
// silently address something other than what the caller named. The
// ASCII case fold and the case-insensitive substring test every search
// field uses live here too, so no module folds case its own way, and so
// does the one rule for the short names authors type into lists: asset
// labels, entity tags and recording names.

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

/// Folds an ASCII letter to lower case and leaves every other byte as it
/// is. Unlike std::tolower it ignores the C locale, so a search or an
/// extension match is the same on every machine, and a UTF-8 byte is never
/// rewritten.
[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
  return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c;
}

/// True when `needle` occurs in `haystack` ignoring ASCII case. An empty or
/// null needle matches everything; a null haystack matches only that.
[[nodiscard]] inline bool contains_ignoring_case(const char *haystack,
                                                 const char *needle) noexcept {
  if ((needle == nullptr) || (needle[0] == '\0')) {
    return true;
  }
  if (haystack == nullptr) {
    return false;
  }
  for (const char *start = haystack; *start != '\0'; ++start) {
    std::size_t i = 0U;
    while ((needle[i] != '\0') && (start[i] != '\0') &&
           (ascii_lower(start[i]) == ascii_lower(needle[i]))) {
      ++i;
    }
    if (needle[i] == '\0') {
      return true;
    }
    if (start[i] == '\0') {
      return false;
    }
  }
  return false;
}

/// True when `a` and `b` are the same text ignoring ASCII case. Null equals
/// nothing, not even null.
[[nodiscard]] inline bool equals_ignoring_case(const char *a,
                                               const char *b) noexcept {
  if ((a == nullptr) || (b == nullptr)) {
    return false;
  }
  for (std::size_t i = 0U;; ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) {
      return false;
    }
    if (a[i] == '\0') {
      return true;
    }
  }
}

/// True when `text` is a name token: 1 to `maxLength` characters, each a
/// letter, digit, '_', '-' or '.'. Asset labels, entity tags and recording
/// names are tokens, so a search term such as "l:hero" names exactly one
/// label, a tag never needs quoting and a name is always a safe file stem.
/// Never truncates: text longer than `maxLength` is not a token.
[[nodiscard]] inline bool name_token_is_valid(const char *text,
                                              std::size_t maxLength) noexcept {
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  for (std::size_t length = 0U; text[length] != '\0'; ++length) {
    const char c = text[length];
    const bool allowed =
        ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
        ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-') || (c == '.');
    if (!allowed || (length >= maxLength)) {
      return false;
    }
  }
  return true;
}

} // namespace engine::core
