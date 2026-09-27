// Implements the editor's Rendering panel: one control per entry of the
// renderer's settings table, reading and writing the cvar the entry
// names, so a setting the renderer adds appears here with no editor
// change.

#include "editor_panels_rendering.h"

#include <cstddef>
#include <cstring>

#include "engine/core/cvar.h"
#include "engine/renderer/render_settings_table.h"
#include "imgui.h"

namespace engine::editor {

namespace {

constexpr const char *kShowRenderingCvar = "editor.show_rendering";

/// Draws one setting's control, writing its cvar when the author changes
/// it, with the entry's tooltip on hover.
void draw_setting(const renderer::RenderSettingEntry &entry) noexcept {
  switch (entry.kind) {
  case renderer::RenderSettingKind::Toggle: {
    bool value = core::cvar_get_bool(entry.cvar, false);
    if (ImGui::Checkbox(entry.label, &value)) {
      static_cast<void>(core::cvar_set_bool(entry.cvar, value));
    }
    break;
  }
  case renderer::RenderSettingKind::Slider: {
    float value = core::cvar_get_float(entry.cvar, entry.minValue);
    if (ImGui::SliderFloat(entry.label, &value, entry.minValue, entry.maxValue,
                           "%.3f")) {
      static_cast<void>(core::cvar_set_float(entry.cvar, value));
    }
    break;
  }
  case renderer::RenderSettingKind::Choice: {
    const char *current = core::cvar_get_string(entry.cvar, "");
    int selected = -1;
    for (std::size_t i = 0U; i < entry.choiceCount; ++i) {
      if (std::strcmp(current, entry.choices[i]) == 0) {
        selected = static_cast<int>(i);
      }
    }
    if (ImGui::Combo(entry.label, &selected, entry.choices,
                     static_cast<int>(entry.choiceCount)) &&
        (selected >= 0)) {
      static_cast<void>(core::cvar_set_string(
          entry.cvar, entry.choices[static_cast<std::size_t>(selected)]));
    }
    break;
  }
  }
  if ((entry.tooltip[0] != '\0') && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", entry.tooltip);
  }
}

} // namespace

void register_rendering_panel_cvars() noexcept {
  static_cast<void>(core::cvar_register_bool(
      kShowRenderingCvar, false,
      "Toggle the editor Rendering panel (Window menu)"));
}

void draw_rendering_panel() noexcept {
  if (!core::cvar_get_bool(kShowRenderingCvar, false)) {
    return;
  }
  bool open = true;
  if (ImGui::Begin("Rendering", &open)) {
    std::size_t count = 0U;
    const renderer::RenderSettingEntry *entries =
        renderer::render_setting_entries(&count);
    const char *section = nullptr;
    for (std::size_t i = 0U; i < count; ++i) {
      if ((section == nullptr) ||
          (std::strcmp(section, entries[i].section) != 0)) {
        section = entries[i].section;
        ImGui::SeparatorText(section);
      }
      draw_setting(entries[i]);
    }
  }
  ImGui::End();
  if (!open) {
    static_cast<void>(core::cvar_set_bool(kShowRenderingCvar, false));
  }
}

} // namespace engine::editor
