// Verifies the Script section's property logic (#134): a property name is
// labelled as Unity labels a field; a value equal to the script's default
// clears the override, so only differences are stored; the scan cache
// reads a script once and again only after the file changes; an override
// edit staged through the Inspector is one undoable step that undo and
// redo replay exactly; and removing a behaviour from an entity's list is
// one undoable step that takes its overrides with it and gives both back.

#include "editor_commands.h"
#include "editor_script_behaviours.h"
#include "editor_script_properties.h"
#include "editor_session.h"
#include "engine/core/vfs.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>

namespace {

using engine::runtime::Entity;
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

std::string label_of(const char *name) {
  char label[64] = {};
  engine::editor::script_property_label(name, label, sizeof(label));
  return label;
}

void check_labels() {
  check(label_of("move_speed") == "Move Speed", "label: snake case");
  check(label_of("moveSpeed") == "Move Speed", "label: camel case");
  check(label_of("speed") == "Speed", "label: one word");
  check(label_of("_hidden") == "Hidden", "label: a leading underscore");
  check(label_of("jumpVY") == "Jump VY", "label: an upper-case run");
}

ScriptPropertyValue float_value(float value) {
  ScriptPropertyValue out{};
  out.type = ScriptPropertyType::Float;
  out.floatValue = value;
  return out;
}

void check_sparse_storage() {
  const ScriptPropertyValue defaultValue = float_value(0.25F);
  ScriptPropertiesComponent set{};
  ScriptPropertiesComponent edited{};
  check(engine::editor::script_properties_with_value(
            set, 0U, "speed", float_value(2.0F), defaultValue, &edited) &&
            (edited.count == 1U),
        "sparse: a value other than the default is stored");
  set = edited;
  check(engine::editor::script_properties_with_value(
            set, 0U, "speed", float_value(0.25F), defaultValue, &edited) &&
            (edited.count == 0U),
        "sparse: setting the default back clears the override");
}

bool write_text(const std::filesystem::path &path, const char *text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return out.good();
}

void check_schema_cache() {
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path root = fs::absolute("editor_script_properties_test_dir", ec);
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const fs::path script = root / "platform.lua";
  check(!ec && write_text(script, "M.properties = { speed = 1.0 }\n") &&
            engine::core::mount("proptest", root.string().c_str()),
        "cache: the script is written and mounted");
  const engine::scripting::ScriptPropertySchema *first =
      engine::editor::script_property_schema("proptest/platform.lua");
  check((first != nullptr) && (first->count == 1U),
        "cache: the script is scanned");
  check(engine::editor::script_property_schema("proptest/missing.lua") ==
            nullptr,
        "cache: a missing script has no scan");

  check(write_text(script, "M.properties = { speed = 1.0, lives = 3 }\n"),
        "cache: the script is edited");
  // A file time two seconds later, so the change shows on every file system
  // whatever its time resolution.
  fs::last_write_time(
      script, fs::last_write_time(script, ec) + std::chrono::seconds(2), ec);
  const engine::scripting::ScriptPropertySchema *second =
      engine::editor::script_property_schema("proptest/platform.lua");
  check((second != nullptr) && (second->count == 2U),
        "cache: an edited script is scanned again");
  static_cast<void>(engine::core::unmount("proptest"));
  engine::editor::reset_script_property_schemas();
  fs::remove_all(root, ec);
}

void check_undo() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  check(world != nullptr, "undo: world");
  if (world == nullptr) {
    return;
  }
  world->end_frame_phase();
  World *previous = engine::editor::editor_session().world;
  engine::editor::editor_session().world = world.get();
  auto &history = engine::editor::editor_session().commandHistory;
  const Entity entity = world->create_scene_object();

