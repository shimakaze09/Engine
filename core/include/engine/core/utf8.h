// Decodes and validates UTF-8, the encoding every engine string is in. The
// decoder is strict, as the Unicode standard requires of a conforming one:
// an overlong form, a surrogate, a code point past U+10FFFF or a truncated
// sequence is not UTF-8, so the same text can never be spelled two ways.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Decodes the code point that starts at `text` into `*outCodePoint` and
/// returns how many bytes it took (1 to 4). Returns 0 when the bytes there
/// are not well-formed UTF-8, or `text` points at the terminator.
[[nodiscard]] inline std::size_t utf8_decode(const char *text,
                                             std::uint32_t *outCodePoint) noexcept {
  if ((text == nullptr) || (text[0] == '\0')) {
    return 0U;
  }
  const auto lead = static_cast<std::uint8_t>(text[0]);
  if (lead < 0x80U) {
    *outCodePoint = lead;
    return 1U;
  }
  std::size_t length = 0U;
  std::uint32_t codePoint = 0U;
  std::uint32_t minimum = 0U;
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2U;
    codePoint = lead & 0x1FU;
    minimum = 0x80U;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3U;
    codePoint = lead & 0x0FU;
    minimum = 0x800U;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4U;
    codePoint = lead & 0x07U;
    minimum = 0x10000U;
  } else {
    return 0U;
  }
  for (std::size_t i = 1U; i < length; ++i) {
    const auto next = static_cast<std::uint8_t>(text[i]);
    if ((next & 0xC0U) != 0x80U) {
      return 0U; // a continuation byte is missing (the terminator included)
    }
    codePoint = (codePoint << 6U) | (next & 0x3FU);
  }
  if ((codePoint < minimum) || (codePoint > 0x10FFFFU) ||
      ((codePoint >= 0xD800U) && (codePoint <= 0xDFFFU))) {
    return 0U;
  }
  *outCodePoint = codePoint;
  return length;
}

/// True when `text` is well-formed UTF-8 throughout (an empty string is).
[[nodiscard]] inline bool utf8_is_valid(const char *text) noexcept {
  if (text == nullptr) {
    return false;
  }
  while (*text != '\0') {
    std::uint32_t codePoint = 0U;
    const std::size_t length = utf8_decode(text, &codePoint);
    if (length == 0U) {
      return false;
    }
    text += length;
  }
  return true;
}

} // namespace engine::core
