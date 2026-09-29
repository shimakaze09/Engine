// Implements the Project Settings window: the draft the Scripting section
// edits, the range and headroom checks Apply waits on, and the save that
// rewrites only the document's script limits before applying them to the
// running VM.

#include "editor_project_settings.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "editor_session.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/scripting/script_limits.h"
#include "engine/scripting/scripting.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor";
constexpr const char *kShowProjectSettingsCvar = "editor.show_project_settings";
constexpr std::size_t kBytesPerMiB = 1024U * 1024U;

/// What the window shows, seeded from the document when it opens.
struct ProjectSettingsWindow final {
  bool seeded = false;
  /// The document the draft was seeded from.
  char projectFile[kProjectOsPathCapacity] = {};
  char projectName[content::kProjectNameCapacity] = {};
  /// Why the document would not read; the window edits nothing then.
  bool readFailed = false;
  /// The limits the document holds, and the edits not yet applied.
  ProjectSettingsDraft saved{};
  ProjectSettingsDraft draft{};
  /// The outcome of the last Apply, shown until the next.
  char status[160] = {};
  bool statusIsError = false;
};

ProjectSettingsWindow g_window{};

bool drafts_equal(const ProjectSettingsDraft &a,
                  const ProjectSettingsDraft &b) noexcept {
  return (a.instructionLimit == b.instructionLimit) &&
         (a.memoryLimitMiB == b.memoryLimitMiB);
}

bool in_range(int value, std::uint32_t minimum, std::uint32_t maximum) noexcept {
  return (value == 0) || ((value >= static_cast<int>(minimum)) &&
                          (static_cast<std::uint32_t>(value) <= maximum));
}

void seed_window(const char *projectFile) noexcept {
  g_window = ProjectSettingsWindow{};
  g_window.seeded = true;
  std::snprintf(g_window.projectFile, sizeof(g_window.projectFile), "%s",
                projectFile);
  std::unique_ptr<content::ProjectDocument> document(
      new (std::nothrow) content::ProjectDocument());
  if ((document == nullptr) ||
      !content::read_project_document(projectFile, document.get())
           .has_value()) {
    g_window.readFailed = true;
    return;
  }
  std::snprintf(g_window.projectName, sizeof(g_window.projectName), "%s",
                document->name);
  g_window.saved = project_settings_draft(document->scriptLimits);
  g_window.draft = g_window.saved;
}

void set_status(const char *text, bool isError) noexcept {
  std::snprintf(g_window.status, sizeof(g_window.status), "%s", text);
  g_window.statusIsError = isError;
}

void draw_scripting_section() noexcept {
  ImGui::SeparatorText("Scripting");
  ProjectSettingsDraft &draft = g_window.draft;
  ImGui::SetNextItemWidth(editor_px(180.0F));
  ImGui::InputInt("Instructions per frame", &draft.instructionLimit, 100000,
                  1000000);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Lua instructions all scripts share in one frame; a "
                      "script that runs past it stops with an error "
                      "instead of freezing the game. 0 is unlimited.");
  }
  ImGui::SetNextItemWidth(editor_px(180.0F));
  ImGui::InputInt("Memory limit (MiB)", &draft.memoryLimitMiB, 16, 256);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Memory the Lua scripts may hold; an allocation past "
                      "it fails with an error. 0 is unlimited.");
  }
  const std::size_t used = scripting::get_memory_used();
  ImGui::TextDisabled("0 means unlimited. Scripts hold %.2f MiB now.",
                      static_cast<double>(used) /
                          static_cast<double>(kBytesPerMiB));

  const char *problem = project_settings_problem(draft, used);
  if (problem != nullptr) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s", problem);
  }
  const bool changed = !drafts_equal(draft, g_window.saved);
  ImGui::BeginDisabled((problem != nullptr) || !changed);
  if (ImGui::Button("Apply")) {
    if (save_project_settings(g_window.projectFile, draft)) {
      g_window.saved = draft;
      set_status("Saved to the project and applied.", false);
    } else {
      set_status("Not saved; the Log says why.", true);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!changed);
  if (ImGui::Button("Revert")) {
    draft = g_window.saved;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Restore Defaults")) {
    draft = default_project_settings_draft();
  }
  if (g_window.status[0] != '\0') {
    if (g_window.statusIsError) {
      ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s",
                         g_window.status);
    } else {
      ImGui::TextDisabled("%s", g_window.status);
    }
  }
}

} // namespace

