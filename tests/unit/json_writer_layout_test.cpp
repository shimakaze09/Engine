// Pins JsonWriter's two layouts and its float text. Lines puts one object
// member per line, keeps an array of scalars on its member's line and
// gives each object of an array its own lines, so a diff of an authored
// document names the field that changed. Compact is unchanged. A float is
// written with the fewest digits that read back as the same float, in
// either layout.

#include "engine/core/json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace {

using engine::core::JsonLayout;
using engine::core::JsonParser;
using engine::core::JsonValue;
using engine::core::JsonWriter;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// A document with every shape a scene uses: nested objects, a scalar
/// array, an array of objects, and empty containers.
void write_sample(JsonWriter *writer) noexcept {
  writer->begin_object();
  writer->write_uint("version", 6U);
  writer->begin_array("position");
  writer->write_float_value(0.0F);
  writer->write_float_value(1.5F);
  writer->write_float_value(-2.0F);
  writer->end_array();
  writer->begin_array("entities");
  writer->begin_object();
  writer->write_string("name", "A");
  writer->write_bool("active", true);
  writer->end_object();
  writer->begin_object();
  writer->write_key("components");
  writer->begin_object();
  writer->end_object();
  writer->begin_array("tags");
  writer->end_array();
  writer->end_object();
  writer->end_array();
  writer->end_object();
}

void check_lines_layout() {
  std::unique_ptr<JsonWriter> writer(new (std::nothrow)
                                         JsonWriter(JsonLayout::Lines));
  if (writer == nullptr) {
    CHECK(false, "allocate the writer");
    return;
  }
  write_sample(writer.get());
  CHECK(writer->ok(), "the Lines document completes");
  constexpr const char *kExpected = "{\n"
                                    "  \"version\": 6,\n"
                                    "  \"position\": [0, 1.5, -2],\n"
                                    "  \"entities\": [\n"
                                    "    {\n"
                                    "      \"name\": \"A\",\n"
                                    "      \"active\": true\n"
                                    "    },\n"
                                    "    {\n"
                                    "      \"components\": {},\n"
                                    "      \"tags\": []\n"
                                    "    }\n"
                                    "  ]\n"
                                    "}\n";
  CHECK(std::strcmp(writer->result(), kExpected) == 0,
        "Lines: one member per line, scalar arrays inline, a trailing "
        "newline");
  if (std::strcmp(writer->result(), kExpected) != 0) {
    std::fprintf(stderr, "got:\n%s", writer->result());
  }

  // The layout survives a reset, and the text parses back.
  writer->reset();
  write_sample(writer.get());
  CHECK(std::strcmp(writer->result(), kExpected) == 0,
        "the layout survives reset()");
  std::unique_ptr<JsonParser> parser(new (std::nothrow) JsonParser());
  CHECK((parser != nullptr) &&
            parser->parse(writer->result(), writer->result_size()),
        "the Lines document parses");
}

void check_compact_layout() {
  std::unique_ptr<JsonWriter> writer(new (std::nothrow) JsonWriter());
  if (writer == nullptr) {
    CHECK(false, "allocate the writer");
    return;
  }
  write_sample(writer.get());
  CHECK(std::strcmp(writer->result(),
                    "{\"version\":6,\"position\":[0,1.5,-2],\"entities\":"
                    "[{\"name\":\"A\",\"active\":true},{\"components\":{},"
                    "\"tags\":[]}]}") == 0,
        "Compact: no whitespace, as before");
}

/// Writes `value` alone in an array and returns the text between the
/// brackets.
bool float_text(float value, char *out, std::size_t capacity) {
  std::unique_ptr<JsonWriter> writer(new (std::nothrow) JsonWriter());
  if (writer == nullptr) {
    return false;
  }
  writer->begin_array();
  writer->write_float_value(value);
  writer->end_array();
  const std::size_t size = writer->result_size();
  if (!writer->ok() || (size < 2U) || (size - 1U > capacity)) {
    return false;
  }
  std::memcpy(out, writer->result() + 1, size - 2U);
  out[size - 2U] = '\0';
  return true;
}

void check_float_text() {
  struct Case {
    float value;
    const char *text;
  };
  // Each is the shortest text strtof reads back as exactly this float;
  // %.9g wrote them as 0.150000006, 0.100000001, 0.300000012 and so on.
  const Case cases[] = {
      {0.15F, "0.15"},
      {0.1F, "0.1"},
      {0.3F, "0.3"},
      {1.0F, "1"},
      {-0.0F, "-0"},
      {1.0e-7F, "1e-07"},
      {16777216.0F, "16777216"},
      {3.14159274F, "3.1415927"},
      {1.17549435e-38F, "1.1754944e-38"},
  };
  for (const Case &c : cases) {
    char text[32] = {};
    CHECK(float_text(c.value, text, sizeof(text)) &&
              (std::strcmp(text, c.text) == 0),
          c.text);
  }

  // Exhaustive in the part of the range authored values live in: every
  // float from 0.001 to 1000 on a coarse bit stride reads back bitwise.
  std::uint32_t bits = 0U;
  float start = 0.001F;
  std::memcpy(&bits, &start, sizeof(bits));
  int mismatches = 0;
  for (float value = start; value < 1000.0F;) {
    char text[32] = {};
    if (!float_text(value, text, sizeof(text))) {
      ++mismatches;
      break;
    }
    const float parsed = std::strtof(text, nullptr);
    if (std::memcmp(&parsed, &value, sizeof(float)) != 0) {
      ++mismatches;
    }
    bits += 997U;
    std::memcpy(&value, &bits, sizeof(value));
  }
  CHECK(mismatches == 0, "every written float reads back as itself");
}

} // namespace

/// Runs this executable or test program.
int main() {
  check_lines_layout();
  check_compact_layout();
  check_float_text();
  if (g_failures != 0) {
    return 1;
  }
  std::printf("PASS: json writer layout\n");
  return 0;
}
