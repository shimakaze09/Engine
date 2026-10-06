// Implements the Script section's property fields: a small cache of
// script scans keyed by path and file time, the typed field for each
// declared property, and the staging of each change as an undoable edit
// of the entity's override set.

#include "editor_script_properties.h"

#include "editor_commands.h"
#include "editor_inspector_widgets.h"
#include "editor_session.h"

#include "imgui.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "engine/core/vfs.h"

namespace engine::editor {

namespace {

using runtime::ScriptPropertiesComponent;
using runtime::ScriptPropertyType;
using runtime::ScriptPropertyValue;
using scripting::ScriptPropertyDecl;
using scripting::ScriptPropertySchema;

/// Scripts whose scans are kept at once; the Inspector shows one entity's
/// script at a time, so a few cover switching between selections.
constexpr std::size_t kSchemaCacheSize = 8U;
/// Largest script the editor scans; a larger file is not one a person
/// edits by hand.
constexpr std::uint64_t kMaxScannedScriptBytes = 4U * 1024U * 1024U;

struct SchemaCacheEntry final {
  char path[runtime::ScriptComponent::kMaxPathLength + 1U] = {};
  std::int64_t mtime = 0;
  bool readable = false;
  bool used = false;
  std::uint64_t lastUse = 0U;
  ScriptPropertySchema schema{};
};

SchemaCacheEntry g_schemas[kSchemaCacheSize] = {};
std::uint64_t g_useClock = 0U;

const ImVec4 kOverriddenColor(0.45F, 0.75F, 1.0F, 1.0F);
const ImVec4 kProblemColor(1.0F, 0.6F, 0.3F, 1.0F);

/// Reads and scans the script into `entry`.
void scan_into(SchemaCacheEntry &entry, const char *path,
               std::int64_t mtime) noexcept {
  entry.mtime = mtime;
  entry.readable = false;
  entry.schema = ScriptPropertySchema{};
  void *data = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_binary_bounded(path, kMaxScannedScriptBytes, &data,
                                     &size)) {
    return;
  }
  scripting::scan_script_properties(static_cast<const char *>(data), size,
                                    &entry.schema);
  core::vfs_free(data);
  entry.readable = true;
}

/// Writes a value as the Inspector shows it in text.
void format_value(const ScriptPropertyValue &value, char *out,
                  std::size_t capacity) noexcept {
  switch (value.type) {
  case ScriptPropertyType::Bool:
    std::snprintf(out, capacity, "%s", value.boolValue ? "true" : "false");
    return;
  case ScriptPropertyType::Integer:
    std::snprintf(out, capacity, "%" PRId64, value.integerValue);
    return;
  case ScriptPropertyType::Float:
    std::snprintf(out, capacity, "%g", static_cast<double>(value.floatValue));
    return;
  case ScriptPropertyType::String:
    std::snprintf(out, capacity, "\"%s\"", value.text);
    return;
  }
  std::snprintf(out, capacity, "?");
}

/// Stages `after` as the entity's override set, one undoable edit.
void stage_overrides(runtime::Entity entity,
                     const ScriptPropertiesComponent &before,
                     const ScriptPropertiesComponent &after) noexcept {
  ComponentEditSnapshot from{};
  from.scriptProperties = before;
  ComponentEditSnapshot to{};
  to.scriptProperties = after;
  static_cast<void>(inspector_stage_component_edit(
      entity, ComponentEditType::ScriptProperties, from, to));
}

/// Draws the typed field for one declared property; true, with `*value`
/// updated, when the author changed it.
bool draw_field(const char *label, const ScriptPropertyDecl &decl,
                ScriptPropertyValue *value) noexcept {
  switch (decl.defaultValue.type) {
  case ScriptPropertyType::Bool:
    return ImGui::Checkbox(label, &value->boolValue);
  case ScriptPropertyType::Integer: {
    const std::int64_t minValue =
        decl.hasMin ? static_cast<std::int64_t>(decl.minValue) : 0;
    const std::int64_t maxValue =
        decl.hasMax ? static_cast<std::int64_t>(decl.maxValue) : 0;
    const bool ranged = decl.hasMin && decl.hasMax;
    std::int64_t edited = value->integerValue;
    if (!ImGui::DragScalar(label, ImGuiDataType_S64, &edited, 0.25F,
                           ranged ? &minValue : nullptr,
                           ranged ? &maxValue : nullptr)) {
      return false;
    }
    if (decl.hasMin && (edited < minValue)) {
      edited = minValue;
    }
    if (decl.hasMax && (edited > maxValue)) {
      edited = maxValue;
    }
    value->integerValue = edited;
    return true;
  }
  case ScriptPropertyType::Float: {
    const float minValue =
        decl.hasMin ? static_cast<float>(decl.minValue) : 0.0F;
    const float maxValue =
        decl.hasMax ? static_cast<float>(decl.maxValue) : 0.0F;
    const bool ranged = decl.hasMin && decl.hasMax;
    float edited = value->floatValue;
    if (!inspector_drag_float(label, &edited, 0.01F, ranged ? minValue : 0.0F,
                              ranged ? maxValue : 0.0F)) {
      return false;
    }
    if (decl.hasMin && (edited < minValue)) {
      edited = minValue;
    }
    if (decl.hasMax && (edited > maxValue)) {
      edited = maxValue;
    }
    value->floatValue = edited;
    return true;
  }
  case ScriptPropertyType::String: {
    // ImGui keeps the text being typed while the field is active; the
    // value is taken once, when the edit ends.
    char text[ScriptPropertyValue::kMaxTextLength + 1U] = {};
    std::memcpy(text, value->text, sizeof(text));
    ImGui::InputText(label, text, sizeof(text));
    if (!ImGui::IsItemDeactivatedAfterEdit() ||
        (std::strcmp(text, value->text) == 0)) {
      return false;
    }
    std::memcpy(value->text, text, sizeof(value->text));
    return true;
  }
  }
  return false;
}

/// Lists the values the entity keeps for properties its script does not
/// declare (renamed, removed, or a script that does not read), each with
/// Remove; they are never dropped silently.
void draw_kept_values(runtime::Entity entity, std::size_t behaviour,
                      const ScriptPropertiesComponent &overrides,
                      const ScriptPropertySchema *schema) noexcept {
  bool header = false;
  for (std::size_t i = 0U; i < overrides.count; ++i) {
    const ScriptPropertiesComponent::Override &entry = overrides.overrides[i];
    if ((entry.behaviour != behaviour) ||
        ((schema != nullptr) &&
         (scripting::find_script_property(*schema, entry.name) != nullptr))) {
      continue;
    }
    if (!header) {
      ImGui::TextColored(kProblemColor,
                         "Kept values the script does not declare:");
      header = true;
    }
    char text[ScriptPropertyValue::kMaxTextLength + 8U] = {};
    format_value(entry.value, text, sizeof(text));
    ImGui::PushID(entry.name);
    ImGui::BulletText("%s = %s", entry.name, text);
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      ScriptPropertiesComponent edited = overrides;
      static_cast<void>(
          runtime::script_properties_clear(&edited, behaviour, entry.name));
      stage_overrides(entity, overrides, edited);
      inspector_commit_pending_edit();
      ImGui::PopID();
      return;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Removes this entity's value. It is kept until then "
                        "so a renamed or removed property loses nothing.");
    }
    ImGui::PopID();
  }
}

} // namespace

