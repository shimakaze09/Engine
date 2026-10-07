// Implements the project hub: the recent-projects list, opening a
// project by its document, creating one from the empty-project template,
// and the full-viewport screen that offers them.

#include "editor_project_hub.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/content/project_document.h"
#include "engine/core/platform.h"
#include "engine/engine.h"
#include "engine/project.h"

#include "editor_scene_document.h"
#include "editor_session.h"

namespace engine::editor {

namespace {

namespace fs = std::filesystem;

constexpr const char *kNewProjectPopup = "New Project";
/// The engine content's empty-project template, under the engine root.
constexpr const char *kEmptyProjectTemplate = "templates~/empty_project";

/// The hub's per-session state.
struct HubState final {
  char error[1024] = {};
  /// A failed open the author has not acknowledged: drawn as a modal
  /// while a project is open, where the hub and its inline error are not.
  bool openErrorPending = false;
  char newName[content::kProjectNameCapacity] = {};
  char newLocation[kProjectOsPathCapacity] = {};
  bool openNewProjectPopup = false;
  core::FileDialogTicket openDialog = core::kNoFileDialog;
  core::FileDialogTicket folderDialog = core::kNoFileDialog;
};

HubState g_hub{};
/// Kept whole: a project that is not found stays listed, marked, as Unity
/// Hub and Godot's project manager keep one, so a project on a drive that
/// is not connected comes back with it.
RecentList g_recentProjects{"editor_recent_projects.json", "projects",
                            kMaxRecentEntries, nullptr};

/// Which recent projects were not found, checked when the list's entries
/// change rather than every frame: a path on a network drive that is gone
/// can stall each check.
struct MissingCache final {
  char checked[kMaxRecentEntries][kMaxRecentPathLength] = {};
  std::size_t count = 0U;
  bool valid = false;
  bool missing[kMaxRecentEntries] = {};
};

MissingCache g_missing{};

void refresh_missing(std::size_t count) noexcept {
  bool same = g_missing.valid && (g_missing.count == count);
  for (std::size_t i = 0U; same && (i < count); ++i) {
    same = std::strcmp(g_missing.checked[i],
                       recent_list_at(&g_recentProjects, i)) == 0;
  }
  if (same) {
    return;
  }
  g_missing.valid = true;
  g_missing.count = count;
  for (std::size_t i = 0U; i < count; ++i) {
    const char *path = recent_list_at(&g_recentProjects, i);
    std::snprintf(g_missing.checked[i], sizeof(g_missing.checked[i]), "%s",
                  path);
    g_missing.missing[i] = !recent_entry_exists(path);
  }
}

/// Records the last failure; "" forgets it, and any failed open with it.
void set_error(const char *text) noexcept {
  std::snprintf(g_hub.error, sizeof(g_hub.error), "%s",
                (text != nullptr) ? text : "");
  if (g_hub.error[0] == '\0') {
    g_hub.openErrorPending = false;
  }
}

/// The directory a new project is suggested in: the one holding the most
/// recent project, so projects made one after another land together.
void suggest_location() noexcept {
  if (g_hub.newLocation[0] != '\0') {
    return;
  }
  const char *latest = recent_list_at(&g_recentProjects, 0U);
  if (latest[0] == '\0') {
    return;
  }
  std::error_code ec{};
  fs::path project(latest);
  if (!fs::is_directory(project, ec)) {
    project = project.parent_path();
  }
  const std::string parent = project.parent_path().string();
  if (parent.size() < sizeof(g_hub.newLocation)) {
    std::memcpy(g_hub.newLocation, parent.c_str(), parent.size() + 1U);
  }
}

/// Takes a finished dialog's chosen path into `out`; true when one was
/// chosen. A dialog still open, or cancelled, gives nothing.
bool take_dialog(core::FileDialogTicket *ticket, char *out,
                 std::size_t capacity) noexcept {
  if (*ticket == core::kNoFileDialog) {
    return false;
  }
  core::FileDialogResult result{};
  const core::FileDialogPoll poll =
      core::platform_take_file_dialog_result(*ticket, &result);
  if (poll == core::FileDialogPoll::Pending) {
    return false;
  }
  *ticket = core::kNoFileDialog;
  if ((poll != core::FileDialogPoll::Ready) ||
      (result.outcome != core::FileDialogOutcome::Chosen) ||
      (std::strlen(result.path) >= capacity)) {
    return false;
  }
  std::snprintf(out, capacity, "%s", result.path);
  return true;
}

void draw_recent_rows() noexcept {
  const std::size_t count = recent_list_count(&g_recentProjects);
  if (count == 0U) {
    ImGui::TextDisabled("No recent projects. Create one, or open one from "
                        "disk.");
    return;
  }
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("##recent_projects", 3, kFlags)) {
    return;
  }
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 2.5F);
  ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableHeadersRow();
  refresh_missing(count);
  // A copy of the path: Open or Remove below changes the list.
  char chosen[kMaxRecentPathLength] = {};
  bool open = false;
  bool remove = false;
  for (std::size_t i = 0U; i < count; ++i) {
    const char *path = recent_list_at(&g_recentProjects, i);
    char name[content::kProjectNameCapacity] = {};
    if (!project_display_name(path, name, sizeof(name))) {
      std::snprintf(name, sizeof(name), "(unnamed)");
    }
    const bool missing = g_missing.missing[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Selectable(name, false,
                          ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap |
                              ImGuiSelectableFlags_AllowDoubleClick) &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !missing) {
      std::snprintf(chosen, sizeof(chosen), "%s", path);
      open = true;
    }
    ImGui::TableNextColumn();
    if (missing) {
      ImGui::TextColored(ImVec4(0.9F, 0.6F, 0.3F, 1.0F), "Missing: %s", path);
      ImGui::SetItemTooltip("Nothing is at this path now: the project was "
                            "moved or deleted, or is on a drive that is not "
                            "connected. Remove drops it from this list.");
    } else {
      ImGui::TextDisabled("%s", path);
    }
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(missing);
    if (ImGui::SmallButton("Open")) {
      std::snprintf(chosen, sizeof(chosen), "%s", path);
      open = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      std::snprintf(chosen, sizeof(chosen), "%s", path);
      remove = true;
    }
    ImGui::SetItemTooltip("Remove from this list; the project stays on disk");
    ImGui::PopID();
  }
  ImGui::EndTable();
  if (open) {
    static_cast<void>(project_hub_open(chosen));
  } else if (remove) {
    recent_list_remove(&g_recentProjects, chosen);
  }
}

