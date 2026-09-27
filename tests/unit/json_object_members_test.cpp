// Verifies JsonParser's object-member walk: members come back in document
// order with their keys decodable, duplicates each count, an empty object
// or a non-object has none, and an index past the end is refused. A strict
// reader depends on this to refuse keys it does not know and keys that
// appear twice.

#include "engine/core/json.h"

#include <cstring>

#include "../test_harness.h"

int main() {
  engine::tests::TestContext t;
  engine::core::JsonParser parser{};
  const char *text = "{\"a\":1, \"b\\u0021\":[2,3], \"a\":{\"c\":4}}";
  t.check(parser.parse(text, std::strlen(text)), "the document parses");
  const engine::core::JsonValue root = *parser.root();
  t.check(parser.object_size(root) == 3U, "three members, duplicate counted");

  engine::core::JsonValue key{};
  engine::core::JsonValue value{};
  char name[16] = {};
  std::uint32_t number = 0U;
  t.check(parser.get_object_member(root, 0U, &key, &value) &&
              parser.copy_string_strict(key, name, sizeof(name)) &&
              (std::strcmp(name, "a") == 0) && parser.as_uint(value, &number) &&
              (number == 1U),
          "the first member is a = 1");
  t.check(parser.get_object_member(root, 1U, &key, &value) &&
              parser.copy_string_strict(key, name, sizeof(name)) &&
              (std::strcmp(name, "b!") == 0) &&
              (parser.array_size(value) == 2U),
          "the second key decodes its escape and keeps its array");
  t.check(parser.get_object_member(root, 2U, &key, &value) &&
              parser.copy_string_strict(key, name, sizeof(name)) &&
              (std::strcmp(name, "a") == 0) &&
              (value.type == engine::core::JsonValue::Type::Object),
          "the duplicate key is the third member");
  t.check(!parser.get_object_member(root, 3U, &key, &value),
          "an index past the end is refused");

  const char *empty = "{ }";
  t.check(parser.parse(empty, std::strlen(empty)) &&
              (parser.object_size(*parser.root()) == 0U) &&
              !parser.get_object_member(*parser.root(), 0U, &key, &value),
          "an empty object has no members");
  const char *array = "[1,2]";
  t.check(parser.parse(array, std::strlen(array)) &&
              (parser.object_size(*parser.root()) == 0U) &&
              !parser.get_object_member(*parser.root(), 0U, &key, &value),
          "an array has no members");
  return t.finish("json_object_members");
}
