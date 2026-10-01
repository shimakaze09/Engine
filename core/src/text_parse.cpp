// Implements the strict whole-token number parsers in text_parse.h.

#include "engine/core/text_parse.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace engine::core {

bool parse_float_token(const char *text, float *out) noexcept {
  if ((text == nullptr) || (out == nullptr) || (text[0] == '\0') ||
      (std::isspace(static_cast<unsigned char>(text[0])) != 0)) {
    return false;
  }
  // strtof rather than std::from_chars: AppleClang's libc++ still deletes
  // the floating-point overload. The checks restore from_chars's
  // strictness.
  errno = 0;
  char *end = nullptr;
  const float parsed = std::strtof(text, &end);
  if ((end != text + std::strlen(text)) || (errno == ERANGE) ||
      !std::isfinite(parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

} // namespace engine::core
