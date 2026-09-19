// Regression for #539: JsonParser's pointer-returning navigation draws on
// a fixed scratch ring that only parse() resets, so a long walk used to
// turn silently into "field missing" after kScratchSlots calls. The
// exhaustion is now observable: scratch_exhausted() reports it, exactly
// one warning is logged per parse, parse() clears it, and the by-value
// overloads walk the same document without touching the ring.

#include "engine/core/json.h"
#include "engine/core/logging.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

int g_scratchWarnings = 0;

void counting_sink(engine::core::LogLevel level, const char *channel,
                   const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Warning) &&
      (std::strcmp(channel, "json") == 0) &&
      (std::strstr(message, "scratch exhausted") != nullptr)) {
    ++g_scratchWarnings;
  }
}

/// Builds [0,1,...,count-1].
std::string build_array(std::size_t count) {
  std::string json = "[";
  char element[24] = {};
  for (std::size_t i = 0U; i < count; ++i) {
    std::snprintf(element, sizeof(element), "%s%zu", (i == 0U) ? "" : ",", i);
    json += element;
  }
  json += "]";
  return json;
}

} // namespace

/// Runs this executable or test program.
int main() {
  constexpr std::size_t kSlots = engine::core::JsonParser::kScratchSlots;
  constexpr std::size_t kCount = kSlots + 100U;
  const std::string json = build_array(kCount);
  engine::core::JsonParser parser{};
  CHECK(engine::core::initialize_logging(), "logging initialized");
  CHECK(engine::core::log_register_sink(&counting_sink, nullptr),
        "sink registered");

  CHECK(parser.parse(json.c_str(), json.size()), "array parses");
  CHECK(!parser.scratch_exhausted(), "a fresh parse is not exhausted");

  // Pointer walk: exactly kScratchSlots succeed, the rest read as missing.
  std::size_t returned = 0U;
  for (std::size_t i = 0U; i < kCount; ++i) {
    if (parser.get_array_element(*parser.root(), i) != nullptr) {
      ++returned;
    }
  }
  CHECK(returned == kSlots, "the pointer walk stops at the ring size");
  CHECK(parser.scratch_exhausted(), "exhaustion is reported");
  CHECK(g_scratchWarnings == 1, "exhaustion is logged exactly once per parse");

  // By-value walk after exhaustion: every element still reads exactly.
  bool exact = true;
  for (std::size_t i = 0U; i < kCount; ++i) {
    engine::core::JsonValue value{};
    std::uint32_t number = 0U;
    if (!parser.get_array_element(*parser.root(), i, &value) ||
        !parser.as_uint(value, &number) || (number != i)) {
      exact = false;
    }
  }
  CHECK(exact, "the by-value overloads read the whole array");
  CHECK(g_scratchWarnings == 1, "by-value navigation never logs");

  // parse() resets the ring and the flag; a second exhaustion logs again.
  CHECK(parser.parse(json.c_str(), json.size()), "array re-parses");
  CHECK(!parser.scratch_exhausted(), "parse clears the exhaustion");
  CHECK(parser.get_array_element(*parser.root(), 0U) != nullptr,
        "the ring is usable again after parse");
  for (std::size_t i = 1U; i < kCount; ++i) {
    static_cast<void>(parser.get_array_element(*parser.root(), i));
  }
  CHECK(g_scratchWarnings == 2, "each parse logs its own exhaustion once");

  engine::core::log_unregister_sink(&counting_sink, nullptr);
  engine::core::shutdown_logging();
  if (g_failures != 0) {
    std::fprintf(stderr, "json_scratch_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("json_scratch_test passed");
  return 0;
}