ProjectSettingsDraft
project_settings_draft(const content::ProjectScriptLimits &limits) noexcept {
  const ScriptLimits inForce = project_script_limits(limits);
  ProjectSettingsDraft draft{};
  draft.instructionLimit = inForce.instructionLimit;
  draft.memoryLimitMiB = static_cast<int>(inForce.memoryLimitBytes / kBytesPerMiB);
  return draft;
}

ProjectSettingsDraft default_project_settings_draft() noexcept {
  return project_settings_draft(content::ProjectScriptLimits{});
}

const char *project_settings_problem(const ProjectSettingsDraft &draft,
                                     std::size_t memoryUsed) noexcept {
  if (!in_range(draft.instructionLimit, content::kProjectMinInstructionLimit,
                content::kProjectMaxInstructionLimit)) {
    return "Instructions per frame must be 0 (unlimited) or from 100000 to "
           "1000000000.";
  }
  if (!in_range(draft.memoryLimitMiB, content::kProjectMinMemoryLimitMiB,
                content::kProjectMaxMemoryLimitMiB)) {
    return "The memory limit must be 0 (unlimited) or from 16 to 2048 MiB.";
  }
  if ((draft.memoryLimitMiB != 0) &&
      (static_cast<std::size_t>(draft.memoryLimitMiB) * kBytesPerMiB <=
       memoryUsed)) {
    return "The running scripts already hold more memory than that.";
  }
  return nullptr;
}

bool save_project_settings(const char *projectFile,
                           const ProjectSettingsDraft &draft) noexcept {
  if ((projectFile == nullptr) || (projectFile[0] == '\0')) {
    return false;
  }
  if (const char *problem =
          project_settings_problem(draft, scripting::get_memory_used())) {
    core::log_message(core::LogLevel::Error, kLogChannel, problem);
    return false;
  }
  std::unique_ptr<content::ProjectDocument> document(
      new (std::nothrow) content::ProjectDocument());
  if ((document == nullptr) ||
      !content::read_project_document(projectFile, document.get())
           .has_value()) {
    return false; // the reader logged why
  }
  const ProjectSettingsDraft defaults = default_project_settings_draft();
  content::ProjectScriptLimits &limits = document->scriptLimits;
  limits.instructionLimitSet =
      draft.instructionLimit != defaults.instructionLimit;
  limits.instructionLimit =
      limits.instructionLimitSet
          ? static_cast<std::uint32_t>(draft.instructionLimit)
          : 0U;
  limits.memoryLimitSet = draft.memoryLimitMiB != defaults.memoryLimitMiB;
  limits.memoryLimitMiB = limits.memoryLimitSet
                              ? static_cast<std::uint32_t>(draft.memoryLimitMiB)
                              : 0U;
  if (!content::write_project_document(projectFile, *document)) {
    return false; // the writer logged why
  }
  scripting::set_instruction_limit(draft.instructionLimit);
  scripting::set_memory_limit(static_cast<std::size_t>(draft.memoryLimitMiB) *
                              kBytesPerMiB);
  char message[400] = {};
  std::snprintf(message, sizeof(message),
                "project settings saved to %.300s and applied", projectFile);
  core::log_message(core::LogLevel::Info, kLogChannel, message);
  return true;
}

void register_project_settings() noexcept {
  static_cast<void>(core::cvar_register_bool(
      kShowProjectSettingsCvar, false,
      "Toggle the Project Settings window (Edit menu)"));
}

void draw_project_settings_panel() noexcept {
  if (!core::cvar_get_bool(kShowProjectSettingsCvar, false) ||
      !has_open_project()) {
    g_window.seeded = false;
    return;
  }
  draw_project_settings_window(active_config().projectFile);
}

void draw_project_settings_window(const char *projectFile) noexcept {
  if (projectFile == nullptr) {
    return;
  }
  if (!g_window.seeded ||
      (std::strcmp(g_window.projectFile, projectFile) != 0)) {
    seed_window(projectFile);
  }
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(editor_px(560.0F), editor_px(300.0F)),
                           ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Project Settings", &open)) {
    if (g_window.readFailed) {
      ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F),
                         "The project document will not read; the Log says "
                         "why.");
      ImGui::TextWrapped("%s", g_window.projectFile);
    } else {
      ImGui::Text("Project: %s", g_window.projectName);
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("Saved in %s, for everyone who opens the project.",
                         g_window.projectFile);
      ImGui::PopStyleColor();
      draw_scripting_section();
    }
  }
  ImGui::End();
  if (!open) {
    static_cast<void>(core::cvar_set_bool(kShowProjectSettingsCvar, false));
    g_window.seeded = false;
  }
}

} // namespace engine::editor
