// Verifies the two bounded C-string copies in core/string_util.h: the
// truncating copy_string always terminates inside the destination, and the
// refusing copy_string_strict copies only a string that fits whole, leaves
// the destination empty otherwise, and never reads past the destination's
// capacity in the source (so an unterminated same-sized array is refused,
// not scanned). Also the ASCII case fold, which leaves non-letters and
// UTF-8 bytes alone, and the case-insensitive substring test every search
// field shares.

#include "engine/core/string_util.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

using engine::core::ascii_lower;
using engine::core::contains_ignoring_case;
using engine::core::copy_string;
using engine::core::copy_string_strict;
using engine::core::equals_ignoring_case;
using engine::core::name_token_is_valid;

static_assert(ascii_lower('A') == 'a' && ascii_lower('Z') == 'z' &&
                  ascii_lower('a') == 'a' && ascii_lower('@') == '@' &&
                  ascii_lower('[') == '[' &&
                  ascii_lower(static_cast<char>(0xC3)) ==
                      static_cast<char>(0xC3),
              "only ASCII letters fold");

/// Matches anywhere, ignoring ASCII case; an empty or null needle matches
/// everything, a null haystack only that, and a needle longer than the
/// haystack or one that runs off its end does not match.
int check_contains_ignoring_case() {
  if (!contains_ignoring_case("assets/Textures/Rock_Albedo.png", "rock_al") ||
      !contains_ignoring_case("ABC", "abc") ||
      !contains_ignoring_case("xxabc", "ABC") ||
      !contains_ignoring_case("abc", "") ||
      !contains_ignoring_case("abc", nullptr) ||
      !contains_ignoring_case(nullptr, "")) {
    return 40;
  }
  if (contains_ignoring_case("ab", "abc") ||
      contains_ignoring_case("xxab", "abc") ||
      contains_ignoring_case("abd", "abc") ||
      contains_ignoring_case(nullptr, "a") || contains_ignoring_case("", "a")) {
    return 41;
  }
  // A UTF-8 name matches itself byte for byte and is never case-folded.
  if (!contains_ignoring_case("\xE6\xA4\x85\xE5\xAD\x90.mesh",
                              "\xE5\xAD\x90") ||
      contains_ignoring_case("\xC3\x89", "\xC3\xA9")) {
    return 42;
  }
  return 0;
}

/// A string one shorter than the capacity fits whole; exactly the capacity
/// (no room for the terminator) is refused with an empty destination.
int check_strict_bound() {
  char dst[8] = "stale";
  if (!copy_string_strict(dst, sizeof(dst), "1234567") ||
      (std::strcmp(dst, "1234567") != 0)) {
    return 1;
  }
  if (copy_string_strict(dst, sizeof(dst), "12345678") || (dst[0] != '\0')) {
    return 2;
  }
  if (!copy_string_strict(dst, sizeof(dst), "") || (dst[0] != '\0')) {
    return 3;
  }
  return 0;
}

/// A same-sized source array with no terminator is refused: the scan stops
/// at the capacity instead of reading beyond the array.
int check_strict_unterminated_source() {
  char src[8];
  std::memset(src, 'x', sizeof(src));
  char dst[8] = "stale";
  if (copy_string_strict(dst, sizeof(dst), src) || (dst[0] != '\0')) {
    return 10;
  }
  return 0;
}

/// Null and zero-capacity arguments: a null source is an empty string, a
/// null destination or zero capacity fails without touching anything.
int check_strict_null_arguments() {
  char dst[4] = "abc";
  if (!copy_string_strict(dst, sizeof(dst), nullptr) || (dst[0] != '\0')) {
    return 20;
  }
  if (copy_string_strict(nullptr, 4U, "a")) {
    return 21;
  }
  char untouched[4] = "abc";
  if (copy_string_strict(untouched, 0U, "a") ||
      (std::strcmp(untouched, "abc") != 0)) {
    return 22;
  }
  return 0;
}

