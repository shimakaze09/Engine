// Verifies an entity's behaviour list (ScriptComponent, #134): the list
// helpers keep it compact and free of repeats and carry each property
// override with its behaviour through a removal or a reorder; the World
// refuses a list with a gap, a repeated script or an unterminated path;
// and scenes write one enabled behaviour as the bare path the format has
// always used, write any other list as an array with disabled entries
// marked, round-trip per-behaviour overrides byte for byte, and refuse a
// malformed list with the previous world kept. A prefab carries the list
// and its overrides through save and instantiate.

#include "engine/runtime/prefab_serializer.h"
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
using engine::runtime::kMaxScriptBehaviours;
using engine::runtime::ScriptComponent;
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

/// The list holding `paths`, all enabled.
ScriptComponent list_of(std::initializer_list<const char *> paths) {
  ScriptComponent list{};
  for (const char *path : paths) {
    static_cast<void>(engine::runtime::script_behaviour_append(&list, path));
  }
  return list;
}

/// The value of behaviour `behaviour`'s override of `name`, or -1.
float override_of(const ScriptPropertiesComponent &set, std::size_t behaviour,
                  const char *name) {
  const ScriptPropertiesComponent::Override *entry =
      engine::runtime::script_property_override(set, behaviour, name);
  return (entry != nullptr) ? entry->value.floatValue : -1.0F;
}

void check_list_rules() {
  ScriptComponent list{};
  check(engine::runtime::script_behaviour_count(list) == 0U,
        "list: a new component lists no behaviour");
  check(engine::runtime::script_behaviour_append(&list, "a.lua") &&
            engine::runtime::script_behaviour_append(&list, "b.lua") &&
            (engine::runtime::script_behaviour_count(list) == 2U) &&
            list.behaviours[0].enabled && list.behaviours[1].enabled,
        "list: appended behaviours are listed in order, enabled");
  check(!engine::runtime::script_behaviour_append(&list, "a.lua") &&
            !engine::runtime::script_behaviour_append(&list, "") &&
            (engine::runtime::script_behaviour_count(list) == 2U),
        "list: a repeated script and an empty path are refused");
  std::string longPath(ScriptComponent::kMaxPathLength + 1U, 'p');
  check(!engine::runtime::script_behaviour_append(&list, longPath.c_str()),
        "list: a path one past the field is refused, never cut");
  longPath.pop_back();
  check(engine::runtime::script_behaviour_append(&list, longPath.c_str()),
        "list: a path that fills the field exactly is kept");
  check(engine::runtime::find_script_behaviour(list, "b.lua") == 1U &&
            engine::runtime::find_script_behaviour(list, "x.lua") ==
                kMaxScriptBehaviours,
        "list: a script is found by its path");

  ScriptComponent full{};
  bool filled = true;
  for (std::size_t i = 0U; i < kMaxScriptBehaviours; ++i) {
    char path[16] = {};
    std::snprintf(path, sizeof(path), "s%zu.lua", i);
    filled = filled && engine::runtime::script_behaviour_append(&full, path);
  }
  check(filled && !engine::runtime::script_behaviour_append(&full, "x.lua"),
        "list: the list takes kMaxScriptBehaviours and refuses one more");
  check(engine::runtime::script_component_is_valid(full),
        "list: a full list is valid");

  ScriptComponent gap = list_of({"a.lua", "b.lua"});
  gap.behaviours[0].scriptPath[0] = '\0';
  check(!engine::runtime::script_component_is_valid(gap),
        "rules: a list with a gap is invalid");
  ScriptComponent repeated = list_of({"a.lua", "b.lua"});
  std::memcpy(repeated.behaviours[1].scriptPath, "a.lua", 6U);
  check(!engine::runtime::script_component_is_valid(repeated),
        "rules: a list naming one script twice is invalid");
  ScriptComponent unterminated = list_of({"a.lua"});
  std::memset(unterminated.behaviours[0].scriptPath, 'q',
              sizeof(unterminated.behaviours[0].scriptPath));
  check(!engine::runtime::script_component_is_valid(unterminated),
        "rules: an unterminated path is invalid");
  ScriptComponent disabledPastEnd = list_of({"a.lua"});
  disabledPastEnd.behaviours[3].enabled = false;
  check(!engine::runtime::script_component_is_valid(disabledPastEnd),
        "rules: an entry past the list must be empty and enabled");
}

