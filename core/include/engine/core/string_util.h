// Bounded C-string copies shared engine-wide so every module truncates,
// refuses, and terminates identically. copy_string is the truncating form
// for display text and log/scratch buffers; copy_string_strict is the
// refusing form for identity-bearing fields (names that are hashed or
// looked up, asset and script paths), where a truncated copy would
// silently address something other than what the caller named. The
// ASCII case fold and the case-insensitive substring test every search
// field uses live here too, so no module folds case its own way, and so
// does the one rule for the short names authors type into lists: asset
// labels, entity tags, collision-layer names, save slots and recording
// names, which take letters from the scripts authors write in (CJK, kana
// and Hangul included). The case fold is ASCII's: those scripts have no
// case.

#pragma once

#include "engine/core/utf8.h"

#include <cstddef>
#include <cstdint>
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

/// True when `codePoint` may appear in a name token: an ASCII letter or
/// digit, '_', '-' or '.', or a letter from the scripts the engine's authors
/// write names in -- Latin-1 and Latin Extended, Greek, Cyrillic, CJK
/// ideographs, hiragana and katakana (with the prolonged-sound and iteration
/// marks), Hangul syllables, and full-width Latin letters and digits. A
/// documented subset of Unicode's identifier characters (annex UAX-31's
/// XID_Continue), as Godot's identifier rule is the full set. Combining
/// marks are not letters here, so a decomposed (NFD) spelling such as か
/// followed by U+3099 is refused rather than becoming a second name for が:
/// only one spelling of a name is ever stored.
[[nodiscard]] constexpr bool
name_token_code_point_allowed(std::uint32_t codePoint) noexcept {
  if (codePoint < 0x80U) {
    return ((codePoint >= 'a') && (codePoint <= 'z')) ||
           ((codePoint >= 'A') && (codePoint <= 'Z')) ||
           ((codePoint >= '0') && (codePoint <= '9')) || (codePoint == '_') ||
           (codePoint == '-') || (codePoint == '.');
  }
  struct Range final {
    std::uint32_t first;
    std::uint32_t last;
  };
  constexpr Range kLetters[] = {
      {0x00C0U, 0x00D6U}, {0x00D8U, 0x00F6U}, {0x00F8U, 0x024FU},
      {0x0386U, 0x0386U}, {0x0388U, 0x03CEU}, {0x0400U, 0x0481U},
      {0x048AU, 0x04FFU}, {0x3005U, 0x3006U}, {0x3041U, 0x3096U},
      {0x309DU, 0x309FU}, {0x30A1U, 0x30FAU}, {0x30FCU, 0x30FFU},
      {0x3400U, 0x4DBFU}, {0x4E00U, 0x9FFFU}, {0xAC00U, 0xD7A3U},
      {0xF900U, 0xFAFFU}, {0xFF10U, 0xFF19U}, {0xFF21U, 0xFF3AU},
      {0xFF41U, 0xFF5AU}, {0xFF66U, 0xFF9DU}, {0x20000U, 0x2FA1FU},
  };
  for (const Range &range : kLetters) {
    if ((codePoint >= range.first) && (codePoint <= range.last)) {
      return true;
    }
  }
  return false;
}

/// True when `text` is a name token: well-formed UTF-8 of 1 to `maxLength`
/// bytes, every code point one name_token_code_point_allowed accepts. Asset
/// labels, entity tags, collision-layer names, save slots and recording
/// names are tokens, so a search term such as "l:hero" names exactly one
/// label, a tag never needs quoting and a name is always a safe file stem.
/// The limit is in bytes, so storage stays fixed: a 31-byte token holds 31
/// ASCII characters or 10 CJK ones. Never truncates: text longer than
/// `maxLength` is not a token.
[[nodiscard]] inline bool name_token_is_valid(const char *text,
                                              std::size_t maxLength) noexcept {
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  std::size_t length = 0U;
  while (text[length] != '\0') {
    std::uint32_t codePoint = 0U;
    const std::size_t width = utf8_decode(text + length, &codePoint);
    if ((width == 0U) || !name_token_code_point_allowed(codePoint)) {
      return false;
    }
    length += width;
    if (length > maxLength) {
      return false;
    }
  }
  return true;
}

} // namespace engine::core