void draw_new_project_popup() noexcept {
  if (g_hub.openNewProjectPopup) {
    ImGui::OpenPopup(kNewProjectPopup);
    g_hub.openNewProjectPopup = false;
  }
  if (!ImGui::BeginPopupModal(kNewProjectPopup, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  static_cast<void>(take_dialog(&g_hub.folderDialog, g_hub.newLocation,
                                sizeof(g_hub.newLocation)));
  const float fieldWidth = ImGui::GetFontSize() * 28.0F;
  ImGui::TextUnformatted("Template: Empty 3D (camera, light, main script)");
  ImGui::SetNextItemWidth(fieldWidth);
  ImGui::InputText("Name", g_hub.newName, sizeof(g_hub.newName));
  ImGui::SetNextItemWidth(fieldWidth);
  ImGui::InputText("Location", g_hub.newLocation, sizeof(g_hub.newLocation));
  ImGui::SameLine();
  ImGui::BeginDisabled(g_hub.folderDialog != core::kNoFileDialog);
  if (ImGui::Button("Browse...")) {
    g_hub.folderDialog = core::platform_request_file_dialog(
        core::FileDialogKind::Folder, nullptr, 0,
        (g_hub.newLocation[0] != '\0') ? g_hub.newLocation : nullptr);
  }
  ImGui::EndDisabled();
  if ((g_hub.newName[0] != '\0') && (g_hub.newLocation[0] != '\0')) {
    ImGui::TextDisabled("Creates %s/%s", g_hub.newLocation, g_hub.newName);
  }
  if (g_hub.error[0] != '\0') {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95F, 0.45F, 0.4F, 1.0F));
    ImGui::TextWrapped("%s", g_hub.error);
    ImGui::PopStyleColor();
  }
  ImGui::Separator();
  ImGui::BeginDisabled((g_hub.newName[0] == '\0') ||
                       (g_hub.newLocation[0] == '\0'));
  if (ImGui::Button("Create")) {
    if (project_hub_create(g_hub.newLocation, g_hub.newName)) {
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    set_error("");
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

} // namespace

RecentList &recent_projects() noexcept { return g_recentProjects; }

bool project_display_name(const char *path, char *out,
                          std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((path == nullptr) || (path[0] == '\0')) {
    return false;
  }
  fs::path parsed(path);
  while (!parsed.empty() && !parsed.has_filename()) {
    parsed = parsed.parent_path();
  }
  const std::string name =
      (parsed.extension() == content::kProjectFileExtension)
          ? parsed.stem().string()
          : parsed.filename().string();
  if (name.empty() || (name.size() >= capacity)) {
    return false;
  }
  std::memcpy(out, name.c_str(), name.size() + 1U);
  return true;
}

void project_hub_note_open_project() noexcept {
  if (has_open_project()) {
    recent_list_add(&g_recentProjects, active_config().projectFile);
  }
}

void project_hub_seed_bundled_sample() noexcept {
  if (recent_list_count(&g_recentProjects) != 0U) {
    return;
  }
  // Listed by its document, as every opened project is, so opening it
  // later finds the same entry rather than adding a second one.
  char sample[kMaxRecentPathLength] = {};
  static ProjectStorage storage{};
  EngineConfig probe{};
  if (find_bundled_sample_project(sample, sizeof(sample)) &&
      open_project(sample, &storage, &probe).has_value()) {
    recent_list_add(&g_recentProjects, storage.projectFile);
  }
}

bool project_hub_open(const char *path) noexcept {
  // Checked here, before this run ends, so a project that cannot open
  // says why in the hub instead of bouncing the editor back to it.
  static ProjectStorage storage{};
  EngineConfig probe{};
  const auto opened = open_project(path, &storage, &probe);
  if (!opened.has_value()) {
    const ProjectOpenFailure &failure = opened.error();
    char message[sizeof(g_hub.error)] = {};
    std::snprintf(message, sizeof(message), "%.500s: %s%s%s",
                  (path != nullptr) ? path : "",
                  project_open_failure_text(failure.kind),
                  (failure.detail[0] != '\0') ? ": " : "", failure.detail);
    set_error(message);
    g_hub.openErrorPending = true;
    return false;
  }
  set_error("");
  // A project chosen by its directory is listed by its document instead.
  if ((path != nullptr) && (std::strcmp(path, storage.projectFile) != 0)) {
    recent_list_remove(&g_recentProjects, path);
  }
  recent_list_add(&g_recentProjects, storage.projectFile);
  if (editor_session().playState != PlayState::Stopped) {
    stop_play_mode();
  }
  if (request_scene_project_switch(storage.projectFile)) {
    static_cast<void>(request_project_switch(storage.projectFile));
  }
  return true;
}

bool project_hub_create(const char *location, const char *name) noexcept {
  const std::string templateDir =
      (fs::path(active_config().engineRoot) / kEmptyProjectTemplate).string();
  char projectFile[kProjectOsPathCapacity] = {};
  const auto created = create_project(location, name, templateDir.c_str(),
                                      projectFile, sizeof(projectFile));
  if (!created.has_value()) {
    char message[256] = {};
    std::snprintf(message, sizeof(message), "The project was not created: %s.",
                  project_create_failure_text(created.error()));
    set_error(message);
    return false;
  }
  g_hub.newName[0] = '\0';
  return project_hub_open(projectFile);
}

void project_hub_request_open_dialog() noexcept {
  if (g_hub.openDialog != core::kNoFileDialog) {
    return;
  }
  static const core::FileDialogFilter kFilter{"Project", "project"};
  g_hub.openDialog = core::platform_request_file_dialog(
      core::FileDialogKind::Open, &kFilter, 1, nullptr);
}

void project_hub_poll_dialogs() noexcept {
  char opened[kMaxRecentPathLength] = {};
  if (take_dialog(&g_hub.openDialog, opened, sizeof(opened))) {
    static_cast<void>(project_hub_open(opened));
  }
}

void project_hub_close_project() noexcept {
  if (editor_session().playState != PlayState::Stopped) {
    stop_play_mode();
  }
  if (request_scene_project_switch("")) {
    static_cast<void>(request_project_switch(""));
  }
}

const char *project_hub_error() noexcept { return g_hub.error; }

bool project_hub_open_error_pending() noexcept {
  return g_hub.openErrorPending;
}

void project_hub_acknowledge_error() noexcept { set_error(""); }

void draw_project_open_error_popup() noexcept {
  constexpr const char *kPopup = "Could not open project";
  if (g_hub.openErrorPending && !ImGui::IsPopupOpen(kPopup)) {
    ImGui::OpenPopup(kPopup);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport != nullptr) {
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.5F, 0.0F),
                             ImGuiCond_Appearing);
  }
  if (!ImGui::BeginPopupModal(kPopup, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  // Forgotten elsewhere (a later open that succeeded): nothing to show.
  if (!g_hub.openErrorPending) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::TextUnformatted("The project was not opened; this one stays open.");
  ImGui::Separator();
  ImGui::TextWrapped("%s", g_hub.error);
  if (ImGui::Button("OK")) {
    project_hub_acknowledge_error();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void draw_project_hub() noexcept {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport == nullptr) {
    return;
  }
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::SetNextWindowViewport(viewport->ID);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin(kProjectHubWindow, nullptr, kFlags)) {
    ImGui::SetWindowFontScale(1.4F);
    ImGui::TextUnformatted("Projects");
    ImGui::SetWindowFontScale(1.0F);
    ImGui::SameLine();
    const float buttons = ImGui::CalcTextSize("New Project...").x +
                          ImGui::CalcTextSize("Open...").x +
                          (ImGui::GetStyle().FramePadding.x * 4.0F) +
                          ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttons);
    if (ImGui::Button("New Project...")) {
      set_error("");
      suggest_location();
      g_hub.openNewProjectPopup = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_hub.openDialog != core::kNoFileDialog);
    if (ImGui::Button("Open...")) {
      project_hub_request_open_dialog();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if ((g_hub.error[0] != '\0') && !ImGui::IsPopupOpen(kNewProjectPopup)) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95F, 0.45F, 0.4F, 1.0F));
      ImGui::TextWrapped("%s", g_hub.error);
      ImGui::PopStyleColor();
    }
    draw_recent_rows();
    draw_new_project_popup();
  }
  ImGui::End();
}

void project_hub_reset() noexcept {
  core::platform_abandon_file_dialog(g_hub.openDialog);
  core::platform_abandon_file_dialog(g_hub.folderDialog);
  g_hub = HubState{};
  g_missing = MissingCache{};
  recent_list_forget(&g_recentProjects);
}

} // namespace engine::editor