void check_overrides_follow_their_behaviour() {
  ScriptComponent list = list_of({"a.lua", "b.lua", "c.lua"});
  ScriptPropertiesComponent set{};
  check(engine::runtime::script_properties_set(&set, 0U, "speed",
                                               float_value(1.0F)) &&
            engine::runtime::script_properties_set(&set, 1U, "speed",
                                                   float_value(2.0F)) &&
            engine::runtime::script_properties_set(&set, 2U, "speed",
                                                   float_value(3.0F)),
        "overrides: one name may be set per behaviour");
  check(!engine::runtime::script_properties_set(
            &set, kMaxScriptBehaviours, "speed", float_value(9.0F)),
        "overrides: a behaviour index past the list's capacity is refused");

  check(engine::runtime::script_behaviour_swap(&list, &set, 0U, 2U) &&
            (std::strcmp(list.behaviours[0].scriptPath, "c.lua") == 0) &&
            (override_of(set, 0U, "speed") == 3.0F) &&
            (override_of(set, 2U, "speed") == 1.0F) &&
            (override_of(set, 1U, "speed") == 2.0F),
        "reorder: each override moves with its script");
  check(!engine::runtime::script_behaviour_swap(&list, &set, 0U, 3U),
        "reorder: an index past the list is refused");

  check(engine::runtime::script_behaviour_remove(&list, &set, 1U) &&
            (engine::runtime::script_behaviour_count(list) == 2U) &&
            (std::strcmp(list.behaviours[1].scriptPath, "a.lua") == 0) &&
            (set.count == 2U) && (override_of(set, 0U, "speed") == 3.0F) &&
            (override_of(set, 1U, "speed") == 1.0F),
        "remove: the removed behaviour's overrides go and later ones are "
        "renumbered");
  check(engine::runtime::script_component_is_valid(list),
        "remove: the list stays compact");
  check(!engine::runtime::script_behaviour_remove(&list, &set, 2U),
        "remove: an index past the list is refused");
}

