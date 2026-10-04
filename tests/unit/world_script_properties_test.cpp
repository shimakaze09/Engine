// Verifies script property overrides (ScriptPropertiesComponent, #134): the
// World refuses a set with an invalid name, a repeated name, more than
// kMaxOverrides entries or an invalid value; set and clear keep order and
// distinct names; the capacity refuses one past it; and scenes round-trip
// every value type, keeping a float with an integral value a float, write
// nothing for an empty set, and refuse a malformed override object with
// the previous world kept.

#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::ScriptPropertiesComponent;
using engine::runtime::ScriptPropertyType;
using engine::runtime::ScriptPropertyValue;
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

ScriptPropertyValue float_value(float value) {
  ScriptPropertyValue out{};
  out.type = ScriptPropertyType::Float;
  out.floatValue = value;
  return out;
}

ScriptPropertyValue integer_value(std::int64_t value) {
  ScriptPropertyValue out{};
  out.type = ScriptPropertyType::Integer;
  out.integerValue = value;
  return out;
}

void check_set_rules() {
  ScriptPropertiesComponent set{};
  check(engine::runtime::script_properties_set(&set, 0U, "speed",
                                               float_value(2.0F)) &&
            engine::runtime::script_properties_set(&set, 0U, "lives",
                                                   integer_value(3)) &&
            engine::runtime::script_properties_set(&set, 0U, "speed",
                                                   float_value(4.0F)) &&
            (set.count == 2U) &&
            (std::strcmp(set.overrides[0].name, "speed") == 0) &&
            (set.overrides[0].value.floatValue == 4.0F),
        "rules: setting a name again replaces it in place");
  check(!engine::runtime::script_properties_set(&set, 0U, "9lives",
                                                integer_value(1)) &&
            !engine::runtime::script_properties_set(&set, 0U, "has space",
                                                    integer_value(1)) &&
            !engine::runtime::script_properties_set(
                &set, 0U, "a_name_of_thirty_two_characters_",
                integer_value(1)) &&
            (set.count == 2U),
        "rules: a name that is not a Lua identifier of at most 31 bytes is "
        "refused");
  check(!engine::runtime::script_properties_set(
            &set, 0U, "bad",
            float_value(std::numeric_limits<float>::infinity())),
        "rules: a non-finite float is refused");
  ScriptPropertyValue mixed = integer_value(1);
  mixed.floatValue = 1.0F;
  check(!engine::runtime::script_properties_set(&set, 0U, "mixed", mixed),
        "rules: a value with a member its type does not use is refused");
  check(engine::runtime::script_properties_clear(&set, 0U, "speed") &&
            (set.count == 1U) &&
            (std::strcmp(set.overrides[0].name, "lives") == 0) &&
            !engine::runtime::script_properties_clear(&set, 0U, "speed"),
        "rules: clearing removes one override and keeps the rest in order");
  ScriptPropertiesComponent full{};
  bool allSet = true;
  for (std::size_t i = 0U; i < ScriptPropertiesComponent::kMaxOverrides; ++i) {
    char name[8] = {};
    std::snprintf(name, sizeof(name), "p%zu", i);
    allSet = allSet && engine::runtime::script_properties_set(&full, 0U, name,
                                                              integer_value(1));
  }
  check(allSet && !engine::runtime::script_properties_set(&full, 0U, "extra",
                                                          integer_value(1)),
        "rules: one past kMaxOverrides is refused");
}

void check_world_ingress() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "ingress: world");
  if (world == nullptr) {
    return;
  }
  const Entity entity = world->create_scene_object();
  ScriptPropertiesComponent duplicate{};
  duplicate.count = 2U;
  std::snprintf(duplicate.overrides[0].name,
                sizeof(duplicate.overrides[0].name), "%s", "speed");
  std::snprintf(duplicate.overrides[1].name,
                sizeof(duplicate.overrides[1].name), "%s", "speed");
  check(!world->add_script_properties(entity, duplicate) &&
            (world->get_script_properties_ptr(entity) == nullptr),
        "ingress: a repeated name is refused whole");
  ScriptPropertiesComponent valid{};
  static_cast<void>(engine::runtime::script_properties_set(&valid, 0U, "speed",
                                                           float_value(1.5F)));
  check(world->add_script_properties(entity, valid) &&
            (world->get_script_properties_ptr(entity) != nullptr) &&
            (world->get_script_properties_ptr(entity)->count == 1U),
        "ingress: a valid set is stored");
}