  ScriptPropertiesComponent before{};
  ScriptPropertiesComponent after{};
  static_cast<void>(engine::runtime::script_properties_set(&after, 0U, "speed",
                                                           float_value(3.0F)));
  engine::editor::ComponentEditSnapshot from{};
  from.scriptProperties = before;
  engine::editor::ComponentEditSnapshot to{};
  to.scriptProperties = after;
  check(engine::editor::inspector_stage_component_edit(
            entity, engine::editor::ComponentEditType::ScriptProperties, from,
            to),
        "undo: the edit is staged on an entity with no overrides yet");
  engine::editor::inspector_commit_pending_edit();
  const ScriptPropertiesComponent *stored =
      world->get_script_properties_ptr(entity);
  check((stored != nullptr) && (stored->count == 1U) &&
            (stored->overrides[0].value.floatValue == 3.0F),
        "undo: the edit applies");
  history.undo();
  stored = world->get_script_properties_ptr(entity);
  check((stored == nullptr) || (stored->count == 0U),
        "undo: undo leaves no override");
  history.redo();
  stored = world->get_script_properties_ptr(entity);
  check((stored != nullptr) && (stored->count == 1U),
        "undo: redo brings the override back");

  engine::editor::inspector_abandon_pending_edit();
  history.clear();
  engine::editor::editor_session().world = previous;
}

/// True when both lists name the same scripts with the same flags.
bool same_list(const engine::runtime::ScriptComponent &a,
               const engine::runtime::ScriptComponent &b) {
  for (std::size_t i = 0U; i < engine::runtime::kMaxScriptBehaviours; ++i) {
    if ((std::strcmp(a.behaviours[i].scriptPath, b.behaviours[i].scriptPath) !=
         0) ||
        (a.behaviours[i].enabled != b.behaviours[i].enabled)) {
      return false;
    }
  }
  return true;
}

/// True when both sets hold the same overrides in the same order.
bool same_overrides(const ScriptPropertiesComponent &a,
                    const ScriptPropertiesComponent &b) {
  if (a.count != b.count) {
    return false;
  }
  for (std::size_t i = 0U; i < a.count; ++i) {
    if ((a.overrides[i].behaviour != b.overrides[i].behaviour) ||
        (std::strcmp(a.overrides[i].name, b.overrides[i].name) != 0) ||
        !engine::math::script_property_values_equal(a.overrides[i].value,
                                                    b.overrides[i].value)) {
      return false;
    }
  }
  return true;
}

void check_behaviour_list_undo() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  check(world != nullptr, "list undo: world");
  if (world == nullptr) {
    return;
  }
  world->end_frame_phase();
  World *previous = engine::editor::editor_session().world;
  engine::editor::editor_session().world = world.get();
  auto &history = engine::editor::editor_session().commandHistory;
  const Entity entity = world->create_scene_object();

  engine::runtime::ScriptComponent list{};
  ScriptPropertiesComponent overrides{};
  check(engine::runtime::script_behaviour_append(&list, "a.lua") &&
            engine::runtime::script_behaviour_append(&list, "b.lua") &&
            engine::runtime::script_properties_set(&overrides, 0U, "speed",
                                                   float_value(3.0F)) &&
            engine::runtime::script_properties_set(&overrides, 1U, "speed",
                                                   float_value(7.0F)) &&
            world->add_script_component(entity, list) &&
            world->add_script_properties(entity, overrides),
        "list undo: an entity with two behaviours, each overridden");

  engine::runtime::ScriptComponent removed = list;
  ScriptPropertiesComponent removedOverrides = overrides;
  check(engine::runtime::script_behaviour_remove(&removed, &removedOverrides,
                                                 0U) &&
            engine::editor::execute_script_behaviours_edit(entity, removed,
                                                           removedOverrides),
        "list undo: removing the first behaviour runs as a command");
  const engine::runtime::ScriptComponent *storedList =
      world->get_script_component_ptr(entity);
  const ScriptPropertiesComponent *storedOverrides =
      world->get_script_properties_ptr(entity);
  check((storedList != nullptr) &&
            (engine::runtime::script_behaviour_count(*storedList) == 1U) &&
            (std::strcmp(storedList->behaviours[0].scriptPath, "b.lua") == 0) &&
            (storedOverrides != nullptr) && (storedOverrides->count == 1U) &&
            (storedOverrides->overrides[0].behaviour == 0U) &&
            (storedOverrides->overrides[0].value.floatValue == 7.0F),
        "list undo: the list loses the behaviour and the remaining "
        "override is renumbered with it");

  history.undo();
  storedList = world->get_script_component_ptr(entity);
  storedOverrides = world->get_script_properties_ptr(entity);
  check((storedList != nullptr) && same_list(*storedList, list) &&
            (storedOverrides != nullptr) &&
            same_overrides(*storedOverrides, overrides),
        "list undo: one undo restores the list and both overrides exactly");
  history.redo();
  storedList = world->get_script_component_ptr(entity);
  check((storedList != nullptr) &&
            (engine::runtime::script_behaviour_count(*storedList) == 1U),
        "list undo: redo removes it again");

  engine::runtime::ScriptComponent repeated = list;
  std::memcpy(repeated.behaviours[1].scriptPath, "a.lua", 6U);
  check(!engine::editor::execute_script_behaviours_edit(entity, repeated,
                                                        overrides),
        "list undo: a list naming one script twice is refused");

  history.clear();
  engine::editor::editor_session().world = previous;
}

