// Implements the Script section's behaviour list and the command that
// edits the list and its property overrides as one undo step.

#include "editor_script_behaviours.h"

#include <cstdio>
#include <cstring>

#include "editor_reference_pickers.h"
#include "editor_script_properties.h"
#include "editor_session.h"
#include "engine/core/logging.h"
#include "imgui.h"

namespace engine::editor {

namespace {

using runtime::ScriptComponent;
using runtime::ScriptPropertiesComponent;

const ImVec4 kProblemColor(1.0F, 0.6F, 0.3F, 1.0F);

// The path being chosen for a new behaviour; cleared once it is added.
char g_addPath[runtime::ScriptBehaviour::kMaxPathLength + 1U] = {};

/// The entity's current overrides, or an empty set.
ScriptPropertiesComponent current_properties(runtime::Entity entity) noexcept {
  ScriptPropertiesComponent properties{};
  runtime::World *world = editor_session().world;
  if ((world != nullptr) &&
      (world->get_script_properties_ptr(entity) != nullptr)) {
    static_cast<void>(world->get_script_properties(entity, &properties));
  }
  return properties;
}

/// Lists overrides that name a behaviour index past the list, which a file
/// or an older list left behind; they are kept until removed, like the
/// values a script stopped declaring.
void draw_orphaned_values(runtime::Entity entity, const ScriptComponent &script,
                          const ScriptPropertiesComponent &properties) noexcept {
  const std::size_t count = runtime::script_behaviour_count(script);
  bool any = false;
  for (std::size_t i = 0U; i < properties.count; ++i) {
    any = any || (properties.overrides[i].behaviour >= count);
  }
  if (!any) {
    return;
  }
  ImGui::TextColored(kProblemColor,
                     "Kept values for behaviours this entity no longer has:");
  for (std::size_t i = 0U; i < properties.count; ++i) {
    const ScriptPropertiesComponent::Override &entry = properties.overrides[i];
    if (entry.behaviour < count) {
      continue;
    }
    ImGui::PushID(static_cast<int>(i));
    ImGui::BulletText("behaviour %u: %s", static_cast<unsigned>(entry.behaviour),
                      entry.name);
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      ScriptPropertiesComponent edited = properties;
      runtime::script_properties_erase(&edited, i);
      static_cast<void>(execute_script_behaviours_edit(entity, script, edited));
      ImGui::PopID();
      return;
    }
    ImGui::PopID();
  }
}

} // namespace

bool ScriptBehavioursCommand::apply_state(
    bool scriptExists, bool propertiesExist,
    const ComponentEditSnapshot &snapshot) noexcept {
  const runtime::Entity target = resolve_command_target(entity, persistentId);
  return apply_component_snapshot(ComponentEditType::Script, target,
                                  scriptExists, snapshot) &&
         apply_component_snapshot(ComponentEditType::ScriptProperties, target,
                                  propertiesExist, snapshot);
}

bool execute_script_behaviours_edit(
    runtime::Entity entity, const ScriptComponent &script,
    const ScriptPropertiesComponent &properties) noexcept {
  runtime::World *world = editor_session().world;
  if ((world == nullptr) || !runtime::script_component_is_valid(script) ||
      !runtime::script_properties_are_valid(properties)) {
    return false;
  }
  inspector_commit_pending_edit();
  auto *cmd = allocate_command<ScriptBehavioursCommand>();
  if (cmd == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "behaviour edit refused: it could not be recorded for "
                      "undo (out of memory)");
    return false;
  }
  cmd->entity = entity;
  cmd->persistentId = world->persistent_id(entity);
  cmd->scriptBeforeExists =
      capture_component_snapshot(ComponentEditType::Script, entity,
                                 &cmd->before);
  cmd->propertiesBeforeExists = capture_component_snapshot(
      ComponentEditType::ScriptProperties, entity, &cmd->before);
  cmd->propertiesAfterExists =
      cmd->propertiesBeforeExists || (properties.count > 0U);
  cmd->after = cmd->before;
  cmd->after.script = script;
  cmd->after.scriptProperties = properties;
  return editor_session().commandHistory.execute(cmd);
}

bool draw_script_behaviour_list(runtime::Entity entity,
                                ScriptComponent &script) noexcept {
  const std::size_t count = runtime::script_behaviour_count(script);
  const ScriptPropertiesComponent properties = current_properties(entity);
  bool modified = false;
  for (std::size_t b = 0U; b < count; ++b) {
    runtime::ScriptBehaviour &behaviour = script.behaviours[b];
    ImGui::PushID(static_cast<int>(b));
    if (b > 0U) {
      ImGui::Separator();
    }
    modified = ImGui::Checkbox("##Enabled", &behaviour.enabled) || modified;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Enabled: a disabled behaviour gets no callback "
                        "until it is enabled again");
    }
    ImGui::SameLine();
    char path[sizeof(behaviour.scriptPath)] = {};
    std::memcpy(path, behaviour.scriptPath, sizeof(path));
    if (draw_path_reference_picker("Script", path, sizeof(path), ".lua")) {
      ScriptComponent candidate = script;
      std::memcpy(candidate.behaviours[b].scriptPath, path, sizeof(path));
      if ((path[0] != '\0') && runtime::script_component_is_valid(candidate)) {
        std::memcpy(behaviour.scriptPath, path, sizeof(path));
        modified = true;
      } else {
        core::log_message(core::LogLevel::Warning, "editor",
                          "behaviour script not changed: the entity already "
                          "runs that script, or none was chosen (use Remove)");
      }
    }
    bool listChanged = false;
    ScriptComponent edited = script;
    ScriptPropertiesComponent editedProperties = properties;
    ImGui::BeginDisabled(b == 0U);
    if (ImGui::SmallButton("Up")) {
      listChanged = runtime::script_behaviour_swap(&edited, &editedProperties,
                                                   b, b - 1U);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(b + 1U >= count);
    if (ImGui::SmallButton("Down")) {
      listChanged = runtime::script_behaviour_swap(&edited, &editedProperties,
                                                   b, b + 1U);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      listChanged =
          runtime::script_behaviour_remove(&edited, &editedProperties, b);
    }
    if (listChanged) {
      // The list on screen is stale from here; the next frame draws the
      // edited one.
      static_cast<void>(
          execute_script_behaviours_edit(entity, edited, editedProperties));
      ImGui::PopID();
      return false;
    }
    draw_script_property_fields(entity, b, behaviour.scriptPath);
    ImGui::PopID();
  }
  draw_orphaned_values(entity, script, properties);

  ImGui::Separator();
  if (count < runtime::kMaxScriptBehaviours) {
    if (draw_path_reference_picker("Add Behaviour", g_addPath,
                                   sizeof(g_addPath), ".lua") &&
        (g_addPath[0] != '\0')) {
      ScriptComponent edited = script;
      if (runtime::script_behaviour_append(&edited, g_addPath)) {
        static_cast<void>(
            execute_script_behaviours_edit(entity, edited, properties));
      } else {
        core::log_message(core::LogLevel::Warning, "editor",
                          "behaviour not added: the entity already runs that "
                          "script");
      }
      g_addPath[0] = '\0';
      return false;
    }
  } else {
    ImGui::TextDisabled("An entity runs at most %u behaviours.",
                        static_cast<unsigned>(runtime::kMaxScriptBehaviours));
  }
  return modified;
}

} // namespace engine::editor