void check_capacity() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "capacity: world");
  if (world == nullptr) {
    return;
  }
  ScriptPropertiesComponent set{};
  static_cast<void>(
      engine::runtime::script_properties_set(&set, 0U, "speed", float_value(1.0F)));
  bool allAdded = true;
  for (std::size_t i = 0U; i < World::kMaxScriptPropertiesComponents; ++i) {
    allAdded =
        allAdded && world->add_script_properties(world->create_entity(), set);
  }
  check(allAdded, "capacity: every set up to the capacity is added");
  check(!world->add_script_properties(world->create_entity(), set),
        "capacity: one past the capacity is refused");
}

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
  const Entity plain = source->create_scene_object();
  const Entity emptySet = source->create_scene_object();
  const Entity tuned = source->create_scene_object();
  ScriptPropertiesComponent set{};
  ScriptPropertyValue flag{};
  flag.type = ScriptPropertyType::Bool;
  flag.boolValue = true;
  ScriptPropertyValue name{};
  name.type = ScriptPropertyType::String;
  std::snprintf(name.text, sizeof(name.text), "%s", "Main Camera");
  check(engine::runtime::script_properties_set(&set, 0U, "speed",
                                               float_value(3.0F)) &&
            engine::runtime::script_properties_set(&set, 0U, "drop",
                                                   float_value(-0.15F)) &&
            engine::runtime::script_properties_set(&set, 0U, "lives",
                                                   integer_value(3)) &&
            engine::runtime::script_properties_set(&set, 0U, "on", flag) &&
            engine::runtime::script_properties_set(&set, 0U, "camera", name),
        "scene: override set built");
  check((plain != kInvalidEntity) &&
            source->add_script_properties(emptySet,
                                          ScriptPropertiesComponent{}) &&
            source->add_script_properties(tuned, set),
        "scene: source built");
  const std::string text = save(*source);
  check(text.find("\"ScriptProperties\":{\"speed\":3.0,\"drop\":-0.15,"
                  "\"lives\":3,\"on\":true,\"camera\":\"Main Camera\"}") !=
            std::string::npos,
        "scene: overrides are written in order, a float always with a '.'");
  check(text.find("\"ScriptProperties\":{}") == std::string::npos,
        "scene: an empty set writes nothing");
  check(engine::runtime::load_scene(*loaded, text.data(), text.size()),
        "scene: the document loads");
  const std::string reloaded = save(*loaded);
  check(reloaded == text, "scene: a reload saves byte for byte the same");
  check(reloaded.find("\"speed\":3.0") != std::string::npos,
        "scene: a float with an integral value stays a float");

  const char *const malformed[] = {
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "[1]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "{\"a b\":1}}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "{\"a\":1,\"a\":2}}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "{\"a\":null}}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "{\"a\":[1]}}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "{\"a\":1e40}}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "{\"a\":\"012345678901234567890123456789012345678901234567\"}}}]}",
  };
  for (const char *document : malformed) {
    check(
        !engine::runtime::load_scene(*loaded, document, std::strlen(document)),
        "scene: a malformed override object refuses the document");
    check(save(*loaded) == text,
          "scene: a refused load keeps the previous world");
  }
}

} // namespace

/// Runs the script property override suite.
int main() {
  check_set_rules();
  check_world_ingress();
  check_capacity();
  check_scene_round_trip();
  if (g_failures != 0) {
    std::fprintf(stderr, "world_script_properties_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("world_script_properties_test: all checks passed\n");
  return 0;
}