/// The truncating copy keeps its contract beside the strict one: it cuts at
/// capacity - 1 and terminates.
int check_truncating_copy() {
  char dst[4] = {};
  copy_string(dst, sizeof(dst), "abcdef");
  if (std::strcmp(dst, "abc") != 0) {
    return 30;
  }
  copy_string(dst, sizeof(dst), nullptr);
  if (dst[0] != '\0') {
    return 31;
  }
  return 0;
}

/// The name-token rule: letters (CJK, kana and Hangul included), digits,
/// '_', '-' and '.', 1 to maxLength bytes of well-formed UTF-8, never
/// truncated; and case-insensitive equality.
int check_name_tokens() {
  if (!name_token_is_valid("coin", 4U) || !name_token_is_valid("a", 1U) ||
      !name_token_is_valid("Enemy_2.boss-A", 31U)) {
    return 40;
  }
  if (name_token_is_valid("coins", 4U) || name_token_is_valid("", 31U) ||
      name_token_is_valid(nullptr, 31U) || name_token_is_valid("a b", 31U) ||
      name_token_is_valid("a/b", 31U)) {
    return 41;
  }
  // Authors name things in their own script (#1185): 敌人 (Chinese), 地面,
  // 角色, がめん and パワー (kana with the prolonged-sound mark), 주인공
  // (Hangul), café (Latin-1), and full-width Ａ１.
  for (const char *name :
       {"\xE6\x95\x8C\xE4\xBA\xBA", "\xE5\x9C\xB0\xE9\x9D\xA2",
        "\xE8\xA7\x92\xE8\x89\xB2", "\xE3\x81\x8C\xE3\x82\x81\xE3\x82\x93",
        "\xE3\x83\x91\xE3\x83\xAF\xE3\x83\xBC",
        "\xEC\xA3\xBC\xEC\x9D\xB8\xEA\xB3\xB5", "caf\xC3\xA9",
        "\xEF\xBC\xA1\xEF\xBC\x91", "enemy_\xE6\x95\x8C"}) {
    if (!name_token_is_valid(name, 31U)) {
      return 43;
    }
  }
  // The limit is bytes: 主角 is six.
  if (!name_token_is_valid("\xE4\xB8\xBB\xE8\xA7\x92", 6U) ||
      name_token_is_valid("\xE4\xB8\xBB\xE8\xA7\x92", 5U)) {
    return 44;
  }
  // Not UTF-8: an overlong '/', a lone continuation byte, a truncated
  // sequence, an encoded surrogate, a code point past U+10FFFF.
  for (const char *name :
       {"\xC0\xAF", "\x80", "a\xE4\xB8", "\xED\xA0\x80", "\xF4\x90\x80\x80"}) {
    if (name_token_is_valid(name, 31U)) {
      return 45;
    }
  }
  // Not letters: an arrow, an emoji, a CJK full stop, a no-break space, and
  // the decomposed (NFD) spelling of が -- か then a combining voiced mark --
  // so a name has one spelling only.
  for (const char *name : {"a\xE2\x86\x92"
                           "b",
                           "\xF0\x9F\x98\x80", "\xE3\x80\x82",
                           "a\xC2\xA0"
                           "b",
                           "\xE3\x81\x8B\xE3\x82\x99"}) {
    if (name_token_is_valid(name, 31U)) {
      return 46;
    }
  }
  if (!equals_ignoring_case("Hero", "hERO") ||
      equals_ignoring_case("hero", "heroes") ||
      equals_ignoring_case("hero", nullptr) ||
      equals_ignoring_case(nullptr, nullptr) ||
      equals_ignoring_case("\xC3\x89", "\xC3\xA9")) {
    return 42;
  }
  return 0;
}

} // namespace

int main() {
  const int results[] = {
      check_strict_bound(),           check_strict_unterminated_source(),
      check_strict_null_arguments(),  check_truncating_copy(),
      check_contains_ignoring_case(), check_name_tokens(),
  };
  for (const int result : results) {
    if (result != 0) {
      std::fprintf(stderr, "string_util_test failed with code %d\n", result);
      return result;
    }
  }
  return 0;
}
