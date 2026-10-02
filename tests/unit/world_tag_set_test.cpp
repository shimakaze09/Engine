// Verifies entity tags (TagSetComponent): the World refuses a set that is
// not name tokens, repeats a tag ignoring case or holds more than kMaxTags;
// find_entities_by_tag answers in ascending entity index whatever the
// storage order, drops destroyed entities, matches ignoring case and keeps
// the lowest indices when capped; the tag-set capacity refuses one past it;
// and scenes round-trip tags, write nothing for an untagged entity, and
// refuse a malformed, oversized or repeating tag array with the previous
// world kept.

#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::TagSetComponent;
using engine::runtime::World;

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

TagSetComponent tags_of(std::initializer_list<const char *> names) noexcept {
  TagSetComponent tags{};
  for (const char *name : names) {
    static_cast<void>(engine::runtime::tag_set_add(&tags, name));
  }
  return tags;
}

void check_set_rules() {
  TagSetComponent tags{};
  using engine::runtime::TagSetAdd;
  check(engine::runtime::tag_set_add(&tags, "coin") == TagSetAdd::Added,
        "rules: a token is added");
  check(engine::runtime::tag_set_add(&tags, "COIN") ==
            TagSetAdd::AlreadyPresent,
        "rules: a repeat ignoring case is already present");
  check(engine::runtime::tag_set_add(&tags, "has space") ==
            TagSetAdd::InvalidTag,
        "rules: a non-token is refused");
  check(engine::runtime::tag_set_add(
            &tags, "abcdefghijklmnopqrstuvwxyz012345") == TagSetAdd::InvalidTag,
        "rules: 32 characters are refused, never cut");
  check(engine::runtime::tag_set_add(
            &tags, "abcdefghijklmnopqrstuvwxyz01234") == TagSetAdd::Added,
        "rules: 31 characters fit");
  for (int i = 0; i < 6; ++i) {
    char name[8] = {};
    std::snprintf(name, sizeof(name), "t%d", i);
    check(engine::runtime::tag_set_add(&tags, name) == TagSetAdd::Added,
          "rules: tags up to the capacity are added");
  }
  check(tags.count == TagSetComponent::kMaxTags, "rules: the set is full");
  check(engine::runtime::tag_set_add(&tags, "ninth") == TagSetAdd::Full,
        "rules: one past the capacity is refused");
  check(
      engine::runtime::tag_set_remove(&tags, "Coin") && (tags.count == 7U) &&
          (std::strcmp(tags.tags[0], "abcdefghijklmnopqrstuvwxyz01234") == 0) &&
          (tags.tags[7][0] == '\0'),
      "rules: removing ignoring case keeps the rest in order");
  check(!engine::runtime::tag_set_remove(&tags, "coin"),
        "rules: removing an absent tag is false");
}

void check_world_ingress() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "ingress: world");
  if (world == nullptr) {
    return;
  }
  const Entity entity = world->create_entity();
  const TagSetComponent good = tags_of({"enemy", "boss"});
  check(world->add_tag_set_component(entity, good), "ingress: valid set");

  TagSetComponent repeated = good;
  std::memcpy(repeated.tags[1], "ENEMY", 6U);
  TagSetComponent invalid = good;
  std::memcpy(invalid.tags[1], "a/b", 4U);
  TagSetComponent oversized = good;
  oversized.count = TagSetComponent::kMaxTags + 1U;
  TagSetComponent unterminated = good;
  std::memset(unterminated.tags[1], 'x', TagSetComponent::kMaxTagLength + 1U);
  check(!world->add_tag_set_component(entity, repeated) &&
            !world->add_tag_set_component(entity, invalid) &&
            !world->add_tag_set_component(entity, oversized) &&
            !world->add_tag_set_component(entity, unterminated),
        "ingress: repeated, invalid, oversized and unterminated sets are "
        "refused");
  TagSetComponent kept{};
  check(world->get_tag_set_component(entity, &kept) && (kept.count == 2U) &&
            (std::strcmp(kept.tags[1], "boss") == 0),
        "ingress: a refused set leaves the entity's tags unchanged");
}

