// Implements the editor Console panel: log view, filters, and the
// non-spamming status indicator shown while the panel is closed (#155).

#include "editor_panels_console.h"

#include "editor_console_capture.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/runtime/world.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

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
/// is the documented fallback per issue #155's scope).
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
  if (entry.entityIndexHint != kConsoleNoEntityHint) {
    const runtime::Entity resolved = console_capture_resolve_entity_hint(
        entry.entityIndexHint, editor_session().world);
    if (resolved != runtime::kInvalidEntity) {
      select_entity(resolved, false);
    }
  }
}

/// Draws one entry's row plus its inline navigation controls.
void draw_entry_row(const ConsoleEntry &entry, std::size_t rowIndex) noexcept {
  ImGui::PushID(static_cast<int>(rowIndex));
  ImGui::PushStyleColor(ImGuiCol_Text, level_color(entry.level));

  char header[64] = {};
  std::snprintf(header, sizeof(header), "%6.2fs [%-7s] %s",
               static_cast<double>(entry.captureTimeMs) / 1000.0,
               core::log_level_to_string(entry.level), entry.channel);

  char label[kConsoleMessageCapacity + 96] = {};
  if (entry.repeatCount > 1U) {
    std::snprintf(label, sizeof(label), "%s  %s  (x%u)", header,
                 entry.message, entry.repeatCount);
  } else {
    std::snprintf(label, sizeof(label), "%s  %s", header, entry.message);
  }

  const bool hasNavigation =
      (entry.referenceKind != ConsoleReferenceKind::None) ||
      (entry.entityIndexHint != kConsoleNoEntityHint);
  if (ImGui::Selectable(label, false,
                        ImGuiSelectableFlags_AllowDoubleClick)) {
    if (hasNavigation && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      navigate_to_entry(entry);
    }
  }
  ImGui::PopStyleColor();

  if (ImGui::IsItemHovered() && (entry.category == ConsoleSourceCategory::Script)) {
    ImGui::SetTooltip("Script diagnostic — double-click to locate");
  }

  if (hasNavigation && ImGui::BeginPopupContextItem("entry_context")) {
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
    if (entry.entityIndexHint != kConsoleNoEntityHint) {
      const runtime::Entity resolved = console_capture_resolve_entity_hint(
          entry.entityIndexHint, editor_session().world);
      const bool canSelect = (resolved != runtime::kInvalidEntity);
      if (!canSelect) {
        ImGui::BeginDisabled();
      }
      if (ImGui::MenuItem("Select Entity") && canSelect) {
        select_entity(resolved, false);
      }
      if (!canSelect) {
        ImGui::EndDisabled();
      }
    }
    if (ImGui::MenuItem("Copy Message")) {
      ImGui::SetClipboardText(entry.message);
    }
    ImGui::EndPopup();
  } else if (!hasNavigation && ImGui::BeginPopupContextItem("entry_context")) {
    if (ImGui::MenuItem("Copy Message")) {
      ImGui::SetClipboardText(entry.message);
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

} // namespace

void draw_console_panel() noexcept {
  static ConsoleFilter filter{};
  static bool autoScroll = true;
  static bool paused = false;
  static bool collapseView = true;
  // Frozen entry count while paused: the ring keeps recording underneath,
  // but the visible list stops growing until Resume (UI-only, not a
  // capture-layer concept, so it lives here rather than in ConsoleFilter).
  static std::size_t pausedEntryCount = 0U;

  if (!core::cvar_get_bool("editor.show_console", true)) {
    return;
  }

  if (!ImGui::Begin("Console")) {
    ImGui::End();
    return;
  }
  console_capture_mark_seen();

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

  ImGui::SetNextItemWidth(220.0F);
  ImGui::InputTextWithHint("##ConsoleSearch", "Search text...",
                           filter.searchText, sizeof(filter.searchText));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140.0F);
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

  ImGui::BeginChild("##ConsoleScroll", ImVec2(0.0F, 0.0F), false,
                    ImGuiWindowFlags_HorizontalScrollbar);

  // Collapse-view groups adjacent-in-the-filtered-sequence duplicates the
  // same way the capture layer collapses adjacent ingest-time repeats
  // (bounded to one pass, one held-back row, no cross-entry hash table).
  bool havePending = false;
  ConsoleEntry pending{};
  std::size_t pendingRow = 0U;
  std::uint32_t pendingExtra = 0U;

  auto flush_pending = [&]() noexcept {
    if (!havePending) {
      return;
    }
    ConsoleEntry toDraw = pending;
    toDraw.repeatCount += pendingExtra;
    draw_entry_row(toDraw, pendingRow);
    havePending = false;
    pendingExtra = 0U;
  };

  for (std::size_t i = 0U; i < visibleCount; ++i) {
    ConsoleEntry entry{};
    if (!console_capture_get_entry(i, &entry)) {
      break;
    }
    if (!console_filter_matches(filter, entry)) {
      continue;
    }

    if (collapseView && havePending &&
       (pending.level == entry.level) &&
       (std::strcmp(pending.channel, entry.channel) == 0) &&
       (std::strcmp(pending.message, entry.message) == 0)) {
      pendingExtra += entry.repeatCount;
      continue;
    }

    flush_pending();
    pending = entry;
    pendingRow = i;
    havePending = true;
  }
  flush_pending();

  if (autoScroll && !paused &&
     (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F)) {
    ImGui::SetScrollHereY(1.0F);
  }

  ImGui::EndChild();
  ImGui::End();
}

void draw_console_status_indicator() noexcept {
  const std::uint32_t errors = console_capture_unseen_error_count();
  const std::uint32_t warnings = console_capture_unseen_warning_count();
  if ((errors == 0U) && (warnings == 0U)) {
    ImGui::TextUnformatted("Console");
    return;
  }

  char badge[64] = {};
  if (errors > 0U) {
    std::snprintf(badge, sizeof(badge), "Console (%u error%s%s)", errors,
                 (errors == 1U) ? "" : "s",
                 (warnings > 0U) ? ", warnings" : "");
    ImGui::TextColored(level_color(core::LogLevel::Error), "%s", badge);
  } else {
    std::snprintf(badge, sizeof(badge), "Console (%u warning%s)", warnings,
                 (warnings == 1U) ? "" : "s");
    ImGui::TextColored(level_color(core::LogLevel::Warning), "%s", badge);
  }
}

} // namespace engine::editor