const ScriptPropertySchema *
script_property_schema(const char *scriptPath) noexcept {
  if ((scriptPath == nullptr) || (scriptPath[0] == '\0')) {
    return nullptr;
  }
  const std::int64_t mtime = core::vfs_file_mtime(scriptPath);
  ++g_useClock;
  SchemaCacheEntry *slot = nullptr;
  for (SchemaCacheEntry &entry : g_schemas) {
    if (entry.used && (std::strcmp(entry.path, scriptPath) == 0)) {
      slot = &entry;
      break;
    }
  }
  if (slot == nullptr) {
    // The least recently used entry makes room.
    slot = &g_schemas[0];
    for (SchemaCacheEntry &entry : g_schemas) {
      if (!entry.used) {
        slot = &entry;
        break;
      }
      if (entry.lastUse < slot->lastUse) {
        slot = &entry;
      }
    }
    slot->used = true;
    std::snprintf(slot->path, sizeof(slot->path), "%s", scriptPath);
    scan_into(*slot, scriptPath, mtime);
  } else if (slot->mtime != mtime) {
    scan_into(*slot, scriptPath, mtime);
  }
  slot->lastUse = g_useClock;
  return slot->readable ? &slot->schema : nullptr;
}

void script_property_label(const char *name, char *out,
                           std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return;
  }
  std::size_t written = 0U;
  bool startWord = true;
  char previous = '\0';
  for (const char *c = name; (c != nullptr) && (*c != '\0'); ++c) {
    if (*c == '_') {
      startWord = true;
      previous = *c;
      continue;
    }
    const bool upper = (*c >= 'A') && (*c <= 'Z');
    const bool lowerBefore = (previous >= 'a') && (previous <= 'z');
    if ((upper && lowerBefore) || (startWord && (written > 0U))) {
      if (written + 1U < capacity) {
        out[written++] = ' ';
      }
    }
    char shown = *c;
    if ((startWord || (upper && lowerBefore)) && (shown >= 'a') &&
        (shown <= 'z')) {
      shown = static_cast<char>(shown - 'a' + 'A');
    }
    if (written + 1U < capacity) {
      out[written++] = shown;
    }
    startWord = false;
    previous = *c;
  }
  out[written] = '\0';
}

