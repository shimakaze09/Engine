// Verifies the two bounded C-string copies in core/string_util.h: the
// truncating copy_string always terminates inside the destination, and the
// refusing copy_string_strict copies only a string that fits whole, leaves
// the destination empty otherwise, and never reads past the destination's
// capacity in the source (so an unterminated same-sized array is refused,
// not scanned).

#include "engine/core/string_util.h"

#include <cstdio>
#include <cstring>

namespace {

using engine::core::copy_string;
using engine::core::copy_string_strict;

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

} // namespace

int main() {
  const int results[] = {
      check_strict_bound(),
      check_strict_unterminated_source(),
      check_strict_null_arguments(),
      check_truncating_copy(),
  };
  for (const int result : results) {
    if (result != 0) {
      std::fprintf(stderr, "string_util_test failed with code %d\n", result);
      return result;
    }
  }
  return 0;
}
