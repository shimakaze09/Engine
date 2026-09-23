// Checks the generated build identity is actually substituted and usable.
// The header is written by CMake from a template, and a compile cannot
// catch a substitution that went wrong: the string stays well-formed
// either way, so engine_build_id() would hand a crash report an identity
// naming nothing.
//
// The two shapes that matter fail differently. A token whose variable was
// never defined -- a renamed or misspelled one -- substitutes to the empty
// string, collapsing two separators together, which is what the
// empty-field check below catches. A malformed token, unterminated or
// misspelled past the closing @, is copied through verbatim and leaves its
// '@' behind. Both checks are here because neither finds the other.

#include "engine/core/engine_version.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

} // namespace

/// Runs this executable or test program.
int main() {
  const char *id = engine::core::engine_build_id();
  CHECK(id != nullptr, "the build id exists");
  if (id == nullptr) {
    return 1;
  }

  // A malformed token is copied through as written.
  CHECK(std::strchr(id, '@') == nullptr,
        "no unsubstituted template token remains");

  // Six space-separated fields: version, revision, compiler id, compiler
  // version, build type, platform, float flags -- the compiler field
  // carries a space of its own, so count separators rather than fields.
  std::size_t spaces = 0U;
  for (const char *c = id; *c != '\0'; ++c) {
    if (*c == ' ') {
      ++spaces;
    }
  }
  CHECK(spaces >= 5U, "the id carries every field");

  CHECK(std::strstr(id, engine::core::engine_version_string()) != nullptr,
        "the id names the engine version");

  // "unknown" is the documented fallback for a source drop with no git
  // history, so it is not a failure here -- but it must be the explicit
  // word. An undefined variable substitutes to nothing instead, and the
  // two separators around the missing field collapse into one.
  CHECK(std::strstr(id, "  ") == nullptr, "no field is empty");

  // The identity is written straight into a C string literal by CMake. A
  // quote or a backslash reaching it would either break the build or, if
  // it did not, escape the literal; neither belongs in a revision.
  CHECK(std::strchr(id, '"') == nullptr, "the id carries no quote");
  CHECK(std::strchr(id, '\\') == nullptr, "the id carries no backslash");

  if (g_failures != 0) {
    std::fprintf(stderr, "build id was \"%s\"\n", id);
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::printf("build identity: %s\n", id);
  return 0;
}