void check_find_by_tag() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "find: world");
  if (world == nullptr) {
    return;
  }
  Entity entities[6] = {};
  for (Entity &entity : entities) {
    entity = world->create_entity();
  }
  // Tagged out of index order so storage order differs from index order.
  const int order[] = {4, 1, 5, 0, 2};
  for (const int i : order) {
    check(world->add_tag_set_component(
              entities[i], tags_of({(i % 2 == 0) ? "Coin" : "enemy"})),
          "find: tag set added");
  }
  Entity found[8] = {};
  check(world->find_entities_by_tag("coin", found, 8U) == 3U &&
            (found[0] == entities[0]) && (found[1] == entities[2]) &&
            (found[2] == entities[4]),
        "find: matches ignoring case, in ascending entity index");
  check(world->find_entities_by_tag("nothing", found, 8U) == 0U,
        "find: an unused tag finds nothing");

  // A remove swaps the last stored set into the hole; order still follows
  // the index.
  check(world->remove_tag_set_component(entities[4]), "find: one removed");
  check(world->add_tag_set_component(entities[3], tags_of({"coin"})),
        "find: another added");
  check(world->find_entities_by_tag("coin", found, 8U) == 3U &&
            (found[0] == entities[0]) && (found[1] == entities[2]) &&
            (found[2] == entities[3]),
        "find: storage reorders do not change the order");

  check(world->destroy_entity(entities[2]), "find: one destroyed");
  check(world->find_entities_by_tag("coin", found, 8U) == 2U &&
            (found[0] == entities[0]) && (found[1] == entities[3]),
        "find: a destroyed entity drops out");

  Entity one[1] = {};
  check(world->find_entities_by_tag("coin", one, 1U) == 2U &&
            (one[0] == entities[0]),
        "find: capped, it keeps the lowest index and counts them all");
  check(world->find_entities_by_tag("coin", nullptr, 0U) == 2U,
        "find: a count-only call counts");
}

void check_capacity() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "capacity: world");
  if (world == nullptr) {
    return;
  }
  const TagSetComponent tags = tags_of({"crowd"});
  bool allAdded = true;
  for (std::size_t i = 0U; i < World::kMaxTagSetComponents; ++i) {
    const Entity entity = world->create_entity();
    allAdded = allAdded && world->add_tag_set_component(entity, tags);
  }
  check(allAdded, "capacity: every set up to the capacity is added");
  const Entity extra = world->create_entity();
  check(!world->add_tag_set_component(extra, tags),
        "capacity: one past the capacity is refused");
  check(world->find_entities_by_tag("crowd", nullptr, 0U) ==
            World::kMaxTagSetComponents,
        "capacity: the stored sets are all found");
}

/// Saves `world` to a string through the production writer.
std::string save(const World &world) {
  std::vector<char> buffer(256U * 1024U);
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(world, buffer.data(), buffer.size(),
                                   &size)) {
    return {};
  }
  return std::string(buffer.data(), size);
}

void check_scene_round_trip() {
  std::unique_ptr<World> source = make_world();
  std::unique_ptr<World> loaded = make_world();
  check((source != nullptr) && (loaded != nullptr), "scene: worlds");
  if ((source == nullptr) || (loaded == nullptr)) {
    return;
  }
  // Scene objects: a load gives every entity a Transform, so only entities
  // that already have one save back byte for byte.
  const Entity plain = source->create_scene_object();
  const Entity tagged = source->create_scene_object();
  // 敌人 ("enemy"): a tag in the author's own script (#1185).
  const Entity enemy = source->create_scene_object();
  check((plain != kInvalidEntity) &&
            source->add_tag_set_component(tagged, tags_of({"coin", "Gold"})) &&
            source->add_tag_set_component(
                enemy, tags_of({"\xE6\x95\x8C\xE4\xBA\xBA"})),
        "scene: source built");
  const std::string text = save(*source);
  check(text.find("\"Tags\":[\"coin\",\"Gold\"]") != std::string::npos,
        "scene: tags are written as a string array in order");
  check(text.find("\"Tags\":[\"\xE6\x95\x8C\xE4\xBA\xBA\"]") !=
            std::string::npos,
        "scene: a CJK tag is written as its UTF-8 bytes");
  check(engine::runtime::load_scene(*loaded, text.data(), text.size()) &&
            (loaded->find_entities_by_tag("gold", nullptr, 0U) == 1U) &&
            (loaded->find_entities_by_tag("\xE6\x95\x8C\xE4\xBA\xBA", nullptr,
                                          0U) == 1U),
        "scene: tags load back, the CJK one included");
  check(save(*loaded) == text, "scene: a reload saves byte for byte the same");

  const char *const malformed[] = {
      "{\"version\":6,\"entities\":[{\"components\":{\"Tags\":\"coin\"}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"Tags\":[1]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"Tags\":"
      "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"Tags\":"
      "[\"coin\",\"COIN\"]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"Tags\":"
      "[\"has space\"]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"Tags\":"
      "[\"abcdefghijklmnopqrstuvwxyz012345\"]}}]}",
  };
  for (const char *document : malformed) {
    check(
        !engine::runtime::load_scene(*loaded, document, std::strlen(document)),
        "scene: a malformed tag array refuses the document");
    check(loaded->find_entities_by_tag("coin", nullptr, 0U) == 1U,
          "scene: a refused load keeps the previous world");
  }
}

} // namespace

int main() {
  check_set_rules();
  check_world_ingress();
  check_find_by_tag();
  check_capacity();
  check_scene_round_trip();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d tag check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
