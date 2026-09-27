// Implements the editor preferences declared in editor_preferences.h: an
// ImGui settings section named [EnginePreferences][Editor] holding one
// key=value line per preference, the main window's geometry restored from
// it, and the window that edits them.

#include "editor_preferences.h"

#include "editor_session.h"
#include "editor_shortcuts.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace engine::editor {

namespace {

constexpr const char *kCjkFontCvar = "editor.cjk_font";
constexpr const char *kShowPreferencesCvar = "editor.show_preferences";
constexpr const char *kSectionType = "EnginePreferences";
constexpr const char *kCjkFontKey = "CjkFont=";
constexpr const char *kWindowSizeKey = "WindowSize=";
constexpr const char *kWindowMaximizedKey = "WindowMaximized=";
/// A rebound shortcut: Shortcut.<action id>=<chord>, one line per action
/// whose chords differ from the default.
constexpr const char *kShortcutKey = "Shortcut.";
/// Which axes the move and rotate handles follow: World or Local.
constexpr const char *kGizmoSpaceKey = "GizmoSpace=";
/// Whether the Scene view draws its reference grid: 1 or 0.
constexpr const char *kShowGridKey = "ShowGrid=";
/// The Scene camera's fly speed in metres per second.
constexpr const char *kCameraSpeedKey = "CameraSpeed=";

/// The geometry the layout file stored, and whether it waits to be
/// applied.
core::WindowGeometry g_storedGeometry{};
bool g_geometryPending = false;

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

/// Before the layout file is read: nothing staged yet.
void read_init(ImGuiContext *, ImGuiSettingsHandler *) noexcept {
  begin_stored_shortcuts();
}

/// After it is read: the stored bindings apply together, so their
/// conflicts are judged on the final table (see commit_stored_shortcuts).
void apply_all(ImGuiContext *, ImGuiSettingsHandler *) noexcept {
  commit_stored_shortcuts();
}

void read_line(ImGuiContext *, ImGuiSettingsHandler *, void *,
               const char *line) noexcept {
  const std::size_t shortcutKeyLength = std::strlen(kShortcutKey);
  if (std::strncmp(line, kShortcutKey, shortcutKeyLength) == 0) {
    const char *id = line + shortcutKeyLength;
    const char *equals = std::strchr(id, '=');
    char idBuffer[64] = {};
    const std::size_t idLength =
        (equals != nullptr) ? static_cast<std::size_t>(equals - id) : 0U;
    if ((idLength == 0U) || (idLength >= sizeof(idBuffer))) {
      core::log_message(core::LogLevel::Warning, "editor",
                        "stored shortcut line without an action id ignored");
      return;
    }
    std::memcpy(idBuffer, id, idLength);
    static_cast<void>(stage_stored_shortcut(idBuffer, equals + 1));
    return;
  }
  const std::size_t spaceKeyLength = std::strlen(kGizmoSpaceKey);
  if (std::strncmp(line, kGizmoSpaceKey, spaceKeyLength) == 0) {
    const char *value = line + spaceKeyLength;
    if (std::strcmp(value, "World") == 0) {
      editor_session().gizmoWorldSpace = true;
    } else if (std::strcmp(value, "Local") == 0) {
      editor_session().gizmoWorldSpace = false;
    } else {
      core::log_message(core::LogLevel::Warning, "editor",
                        "stored GizmoSpace is neither World nor Local; "
                        "ignored");
    }
    return;
  }
  const std::size_t speedKeyLength = std::strlen(kCameraSpeedKey);
  if (std::strncmp(line, kCameraSpeedKey, speedKeyLength) == 0) {
    const char *value = line + speedKeyLength;
    const char *end = value + std::strlen(value);
    // strtof, not std::from_chars: AppleClang's libc++ deletes the
    // floating-point overload. The checks keep from_chars's strictness:
    // the whole token, no leading space, no overflow.
    errno = 0;
    char *parseEnd = nullptr;
    const float speed = std::strtof(value, &parseEnd);
    if ((value != end) &&
        (std::isspace(static_cast<unsigned char>(value[0])) == 0) &&
        (parseEnd == end) && (errno != ERANGE) && std::isfinite(speed) &&
        (speed > 0.0F)) {
      editor_session().editorCamera.flySpeed = std::clamp(
          speed, EditorCamera::kMinFlySpeed, EditorCamera::kMaxFlySpeed);
    } else {
      core::log_message(core::LogLevel::Warning, "editor",
                        "stored CameraSpeed is not a positive number; "
                        "ignored");
    }
    return;
  }
  const std::size_t gridKeyLength = std::strlen(kShowGridKey);
  if (std::strncmp(line, kShowGridKey, gridKeyLength) == 0) {
    const char *value = line + gridKeyLength;
    if ((value[0] == '0' || value[0] == '1') && (value[1] == '\0')) {
      editor_session().showGrid = value[0] == '1';
    } else {
      core::log_message(core::LogLevel::Warning, "editor",
                        "stored ShowGrid is neither 0 nor 1; ignored");
    }
    return;
  }
  const std::size_t keyLength = std::strlen(kCjkFontKey);
  if (std::strncmp(line, kCjkFontKey, keyLength) == 0) {
    static_cast<void>(core::cvar_set_string(kCjkFontCvar, line + keyLength));
    return;
  }
  const std::size_t sizeKeyLength = std::strlen(kWindowSizeKey);
  const std::size_t maximizedKeyLength = std::strlen(kWindowMaximizedKey);
  const char *const end = line + std::strlen(line);
  if (std::strncmp(line, kWindowSizeKey, sizeKeyLength) == 0) {
    // WindowSize=<width>x<height>, both positive, nothing after.
    int width = 0;
    int height = 0;
    const std::from_chars_result w =
        std::from_chars(line + sizeKeyLength, end, width);
    if ((w.ec != std::errc{}) || (w.ptr == end) || (*w.ptr != 'x')) {
      return;
    }
    const std::from_chars_result h = std::from_chars(w.ptr + 1, end, height);
    if ((h.ec == std::errc{}) && (h.ptr == end) && (width > 0) &&
        (height > 0)) {
      g_storedGeometry.width = width;
      g_storedGeometry.height = height;
      g_geometryPending = true;
    }
  } else if (std::strncmp(line, kWindowMaximizedKey, maximizedKeyLength) == 0) {
    int maximized = 0;
    const std::from_chars_result m =
        std::from_chars(line + maximizedKeyLength, end, maximized);
    if ((m.ec == std::errc{}) && (m.ptr == end)) {
      g_storedGeometry.maximized = (maximized != 0);
    }
  }
}

void write_all(ImGuiContext *, ImGuiSettingsHandler *handler,
               ImGuiTextBuffer *buffer) noexcept {
  buffer->appendf("[%s][Editor]\n", handler->TypeName);
  buffer->appendf("%s%s\n", kCjkFontKey,
                  core::cvar_get_string(kCjkFontCvar, ""));
  // The live window's geometry; without a window (a headless run) the one
  // that was read is carried through, so such a run never forgets it.
  core::WindowGeometry geometry{};
  if (!core::platform_window_geometry(&geometry) || (geometry.width <= 0) ||
      (geometry.height <= 0)) {
    geometry = g_storedGeometry;
  }
  if ((geometry.width > 0) && (geometry.height > 0)) {
    buffer->appendf("%s%dx%d\n", kWindowSizeKey, geometry.width,
                    geometry.height);
    buffer->appendf("%s%d\n", kWindowMaximizedKey, geometry.maximized ? 1 : 0);
  }
  buffer->appendf("%s%s\n", kGizmoSpaceKey,
                  editor_session().gizmoWorldSpace ? "World" : "Local");
  buffer->appendf("%s%d\n", kShowGridKey, editor_session().showGrid ? 1 : 0);
  buffer->appendf("%s%.9g\n", kCameraSpeedKey,
                  static_cast<double>(editor_session().editorCamera.flySpeed));
  for (std::size_t i = 0U; i < editor_shortcut_count(); ++i) {
    const EditorShortcut &row = editor_shortcut_at(i);
    char chord[40] = {};
    if (editor_action_rebound(row.action) &&
        format_key_chord(row.chord, chord, sizeof(chord))) {
      buffer->appendf("%s%s=%s\n", kShortcutKey, row.id, chord);
    }
  }
  buffer->append("\n");
}

/// The key a capture takes: the first keyboard key other than a modifier
/// pressed this frame, or ImGuiKey_None. Mouse buttons and gamepad keys
/// never bind.
ImGuiKey captured_key() noexcept {
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
    const ImGuiKey key = static_cast<ImGuiKey>(k);
    const bool modifier =
        (key == ImGuiKey_LeftCtrl) || (key == ImGuiKey_RightCtrl) ||
        (key == ImGuiKey_LeftShift) || (key == ImGuiKey_RightShift) ||
        (key == ImGuiKey_LeftAlt) || (key == ImGuiKey_RightAlt) ||
        (key == ImGuiKey_LeftSuper) || (key == ImGuiKey_RightSuper);
    if (!modifier && ImGui::IsKeyPressed(key, false)) {
      return key;
    }
  }
  return ImGuiKey_None;
}

