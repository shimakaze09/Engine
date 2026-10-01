// Verifies JsonReadTracker and json_visit_unread_members: a parser with a
// tracker set records each member get_object_field finds, and the walk
// names every member no lookup found, by path, without entering it. A
// scene or prefab loader relies on this to report the keys it would
// otherwise drop on the next save. The top-level variant hands a writer
// each unread member's exact text, so it can carry a newer build's keys
// through a rewrite.

#include "engine/core/json.h"

#include <cstring>
#include <string>
#include <vector>

#include "../test_harness.h"

namespace {

std::vector<std::string> g_paths{};

void collect(const char *path, void * /*userData*/) noexcept {
  g_paths.emplace_back(path);
}

void collect_raw(const char *key, std::size_t keyLength, const char *value,
                 std::size_t valueLength, void * /*userData*/) noexcept {
  g_paths.emplace_back(std::string(key, keyLength) + "=" +
                       std::string(value, valueLength));
}

bool paths_are(const std::vector<std::string> &expected) {
  return g_paths == expected;
}

} // namespace

int main() {
  using engine::core::JsonParser;
  using engine::core::JsonReadTracker;
  using engine::core::JsonValue;
  engine::tests::TestContext t;

  const char *text = "{\"a\":1, \"b\":{\"c\":2,\"d\":3},"
                     " \"e\":[{\"f\":4},{\"g\":5,\"h\":6},[{\"i\":7}]],"
                     " \"x\":{\"y\":1}}";
  const std::size_t length = std::strlen(text);
  JsonParser parser{};
  t.check(parser.parse(text, length), "the document parses");
  const JsonValue root = *parser.root();

  JsonReadTracker tracker{};
  t.check(tracker.reset_for(text, length) && tracker.armed(),
          "the tracker is sized for the document");

  g_paths.clear();
  t.check((engine::core::json_visit_unread_members(root, tracker, &collect,
                                                   nullptr) == 4U) &&
              paths_are({"a", "b", "e", "x"}),
          "with nothing read, each root member is named and none is entered");

  parser.set_read_tracker(&tracker);
  JsonValue value{};
  JsonValue element{};
  t.check(parser.get_object_field(root, "a", &value) &&
              parser.get_object_field(root, "a", &value),
          "a member can be looked up twice");
  const JsonValue *b = parser.get_object_field(root, "b");
  t.check((b != nullptr) && parser.get_object_field(*b, "c", &value),
          "the pointer lookup records its member too");
  t.check(parser.get_object_field(root, "e", &value) &&
              parser.get_array_element(value, 1U, &element) &&
              parser.get_object_field(element, "g", &element),
          "a member of an array element is looked up");
  t.check(!parser.get_object_field(root, "missing", &value),
          "a missing member records nothing");
  parser.set_read_tracker(nullptr);
  t.check(parser.get_object_field(root, "x", &value),
          "a lookup after the tracker is cleared still answers");

  g_paths.clear();
  t.check((engine::core::json_visit_unread_members(root, tracker, &collect,
                                                   nullptr) == 5U) &&
              paths_are({"b.d", "e[0].f", "e[1].h", "e[2][0].i", "x"}),
          "the walk names each unread member by path, in document order");

  JsonReadTracker unarmed{};
  t.check(!unarmed.reset_for(nullptr, 0U) && !unarmed.armed(),
          "an empty document leaves the tracker unarmed");
  g_paths.clear();
  t.check((engine::core::json_visit_unread_members(root, unarmed, &collect,
                                                   nullptr) == 0U) &&
              g_paths.empty(),
          "an unarmed tracker visits nothing");

  const char *elsewhere = "{\"a\":1}";
  tracker.record(elsewhere + 5);
  t.check(!tracker.was_read(elsewhere + 5),
          "a pointer outside the document is ignored");

  const char *arrayText = "[1,2]";
  JsonReadTracker arrayTracker{};
  t.check(parser.parse(arrayText, std::strlen(arrayText)) &&
              arrayTracker.reset_for(arrayText, std::strlen(arrayText)) &&
              (engine::core::json_visit_unread_members(
                   *parser.root(), arrayTracker, &collect, nullptr) == 0U),
          "a root that is not an object has no members to report");
  const char *carried = "{\"known\":1, \"s\":\"a \\\"q\\\"\", "
                        "\"n\":{\"x\": [1, 2]}, \"known\":2, \"t\":true}";
  JsonReadTracker carryTracker{};
  t.check(parser.parse(carried, std::strlen(carried)) &&
              carryTracker.reset_for(carried, std::strlen(carried)),
          "a document with members a reader does not know");
  parser.set_read_tracker(&carryTracker);
  t.check(parser.get_object_field(*parser.root(), "known", &value),
          "the reader looks up the one key it knows");
  parser.set_read_tracker(nullptr);
  g_paths.clear();
  t.check((engine::core::json_visit_unread_top_level_members(
               parser, *parser.root(), carryTracker, &collect_raw, nullptr) ==
           3U) &&
              paths_are({"s=\"a \\\"q\\\"\"", "n={\"x\": [1, 2]}", "t=true"}),
          "each unread member's exact text, a string with its quotes, and "
          "not the repeat of a key that was read");
  return t.finish("json_read_tracker");
}