void check_world_ingress() {
  std::unique_ptr<World> world = make_world();
  check(world != nullptr, "ingress: world");
  if (world == nullptr) {
    return;
  }
  const Entity entity = world->create_entity();
  ScriptComponent repeated = list_of({"a.lua", "b.lua"});
  std::memcpy(repeated.behaviours[1].scriptPath, "a.lua", 6U);
  check(!world->add_script_component(entity, repeated) &&
            (world->get_script_component_ptr(entity) == nullptr),
        "ingress: a list naming one script twice is refused whole");
  ScriptComponent gap = list_of({"a.lua", "b.lua"});
  gap.behaviours[0].scriptPath[0] = '\0';
  check(!world->add_script_component(entity, gap),
        "ingress: a list with a gap is refused");
  ScriptComponent list = list_of({"a.lua", "b.lua"});
  list.behaviours[1].enabled = false;
  check(world->add_script_component(entity, list),
        "ingress: a valid list is stored");
  const ScriptComponent *stored = world->get_script_component_ptr(entity);
  check((stored != nullptr) &&
            (engine::runtime::script_behaviour_count(*stored) == 2U) &&
            !stored->behaviours[1].enabled,
        "ingress: the stored list keeps its order and flags");
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
  const Entity single = source->create_scene_object();
  const Entity several = source->create_scene_object();
  ScriptComponent multi = list_of({"a.lua", "b.lua", "c.lua"});
  multi.behaviours[1].enabled = false;
  ScriptPropertiesComponent firstOnly{};
  ScriptPropertiesComponent spread{};
  check(engine::runtime::script_properties_set(&firstOnly, 0U, "speed",
                                               float_value(3.0F)) &&
            engine::runtime::script_properties_set(&spread, 0U, "speed",
                                                   float_value(3.0F)) &&
            engine::runtime::script_properties_set(&spread, 2U, "speed",
                                                   float_value(7.5F)),
        "scene: override sets built");
  check(source->add_script_component(single, list_of({"a.lua"})) &&
            source->add_script_properties(single, firstOnly) &&
            source->add_script_component(several, multi) &&
            source->add_script_properties(several, spread),
        "scene: source built");
  const std::string text = save(*source);
  check(text.find("\"ScriptComponent\":\"a.lua\"") != std::string::npos,
        "scene: one enabled behaviour writes the bare path");
  check(text.find("\"ScriptProperties\":{\"speed\":3.0}") != std::string::npos,
        "scene: the first behaviour's overrides alone write one object");
  check(text.find("\"ScriptComponent\":[\"a.lua\",{\"scriptPath\":\"b.lua\","
                  "\"enabled\":false},\"c.lua\"]") != std::string::npos,
        "scene: a list writes an array, a disabled entry marked");
  check(text.find("\"ScriptProperties\":[{\"speed\":3.0},{},{\"speed\":7.5}]") !=
            std::string::npos,
        "scene: overrides past the first behaviour write one object per "
        "behaviour");
  check(engine::runtime::load_scene(*loaded, text.data(), text.size()),
        "scene: the document loads");
  check(save(*loaded) == text, "scene: a reload saves byte for byte the same");

  const char *const malformed[] = {
      // An empty array, an empty path, a repeated script, an unknown
      // member, a non-bool flag, a number, and nine behaviours.
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "[]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "[\"a.lua\",\"\"]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "[\"a.lua\",{\"scriptPath\":\"a.lua\"}]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "[{\"scriptPath\":\"a.lua\",\"order\":1}]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "[{\"scriptPath\":\"a.lua\",\"enabled\":0}]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "7}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptComponent\":"
      "[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}}]}",
      // Overrides: nine behaviour objects, and a non-object entry.
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "[{},{},{},{},{},{},{},{},{}]}}]}",
      "{\"version\":6,\"entities\":[{\"components\":{\"ScriptProperties\":"
      "[{},1]}}]}",
  };
  for (const char *document : malformed) {
    check(
        !engine::runtime::load_scene(*loaded, document, std::strlen(document)),
        "scene: a malformed behaviour list or override array refuses the "
        "document");
    check(save(*loaded) == text,
          "scene: a refused load keeps the previous world");
  }
}

void check_prefab_round_trip() {
  constexpr const char *kPrefabPath = "world_script_behaviours_test.prefab";
  std::unique_ptr<World> source = make_world();
  std::unique_ptr<World> target = make_world();
  check((source != nullptr) && (target != nullptr), "prefab: worlds");
  if ((source == nullptr) || (target == nullptr)) {
    return;
  }
  const Entity root = source->create_scene_object();
  ScriptComponent multi = list_of({"a.lua", "b.lua"});
  multi.behaviours[0].enabled = false;
  ScriptPropertiesComponent spread{};
  check(engine::runtime::script_properties_set(&spread, 1U, "speed",
                                               float_value(4.0F)) &&
            source->add_script_component(root, multi) &&
            source->add_script_properties(root, spread) &&
            engine::runtime::save_prefab(*source, root, kPrefabPath),
        "prefab: saved");
  const Entity instance =
      engine::runtime::instantiate_prefab(*target, kPrefabPath);
  const ScriptComponent *list = (instance != kInvalidEntity)
                                    ? target->get_script_component_ptr(instance)
                                    : nullptr;
  const ScriptPropertiesComponent *overrides =
      (instance != kInvalidEntity) ? target->get_script_properties_ptr(instance)
                                   : nullptr;
  check((list != nullptr) &&
            (engine::runtime::script_behaviour_count(*list) == 2U) &&
            !list->behaviours[0].enabled && list->behaviours[1].enabled &&
            (std::strcmp(list->behaviours[1].scriptPath, "b.lua") == 0),
        "prefab: the instance runs the same list with the same flags");
  check((overrides != nullptr) && (override_of(*overrides, 1U, "speed") == 4.0F),
        "prefab: the second behaviour's override survives");
  static_cast<void>(std::remove(kPrefabPath));
}

} // namespace

/// Runs the behaviour list suite.
int main() {
  check_list_rules();
  check_overrides_follow_their_behaviour();
  check_world_ingress();
  check_scene_round_trip();
  check_prefab_round_trip();
  if (g_failures != 0) {
    std::fprintf(stderr, "world_script_behaviours_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("world_script_behaviours_test: all checks passed\n");
  return 0;
}