/// The Shortcuts section: every action's chord, rebound by clicking it and
/// pressing the new one, or removed from its context menu. Escape,
/// clicking it again or leaving the window cancels, so a capture never
/// outlives the window's focus and swallows keys meant elsewhere. A chord
/// another action holds is refused and named.
void draw_shortcut_bindings() noexcept {
  static char s_refusal[128] = {};
  ImGui::SeparatorText("Shortcuts");
  const EditorAction capturing = shortcut_capture_target();
  if ((capturing != EditorAction::Count) &&
      !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    end_shortcut_capture();
  } else if (capturing != EditorAction::Count) {
    const ImGuiKey key = captured_key();
    if (key == ImGuiKey_Escape) {
      end_shortcut_capture();
    } else if (key != ImGuiKey_None) {
      const ImGuiKeyChord chord =
          static_cast<ImGuiKeyChord>(ImGui::GetIO().KeyMods) | key;
      EditorAction conflict = EditorAction::Count;
      if (rebind_editor_action(capturing, chord, &conflict)) {
        s_refusal[0] = '\0';
        ImGui::MarkIniSettingsDirty();
      } else if (conflict != EditorAction::Count) {
        std::snprintf(s_refusal, sizeof(s_refusal), "That chord is %s's.",
                      editor_shortcut(conflict).label);
      } else {
        std::snprintf(s_refusal, sizeof(s_refusal),
                      "That key cannot be bound.");
      }
      end_shortcut_capture();
    }
  }
  if (ImGui::BeginTable("shortcuts", 2,
                        ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingFixedFit)) {
    for (std::size_t i = 0U; i < editor_shortcut_count(); ++i) {
      const EditorShortcut &row = editor_shortcut_at(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(row.label);
      ImGui::TableNextColumn();
      ImGui::PushID(row.id);
      const char *shown = (shortcut_capture_target() == row.action)
                              ? "Press a chord..."
                              : editor_shortcut_text(row.action);
      if (ImGui::Button((shown[0] != '\0') ? shown : "(none)")) {
        if (shortcut_capture_target() == row.action) {
          end_shortcut_capture();
        } else {
          begin_shortcut_capture(row.action);
        }
        s_refusal[0] = '\0';
      }
      if (ImGui::BeginPopupContextItem("binding_menu")) {
        if (ImGui::MenuItem("Remove Shortcut", nullptr, false,
                            row.chord != 0) &&
            rebind_editor_action(row.action, 0, nullptr)) {
          ImGui::MarkIniSettingsDirty();
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (s_refusal[0] != '\0') {
    ImGui::TextColored(ImVec4(0.9F, 0.35F, 0.35F, 1.0F), "%s", s_refusal);
  }
  if (ImGui::Button("Restore defaults")) {
    reset_editor_shortcuts();
    ImGui::MarkIniSettingsDirty();
  }
}

} // namespace

void register_editor_preferences() noexcept {
  static_cast<void>(core::cvar_register_string(
      kCjkFontCvar, "",
      "Font file for Chinese and Japanese text in the editor; empty uses the "
      "bundled Noto Sans SC. Set from Edit > Preferences"));
  static_cast<void>(
      core::cvar_register_bool(kShowPreferencesCvar, false,
                               "Toggle the Preferences window (Edit menu)"));
  if ((ImGui::GetCurrentContext() == nullptr) ||
      (ImGui::FindSettingsHandler(kSectionType) != nullptr)) {
    return;
  }
  ImGuiSettingsHandler handler{};
  handler.TypeName = kSectionType;
  handler.TypeHash = ImHashStr(kSectionType);
  handler.ReadInitFn = &read_init;
  handler.ReadOpenFn = &read_open;
  handler.ReadLineFn = &read_line;
  handler.ApplyAllFn = &apply_all;
  handler.WriteAllFn = &write_all;
  ImGui::AddSettingsHandler(&handler);
}

void apply_stored_window_geometry() noexcept {
  if (!g_geometryPending) {
    return;
  }
  g_geometryPending = false;
  static_cast<void>(core::platform_apply_window_geometry(g_storedGeometry));
}

void set_loaded_cjk_font(const char *path) noexcept {
  std::snprintf(g_loadedCjkFont, sizeof(g_loadedCjkFont), "%s",
                (path != nullptr) ? path : "");
}

void draw_editor_preferences_panel() noexcept {
  if (!core::cvar_get_bool(kShowPreferencesCvar, false)) {
    g_draftSeeded = false;
    end_shortcut_capture();
    return;
  }
  if (!g_draftSeeded) {
    std::snprintf(g_fontDraft, sizeof(g_fontDraft), "%s",
                  core::cvar_get_string(kCjkFontCvar, ""));
    g_draftSeeded = true;
  }
  bool open = true;
  if (ImGui::Begin("Preferences", &open)) {
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
    draw_shortcut_bindings();
  } else {
    end_shortcut_capture(); // collapsed
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
