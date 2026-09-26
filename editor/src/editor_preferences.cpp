// Implements the editor preferences declared in editor_preferences.h: an
// ImGui settings section named [EnginePreferences][Editor] holding one
// key=value line per preference, and the window that edits them.

#include "editor_preferences.h"

#include <cstdio>
#include <cstring>

#include "engine/core/cvar.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace engine::editor {

namespace {

constexpr const char *kCjkFontCvar = "editor.cjk_font";
constexpr const char *kShowPreferencesCvar = "editor.show_preferences";
constexpr const char *kSectionType = "EnginePreferences";
constexpr const char *kCjkFontKey = "CjkFont=";

char g_loadedCjkFont[512] = {};
/// The path being edited in the window, seeded from the cvar when the
/// window opens.
char g_fontDraft[512] = {};
bool g_draftSeeded = false;

void *read_open(ImGuiContext *, ImGuiSettingsHandler *,
                const char *name) noexcept {
  // One section; any non-null entry token lets ImGui route its lines here.
  return (std::strcmp(name, "Editor") == 0) ? static_cast<void *>(&g_fontDraft)
                                            : nullptr;
}

void read_line(ImGuiContext *, ImGuiSettingsHandler *, void *,
               const char *line) noexcept {
  const std::size_t keyLength = std::strlen(kCjkFontKey);
  if (std::strncmp(line, kCjkFontKey, keyLength) == 0) {
    static_cast<void>(core::cvar_set_string(kCjkFontCvar, line + keyLength));
  }
}

void write_all(ImGuiContext *, ImGuiSettingsHandler *handler,
               ImGuiTextBuffer *buffer) noexcept {
  buffer->appendf("[%s][Editor]\n", handler->TypeName);
  buffer->appendf("%s%s\n", kCjkFontKey,
                  core::cvar_get_string(kCjkFontCvar, ""));
  buffer->append("\n");
}

} // namespace

void register_editor_preferences() noexcept {
  static_cast<void>(core::cvar_register_string(
      kCjkFontCvar, "",
      "Font file for Chinese and Japanese text in the editor; empty uses the "
      "bundled Noto Sans SC. Set from Window > Editor Settings"));
  static_cast<void>(core::cvar_register_bool(
      kShowPreferencesCvar, false,
      "Toggle the Editor Settings window (Window menu)"));
  if ((ImGui::GetCurrentContext() == nullptr) ||
      (ImGui::FindSettingsHandler(kSectionType) != nullptr)) {
    return;
  }
  ImGuiSettingsHandler handler{};
  handler.TypeName = kSectionType;
  handler.TypeHash = ImHashStr(kSectionType);
  handler.ReadOpenFn = &read_open;
  handler.ReadLineFn = &read_line;
  handler.WriteAllFn = &write_all;
  ImGui::AddSettingsHandler(&handler);
}

void set_loaded_cjk_font(const char *path) noexcept {
  std::snprintf(g_loadedCjkFont, sizeof(g_loadedCjkFont), "%s",
                (path != nullptr) ? path : "");
}

void draw_editor_preferences_panel() noexcept {
  if (!core::cvar_get_bool(kShowPreferencesCvar, false)) {
    g_draftSeeded = false;
    return;
  }
  if (!g_draftSeeded) {
    std::snprintf(g_fontDraft, sizeof(g_fontDraft), "%s",
                  core::cvar_get_string(kCjkFontCvar, ""));
    g_draftSeeded = true;
  }
  bool open = true;
  if (ImGui::Begin("Editor Settings", &open)) {
    ImGui::SeparatorText("Font");
    ImGui::TextWrapped("In use for Chinese and Japanese text: %s",
                       (g_loadedCjkFont[0] != '\0') ? g_loadedCjkFont : "none");
    ImGui::InputTextWithHint("Font file", "empty uses the bundled face",
                             g_fontDraft, sizeof(g_fontDraft));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("A .ttf, .ttc or .otf file with the characters "
                        "your project uses");
    }
    if (ImGui::Button("Apply")) {
      static_cast<void>(core::cvar_set_string(kCjkFontCvar, g_fontDraft));
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Use bundled")) {
      g_fontDraft[0] = '\0';
      static_cast<void>(core::cvar_set_string(kCjkFontCvar, ""));
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::TextDisabled("Takes effect the next time the editor starts.");
  }
  ImGui::End();
  if (!open) {
    static_cast<void>(core::cvar_set_bool(kShowPreferencesCvar, false));
  }
}

std::size_t editor_preferences_section(char *out,
                                       std::size_t capacity) noexcept {
  ImGuiSettingsHandler *handler = ImGui::FindSettingsHandler(kSectionType);
  if ((handler == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return 0U;
  }
  ImGuiTextBuffer buffer;
  handler->WriteAllFn(ImGui::GetCurrentContext(), handler, &buffer);
  const std::size_t size = static_cast<std::size_t>(buffer.size());
  if (size + 1U > capacity) {
    return 0U;
  }
  std::memcpy(out, buffer.c_str(), size + 1U);
  return size;
}

} // namespace engine::editor
