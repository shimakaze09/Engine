// Implements the editor Console panel: log view, filters, and the
// non-spamming status indicator shown while the panel is closed.

#include "editor_panels_console.h"

#include "editor_console_capture.h"
#include "editor_console_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/runtime/world.h"

#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace engine::editor {

namespace {

/// One color per severity, reused for the row tag and the search-bar
/// toggle buttons so the mapping stays visually consistent.
ImVec4 level_color(core::LogLevel level) noexcept {
  switch (level) {
  case core::LogLevel::Trace:
    return ImVec4(0.55F, 0.55F, 0.58F, 1.0F);
  case core::LogLevel::Info:
    return ImVec4(0.75F, 0.78F, 0.82F, 1.0F);
  case core::LogLevel::Warning:
    return ImVec4(0.95F, 0.72F, 0.25F, 1.0F);
  case core::LogLevel::Error:
    return ImVec4(0.92F, 0.35F, 0.32F, 1.0F);
  case core::LogLevel::Fatal:
    return ImVec4(1.0F, 0.15F, 0.15F, 1.0F);
  default:
    return ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
  }
}

/// Sets `editor_session().selectedAssetPath` to `path` so the Assets panel
/// highlights and inspects it — the "navigate to source" action for script
/// and asset diagnostics (there is no OS text-editor integration yet; this
/// is the documented fallback scope).
void select_asset_in_browser(const char *path) noexcept {
  std::snprintf(editor_session().selectedAssetPath,
               sizeof(editor_session().selectedAssetPath), "%s", path);
}

/// Runs an entry's primary navigation action (double-click or the detail
/// pane's button): select the referenced asset/script in the Assets panel,
/// or select the referenced entity in the hierarchy when it safely
/// resolves against the attached world.
void navigate_to_entry(const ConsoleEntry &entry) noexcept {
  if (entry.referenceKind == ConsoleReferenceKind::ScriptLocation ||
     entry.referenceKind == ConsoleReferenceKind::AssetPath) {
    select_asset_in_browser(entry.referencePath);
    return;
  }
  if (entry.entityPersistentId != runtime::kInvalidPersistentId) {
    const runtime::Entity resolved = console_capture_resolve_entity(
        entry.entityPersistentId, editor_session().world);
    if (resolved != runtime::kInvalidEntity) {
      select_entity(resolved, false);
    }
  }
}

/// Draws one entry's row plus its inline navigation controls.
/// Copies `message` up to its first newline into `out` (a Lua traceback
/// carries "\n\t..." frames; the row stays one line and the full text,
/// newlines included, is available in the hover tooltip instead).
void first_line(const char *message, char *out, std::size_t outCapacity) noexcept {
  std::size_t i = 0U;
  for (; (i + 1U < outCapacity) && (message[i] != '\0') &&
        (message[i] != '\n');
      ++i) {
    out[i] = message[i];
  }
  out[i] = '\0';
  const bool hasMore = (message[i] != '\0');
  if (hasMore && (i + 4U < outCapacity)) {
    std::snprintf(out + i, outCapacity - i, " [...]");
  }
}

/// Copies every entry the filters show, one line each, oldest first: a
/// plain-text export of what the Log is showing. Built only when asked.
void copy_shown_log(const ConsoleFilter &filter) noexcept {
  std::string copyText;
  const std::size_t count = console_capture_entry_count();
  for (std::size_t i = 0U; i < count; ++i) {
    ConsoleEntry entry{};
    if (!console_capture_get_entry(i, &entry) ||
        !console_filter_matches(filter, entry)) {
      continue;
    }
    char line[kConsoleMessageCapacity + 96] = {};
    std::snprintf(line, sizeof(line), "[%s][%s] %s%s\n",
                  core::log_level_to_string(entry.level), entry.channel,
                  entry.message, (entry.repeatCount > 1U) ? " (repeated)" : "");
    copyText += line;
  }
  ImGui::SetClipboardText(copyText.c_str());
}

/// The items every right-click in the log offers, on a line or not: what
/// the Log's own buttons and toggles do, as Unreal's Output Log offers
/// Clear Log and Copy wherever it is right-clicked.
void draw_log_menu_items(ConsolePanelState &console) noexcept {
  if (ImGui::MenuItem("Clear")) {
    console_capture_clear();
  }
  if (ImGui::MenuItem("Copy All")) {
    copy_shown_log(console.filter);
  }
  ImGui::Separator();
  ImGui::MenuItem("Collapse", nullptr, &console.collapseView);
  ImGui::MenuItem("Autoscroll", nullptr, &console.autoScroll);
  ImGui::MenuItem("Pause", nullptr, &console.paused);
}

/// The menu a right-click on the log's empty space opens; a line's own
/// menu, over the line, ends with the same items.
void draw_log_space_menu(ConsolePanelState &console) noexcept {
  if (!ImGui::BeginPopupContextWindow("log_space_menu",
                                      ImGuiPopupFlags_MouseButtonRight |
                                          ImGuiPopupFlags_NoOpenOverItems)) {
    return;
  }
  draw_log_menu_items(console);
  ImGui::EndPopup();
}

void draw_entry_row(const ConsoleEntry &entry, std::size_t rowIndex) noexcept {
  ImGui::PushID(static_cast<int>(rowIndex));
  ImGui::PushStyleColor(ImGuiCol_Text, level_color(entry.level));

  char header[64] = {};
  std::snprintf(header, sizeof(header), "%6.2fs [%-7s] %s",
               static_cast<double>(entry.captureTimeMs) / 1000.0,
               core::log_level_to_string(entry.level), entry.channel);

  char messageLine[kConsoleMessageCapacity] = {};
  first_line(entry.message, messageLine, sizeof(messageLine));

  char label[kConsoleMessageCapacity + 96] = {};
  if (entry.repeatCount > 1U) {
    std::snprintf(label, sizeof(label), "%s  %s  (x%u)", header, messageLine,
                 entry.repeatCount);
  } else {
    std::snprintf(label, sizeof(label), "%s  %s", header, messageLine);
  }

  const bool hasNavigation =
      (entry.referenceKind != ConsoleReferenceKind::None) ||
      (entry.entityPersistentId != runtime::kInvalidPersistentId);
  if (ImGui::Selectable(label, false,
                        ImGuiSelectableFlags_AllowDoubleClick)) {
    if (hasNavigation && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      navigate_to_entry(entry);
    }
  }
  ImGui::PopStyleColor();

  if (ImGui::IsItemHovered()) {
    // The full untruncated (modulo the ring's own capacity bound) message,
    // newlines included, so a Lua stack traceback reads normally on hover
    // even though the row itself stays a single compact line.
    ImGui::SetTooltip("%s%s", entry.message,
                      entry.truncated ? "\n[...diagnostic truncated...]" : "");
  }

  if (ImGui::BeginPopupContextItem("entry_context")) {
    if (entry.referenceKind == ConsoleReferenceKind::ScriptLocation) {
      char pathLine[kConsolePathCapacity + 16] = {};
      std::snprintf(pathLine, sizeof(pathLine), "%s:%d", entry.referencePath,
                   entry.referenceLine);
      if (ImGui::MenuItem("Select Script in Assets")) {
        select_asset_in_browser(entry.referencePath);
      }
      if (ImGui::MenuItem("Copy path:line")) {
        ImGui::SetClipboardText(pathLine);
      }
    } else if (entry.referenceKind == ConsoleReferenceKind::AssetPath) {
      if (ImGui::MenuItem("Select Asset")) {
        select_asset_in_browser(entry.referencePath);
      }
      if (ImGui::MenuItem("Copy Path")) {
        ImGui::SetClipboardText(entry.referencePath);
      }
    }
    if (entry.entityPersistentId != runtime::kInvalidPersistentId) {
      const runtime::Entity resolved = console_capture_resolve_entity(
          entry.entityPersistentId, editor_session().world);
      const bool canSelect = (resolved != runtime::kInvalidEntity);
      if (ImGui::MenuItem("Select Entity", nullptr, false, canSelect)) {
        select_entity(resolved, false);
      }
    }
    if (ImGui::MenuItem("Copy Message")) {
      ImGui::SetClipboardText(entry.message);
    }
    ImGui::Separator();
    draw_log_menu_items(editor_session().console);
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

/// Tab completes the word under the cursor; Up and Down step through the
/// commands entered before.
int command_line_callback(ImGuiInputTextCallbackData *data) noexcept {
  ConsolePanelState *console = static_cast<ConsolePanelState *>(data->UserData);
  const char *replacement = nullptr;
  char completed[kConsoleCommandCapacity] = {};
  if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    char candidates[512] = {};
    const std::size_t matches =
        complete_console_line(data->Buf, completed, sizeof(completed),
                              candidates, sizeof(candidates));
    if (matches > 1U) {
      core::log_message(core::LogLevel::Info, "console", candidates);
    }
    if (matches > 0U) {
      replacement = completed;
    }
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    replacement = (data->EventKey == ImGuiKey_UpArrow)
                      ? console->history.older()
                      : console->history.newer();
  }
  if ((replacement != nullptr) && (std::strcmp(replacement, data->Buf) != 0)) {
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, replacement);
  }
  return 0;
}

/// The command line under the log: Enter runs the line through the
/// console, whose output reaches the log above it.
void draw_command_line(ConsolePanelState &console) noexcept {
  ImGui::Separator();
  if (console.focusCommandLine) {
    ImGui::SetKeyboardFocusHere();
    console.focusCommandLine = false;
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  constexpr ImGuiInputTextFlags kFlags =
      ImGuiInputTextFlags_EnterReturnsTrue |
      ImGuiInputTextFlags_CallbackCompletion |
      ImGuiInputTextFlags_CallbackHistory;
  if (!ImGui::InputTextWithHint(
          "##ConsoleCommand", "Command (help lists them; Tab completes)",
          console.commandLine, sizeof(console.commandLine), kFlags,
          &command_line_callback, &console)) {
    return;
  }
  if (console.commandLine[0] != '\0') {
    console.history.push(console.commandLine);
    static_cast<void>(core::console_execute(console.commandLine));
    console.scrollToEnd = true;
  }
  console.commandLine[0] = '\0';
  console.history.reset_cursor();
  // Enter leaves the field; the next command goes straight in.
  ImGui::SetKeyboardFocusHere(-1);
}

} // namespace

void draw_console_panel() noexcept {
  ConsolePanelState &console = editor_session().console;
  ConsoleFilter &filter = console.filter;
  bool &autoScroll = console.autoScroll;
  bool &paused = console.paused;
  bool &collapseView = console.collapseView;
  std::size_t &pausedEntryCount = console.pausedEntryCount;

  if (!core::cvar_get_bool("editor.show_log", true)) {
    return;
  }
  // The Log is the editor's log centre, named as Unreal's Output Log is,
  // apart from any operating-system console. A layout saved when it was
  // called Console has no place for it: it opens beside the Assets panel.
  const ImGuiWindow *assets = ImGui::FindWindowByName("Assets");
  if ((assets != nullptr) && (assets->DockId != 0U)) {
    ImGui::SetNextWindowDockID(assets->DockId, ImGuiCond_FirstUseEver);
  }

  if (console.focusRequested) {
    ImGui::SetNextWindowFocus();
    console.focusRequested = false;
  }
  if (!ImGui::Begin(kLogWindow)) {
    ImGui::End();
    return;
  }
  console_capture_mark_seen();

  // Clear stays on the toolbar, as in Unity's Console, being the Log's
  // most frequent action; Copy All is in every right-click menu.
  if (ImGui::Button("Clear")) {
    console_capture_clear();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Pause", &paused);
  ImGui::SameLine();
  ImGui::Checkbox("Autoscroll", &autoScroll);
  ImGui::SameLine();
  ImGui::Checkbox("Collapse", &collapseView);
  ImGui::SameLine();
  ImGui::Checkbox("This Session", &filter.sessionOnly);

  ImGui::SetNextItemWidth(editor_px(220.0F));
  ImGui::InputTextWithHint("##ConsoleSearch", "Search text...",
                           filter.searchText, sizeof(filter.searchText));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(editor_px(140.0F));
  ImGui::InputTextWithHint("##ConsoleChannel", "Channel (exact)...",
                           filter.channelFilter, sizeof(filter.channelFilter));

  ImGui::SameLine();
  ImGui::Checkbox("Trace", &filter.showTrace);
  ImGui::SameLine();
  ImGui::Checkbox("Info", &filter.showInfo);
  ImGui::SameLine();
  ImGui::Checkbox("Warn", &filter.showWarning);
  ImGui::SameLine();
  ImGui::Checkbox("Error", &filter.showError);
  ImGui::SameLine();
  ImGui::Checkbox("Fatal", &filter.showFatal);

  ImGui::Separator();

  const std::size_t liveCount = console_capture_entry_count();
  if (!paused) {
    pausedEntryCount = liveCount;
  }
  const std::size_t visibleCount =
      (pausedEntryCount < liveCount) ? pausedEntryCount : liveCount;

  // The log fills what the command line under it leaves.
  ImGui::BeginChild("##ConsoleScroll",
                    ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing()), false,
                    ImGuiWindowFlags_HorizontalScrollbar);

  // Collapse groups every identical line wherever it falls, first-seen
  // order, as Unity's Console does; off, each captured entry draws on its
  // own (a back-to-back repeat still shows its count).
  if (collapseView) {
    static ConsoleCollapser collapser{};
    collapser.clear();
    for (std::size_t i = 0U; i < visibleCount; ++i) {
      ConsoleEntry entry{};
      if (!console_capture_get_entry(i, &entry)) {
        break;
      }
      if (console_filter_matches(filter, entry)) {
        collapser.add(entry, i);
      }
    }
    for (std::size_t group = 0U; group < collapser.size(); ++group) {
      ConsoleEntry entry{};
      const std::size_t index = collapser.first_index(group);
      if (console_capture_get_entry(index, &entry)) {
        entry.repeatCount = collapser.total(group);
        draw_entry_row(entry, index);
      }
    }
  } else {
    for (std::size_t i = 0U; i < visibleCount; ++i) {
      ConsoleEntry entry{};
      if (!console_capture_get_entry(i, &entry)) {
        break;
      }
      if (console_filter_matches(filter, entry)) {
        draw_entry_row(entry, i);
      }
    }
  }

  if ((autoScroll && !paused &&
       (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F)) ||
      console.scrollToEnd) {
    ImGui::SetScrollHereY(1.0F);
    console.scrollToEnd = false;
  }

  draw_log_space_menu(console);
  ImGui::EndChild();
  draw_command_line(console);
  ImGui::End();
}

float console_status_indicator_width() noexcept {
  char status[64] = {};
  if (!format_console_status(console_capture_unseen_error_count(),
                             console_capture_unseen_warning_count(), status,
                             sizeof(status))) {
    return 0.0F;
  }
  return ImGui::CalcTextSize(status).x;
}

void draw_console_status_indicator() noexcept {
  const std::uint32_t errors = console_capture_unseen_error_count();
  char status[64] = {};
  if (!format_console_status(errors, console_capture_unseen_warning_count(),
                             status, sizeof(status))) {
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text,
                        level_color((errors > 0U) ? core::LogLevel::Error
                                                  : core::LogLevel::Warning));
  const bool clicked =
      ImGui::Selectable(status, false, ImGuiSelectableFlags_None,
                        ImVec2(ImGui::CalcTextSize(status).x, 0.0F));
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Log");
  }
  if (clicked) {
    core::cvar_set_bool("editor.show_log", true);
    editor_session().console.focusRequested = true;
  }
}

} // namespace engine::editor
