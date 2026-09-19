// Regression for #515: iterating an array whose elements contain nested
// arrays must cost work linear in the element count. JsonParser keeps a
// resume memo per recently walked array; with a single memo, every nested
// walk (a Transform's position array) evicted the outer array's memo, so
// the entity loop of every real scene re-parsed entities 0..i on each
// iteration. The cost is measured by the parser's own element-scan
// counter, never by wall-clock time, and the values read back are checked
// against what was written so the memo cannot return the wrong element.

#include "engine/core/json.h"

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

/// Builds {"entities":[{"id":i,"p":[i,i+1,i+2],"r":[0,0,0,1]} ...]}.
std::string build_scene(std::size_t entityCount) {
  std::string json = "{\"entities\":[";
  char element[96] = {};
  for (std::size_t i = 0U; i < entityCount; ++i) {
    std::snprintf(element, sizeof(element),
                  "%s{\"id\":%zu,\"p\":[%zu,%zu,%zu],\"r\":[0,0,0,1]}",
                  (i == 0U) ? "" : ",", i, i, i + 1U, i + 2U);
    json += element;
  }
  json += "]}";
  return json;
}

/// Walks every entity the way scene load does — element, then the nested
/// arrays inside it — and returns the scan count, or 0 on a wrong value.
std::size_t walk_scene(const std::string &json, std::size_t entityCount) {
  engine::core::JsonParser parser{};
  if (!parser.parse(json.data(), json.size()) || (parser.root() == nullptr)) {
    return 0U;
  }
  engine::core::JsonValue entities{};
  if (!parser.get_object_field(*parser.root(), "entities", &entities)) {
    return 0U;
  }
  for (std::size_t i = 0U; i < entityCount; ++i) {
    engine::core::JsonValue entity{};
    engine::core::JsonValue id{};
    engine::core::JsonValue position{};
    engine::core::JsonValue rotation{};
    std::uint32_t idValue = 0U;
    float p[3] = {};
    float r[4] = {};
    if (!parser.get_array_element(entities, i, &entity) ||
        !parser.get_object_field(entity, "id", &id) ||
        !parser.as_uint(id, &idValue) || (idValue != i) ||
        !parser.get_object_field(entity, "p", &position) ||
        !parser.as_float_array(position, p, 3U) ||
        (p[0] != static_cast<float>(i)) ||
        (p[2] != static_cast<float>(i + 2U)) ||
        !parser.get_object_field(entity, "r", &rotation) ||
        !parser.as_float_array(rotation, r, 4U) || (r[3] != 1.0F)) {
      return 0U;
    }
  }
  return parser.array_element_scans();
}

} // namespace

/// Runs this executable or test program.
int main() {
  constexpr std::size_t kSmall = 1000U;
  constexpr std::size_t kLarge = 4000U;
  const std::size_t smallScans = walk_scene(build_scene(kSmall), kSmall);
  const std::size_t largeScans = walk_scene(build_scene(kLarge), kLarge);
  CHECK(smallScans != 0U, "small scene walks and reads back exactly");
  CHECK(largeScans != 0U, "large scene walks and reads back exactly");

  // Linear: each entity costs one outer element scan plus its two nested
  // arrays' element scans (3 + 4), so 4x the entities is 4x the scans.
  // Quadratic re-parsing of the outer array puts the large walk at ~16x.
  CHECK(largeScans <= (4U * smallScans) + 64U,
        "scans grow linearly with the entity count");
  std::printf("scans: %zu entities -> %zu, %zu entities -> %zu\n", kSmall,
              smallScans, kLarge, largeScans);

  // Memo correctness across arrays: alternating walks of two arrays must
  // both resume, and a descending index must still find the right element.
  {
    const char *json = "{\"a\":[10,11,12,13],\"b\":[20,21,22,23]}";
    engine::core::JsonParser parser{};
    engine::core::JsonValue a{};
    engine::core::JsonValue b{};
    CHECK(parser.parse(json, std::strlen(json)), "pair parses");
    CHECK(parser.get_object_field(*parser.root(), "a", &a) &&
              parser.get_object_field(*parser.root(), "b", &b),
          "pair fields resolve");
    const std::size_t order[] = {0U, 0U, 1U, 1U, 3U, 3U, 2U, 2U};
    for (std::size_t step = 0U; step < 8U; ++step) {
      const std::size_t index = order[step];
      const bool fromA = (step % 2U) == 0U;
      engine::core::JsonValue element{};
      std::uint32_t value = 0U;
      CHECK(parser.get_array_element(fromA ? a : b, index, &element) &&
                parser.as_uint(element, &value) &&
                (value == (fromA ? 10U : 20U) + index),
            "alternating and descending walks return the right element");
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("json_navigation_test passed");
  return 0;
}