bool script_properties_with_value(const ScriptPropertiesComponent &current,
                                  std::size_t behaviour, const char *name,
                                  const ScriptPropertyValue &value,
                                  const ScriptPropertyValue &defaultValue,
                                  ScriptPropertiesComponent *out) noexcept {
  ScriptPropertiesComponent edited = current;
  if (math::script_property_values_equal(value, defaultValue)) {
    static_cast<void>(
        runtime::script_properties_clear(&edited, behaviour, name));
  } else if (!runtime::script_properties_set(&edited, behaviour, name,
                                             value)) {
    return false;
  }
  *out = edited;
  return true;
}

void draw_script_property_fields(runtime::Entity entity, std::size_t behaviour,
                                 const char *scriptPath) noexcept {
  runtime::World *world = editor_session().world;
  if ((world == nullptr) || (scriptPath == nullptr) ||
      (scriptPath[0] == '\0')) {
    return;
  }
  const ScriptPropertySchema *schema = script_property_schema(scriptPath);
  ScriptPropertiesComponent overrides{};
  if (world->get_script_properties_ptr(entity) != nullptr) {
    static_cast<void>(world->get_script_properties(entity, &overrides));
  }

  ImGui::PushID("ScriptProperties");
  if (schema == nullptr) {
    ImGui::TextColored(kProblemColor, "Script not found or unreadable: %s",
                       scriptPath);
  } else {
    for (std::size_t i = 0U; i < schema->count; ++i) {
      const ScriptPropertyDecl &decl = schema->properties[i];
      const ScriptPropertiesComponent::Override *entry =
          runtime::script_property_override(overrides, behaviour, decl.name);
      const bool stale =
          (entry != nullptr) && (entry->value.type != decl.defaultValue.type);
      const bool overridden = (entry != nullptr) && !stale;
      ScriptPropertyValue value = overridden ? entry->value : decl.defaultValue;

      char label[64] = {};
      script_property_label(decl.name, label, sizeof(label));
      ImGui::PushID(decl.name);
      if (overridden) {
        ImGui::PushStyleColor(ImGuiCol_Text, kOverriddenColor);
      }
      const bool changed = draw_field(label, decl, &value);
      if (overridden) {
        ImGui::PopStyleColor();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        char shown[ScriptPropertyValue::kMaxTextLength + 8U] = {};
        format_value(decl.defaultValue, shown, sizeof(shown));
        ImGui::SetTooltip(
            "%s%s%s (%s, default %s)%s", decl.tooltip,
            (decl.tooltip[0] != '\0') ? "\n" : "", decl.name,
            scripting::script_property_type_name(decl.defaultValue.type), shown,
            overridden ? "\nThis entity's value differs; "
                         "right-click to reset it"
                       : "");
      }
      bool reset = false;
      if (ImGui::BeginPopupContextItem("PropertyMenu")) {
        reset = ImGui::MenuItem("Reset to Default", nullptr, false,
                                entry != nullptr);
        ImGui::EndPopup();
      }
      if (stale) {
        ImGui::TextColored(
            kProblemColor,
            "The stored value is a %s; the script now declares "
            "a %s.",
            scripting::script_property_type_name(entry->value.type),
            scripting::script_property_type_name(decl.defaultValue.type));
        ImGui::SameLine();
        reset = ImGui::SmallButton("Reset") || reset;
      }
      ScriptPropertiesComponent edited{};
      if (reset) {
        edited = overrides;
        static_cast<void>(
            runtime::script_properties_clear(&edited, behaviour, decl.name));
        stage_overrides(entity, overrides, edited);
        inspector_commit_pending_edit();
      } else if (changed &&
                 script_properties_with_value(overrides, behaviour, decl.name,
                                              value, decl.defaultValue,
                                              &edited)) {
        stage_overrides(entity, overrides, edited);
      }
      ImGui::PopID();
    }
    for (std::size_t i = 0U; i < schema->diagnosticCount; ++i) {
      ImGui::TextColored(kProblemColor, "%s line %u: %s", scriptPath,
                         static_cast<unsigned>(schema->diagnostics[i].line),
                         schema->diagnostics[i].message);
    }
    if (schema->droppedDiagnostics > 0U) {
      ImGui::TextColored(kProblemColor, "%zu more problems in %s",
                         schema->droppedDiagnostics, scriptPath);
    }
  }
  draw_kept_values(entity, behaviour, overrides, schema);
  ImGui::PopID();
}

void reset_script_property_schemas() noexcept {
  for (SchemaCacheEntry &entry : g_schemas) {
    entry = SchemaCacheEntry{};
  }
  g_useClock = 0U;
}

} // namespace engine::editor