/// An entity whose script has no overrides holds no overrides component.
/// Adding a behaviour to it is still one undoable command: observed on
/// 2026-10-05 in the editor, Add Behaviour changed the list but recorded
/// nothing, because the command tried to remove the absent component.
void check_behaviour_add_without_overrides() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  check(world != nullptr, "add without overrides: world");
  if (world == nullptr) {
    return;
  }
  world->end_frame_phase();
  World *previous = engine::editor::editor_session().world;
  engine::editor::editor_session().world = world.get();
  auto &history = engine::editor::editor_session().commandHistory;
  history.clear();
  const Entity entity = world->create_scene_object();

  engine::runtime::ScriptComponent list{};
  check(engine::runtime::script_behaviour_append(&list, "a.lua") &&
            world->add_script_component(entity, list) &&
            (world->get_script_properties_ptr(entity) == nullptr),
        "add without overrides: one behaviour and no overrides component");

  engine::runtime::ScriptComponent added = list;
  check(engine::runtime::script_behaviour_append(&added, "b.lua") &&
            engine::editor::execute_script_behaviours_edit(
                entity, added, ScriptPropertiesComponent{}),
        "add without overrides: adding a behaviour runs as a command");
  const engine::runtime::ScriptComponent *storedList =
      world->get_script_component_ptr(entity);
  check((storedList != nullptr) && same_list(*storedList, added) &&
            (world->get_script_properties_ptr(entity) == nullptr) &&
            history.can_undo(),
        "add without overrides: the list gains it, no overrides component "
        "appears, and the edit can be undone");

  check(history.undo(), "add without overrides: undo succeeds");
  storedList = world->get_script_component_ptr(entity);
  check((storedList != nullptr) && same_list(*storedList, list) &&
            (world->get_script_properties_ptr(entity) == nullptr),
        "add without overrides: one undo restores the one-behaviour list");
  check(history.redo(), "add without overrides: redo succeeds");
  storedList = world->get_script_component_ptr(entity);
  check((storedList != nullptr) && same_list(*storedList, added),
        "add without overrides: redo adds it again");

  history.clear();
  engine::editor::editor_session().world = previous;
}

} // namespace

/// Runs the Script section property suite.
int main() {
  check_labels();
  check_sparse_storage();
  check_schema_cache();
  check_undo();
  check_behaviour_list_undo();
  check_behaviour_add_without_overrides();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d editor script property check(s) failed\n",
                 g_failures);
    return 1;
  }
  std::printf("editor_script_properties_test: all checks passed\n");
  return 0;
}
