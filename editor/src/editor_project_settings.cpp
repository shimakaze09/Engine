// Implements the Project Settings window: the drafts the Scripting,
// Physics and Saves sections edit, the checks each Apply waits on, and the
// saves that rewrite only their own part of the document before applying it to
// the running engine.

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
#include "engine/runtime/collision_layers.h"
#include "engine/runtime/save_data.h"
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
  /// The collision layers the document holds, and the edits not yet
  /// applied.
  content::ProjectCollisionLayers savedLayers{};
  content::ProjectCollisionLayers draftLayers{};
  /// The save limit in MiB the document puts in force, and the edit not
  /// yet applied.
  int savedSaveMiB = 0;
  int draftSaveMiB = 0;
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
  g_window.savedLayers = document->collisionLayers;
  g_window.draftLayers = g_window.savedLayers;
  g_window.savedSaveMiB = project_saves_draft(document->saveSettings);
  g_window.draftSaveMiB = g_window.savedSaveMiB;
}

bool layers_equal(const content::ProjectCollisionLayers &a,
                  const content::ProjectCollisionLayers &b) noexcept {
  for (std::size_t i = 0U; i < content::kMaxCollisionLayers; ++i) {
    if ((a.collides[i] != b.collides[i]) ||
        (std::strcmp(a.names[i], b.names[i]) != 0)) {
      return false;
    }
  }
  return true;
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

void draw_saves_section() noexcept {
  ImGui::SeparatorText("Saves");
  int &draft = g_window.draftSaveMiB;
  ImGui::SetNextItemWidth(editor_px(180.0F));
  ImGui::InputInt("Largest save (MiB)", &draft, 1, 16);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("The largest save slot engine.save_data writes; a "
                      "larger save is refused and the previous one kept. "
                      "Saves written before a lower limit still load.");
  }
  const char *problem = project_saves_problem(draft);
  if (problem != nullptr) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s", problem);
  }
  const bool changed = draft != g_window.savedSaveMiB;
  ImGui::PushID("saves");
  ImGui::BeginDisabled((problem != nullptr) || !changed);
  if (ImGui::Button("Apply")) {
    if (save_project_saves(g_window.projectFile, draft)) {
      g_window.savedSaveMiB = draft;
      set_status("Saved to the project and applied.", false);
    } else {
      set_status("Not saved; the Log says why.", true);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!changed);
  if (ImGui::Button("Revert")) {
    draft = g_window.savedSaveMiB;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Restore Default")) {
    draft = project_saves_draft(content::ProjectSaveSettings{});
  }
  ImGui::PopID();
}

/// The label a layer goes by in the matrix: its draft name, else
/// "Layer N".
const char *layer_label(const content::ProjectCollisionLayers &layers,
                        std::uint32_t bit, char *scratch,
                        std::size_t capacity) noexcept {
  if (layers.names[bit][0] != '\0') {
    return layers.names[bit];
  }
  std::snprintf(scratch, capacity, "Layer %u", bit);
  return scratch;
}

/// Unity's triangular Layer Collision Matrix over the layers worth
/// showing: the named ones and any whose row is not the default.
void draw_layer_matrix(content::ProjectCollisionLayers &layers) noexcept {
  std::uint32_t shown[content::kMaxCollisionLayers] = {};
  int count = 0;
  for (std::uint32_t bit = 0U; bit < content::kMaxCollisionLayers; ++bit) {
    if ((layers.names[bit][0] != '\0') ||
        (layers.collides[bit] != 0xFFFFFFFFU)) {
      shown[count++] = bit;
    }
  }
  if (count == 0) {
    ImGui::TextDisabled("Name a layer to choose which layers it collides "
                        "with.");
    return;
  }
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_NoHostExtendX;
  if (!ImGui::BeginTable("##layer_matrix", count + 1, kFlags)) {
    return;
  }
  char scratch[24] = {};
  ImGui::TableSetupColumn("##rows", ImGuiTableColumnFlags_WidthFixed);
  // Columns run from the highest layer down, so each row's boxes start at
  // its own layer and the grid is a triangle, as Unity draws it.
  for (int k = count - 1; k >= 0; --k) {
    ImGui::TableSetupColumn(
        layer_label(layers, shown[k], scratch, sizeof(scratch)),
        ImGuiTableColumnFlags_AngledHeader | ImGuiTableColumnFlags_WidthFixed);
  }
  ImGui::TableAngledHeadersRow();
  for (int r = 0; r < count; ++r) {
    const std::uint32_t row = shown[r];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(layer_label(layers, row, scratch, sizeof(scratch)));
    for (int k = count - 1; k >= 0; --k) {
      ImGui::TableNextColumn();
      const std::uint32_t column = shown[k];
      if (column < row) {
        continue;
      }
      bool collide = (layers.collides[row] & (1U << column)) != 0U;
      ImGui::PushID(
          static_cast<int>((row * content::kMaxCollisionLayers) + column));
      if (ImGui::Checkbox("##pair", &collide)) {
        content::set_collision_layer_pair(&layers, row, column, collide);
      }
      if (ImGui::IsItemHovered()) {
        char other[24] = {};
        ImGui::SetTooltip("%s and %s",
                          layer_label(layers, row, scratch, sizeof(scratch)),
                          layer_label(layers, column, other, sizeof(other)));
      }
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
}

void draw_physics_section() noexcept {
  ImGui::SeparatorText("Physics");
  content::ProjectCollisionLayers &layers = g_window.draftLayers;
  if (ImGui::TreeNode("Layer Names")) {
    ImGui::TextDisabled("Names label the 32 collision layer bits; a scene "
                        "keeps its bits whatever they are called.");
    for (std::uint32_t bit = 0U; bit < content::kMaxCollisionLayers; ++bit) {
      if ((bit % 2U) != 0U) {
        ImGui::SameLine(editor_px(260.0F));
      }
      ImGui::PushID(static_cast<int>(bit));
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%2u", bit);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(editor_px(200.0F));
      ImGui::InputTextWithHint("##name", "unnamed", layers.names[bit],
                               sizeof(layers.names[bit]));
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Layer Collision Matrix")) {
    draw_layer_matrix(layers);
    ImGui::TreePop();
  }

  char problem[160] = {};
  const bool hasProblem =
      project_physics_problem(layers, problem, sizeof(problem));
  if (hasProblem) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s", problem);
  }
  const bool changed = !layers_equal(layers, g_window.savedLayers);
  ImGui::PushID("physics");
  ImGui::BeginDisabled(hasProblem || !changed);
  if (ImGui::Button("Apply")) {
    if (save_project_physics(g_window.projectFile, layers,
                             editor_session().world)) {
      g_window.savedLayers = layers;
      set_status("Saved to the project and applied.", false);
    } else {
      set_status("Not saved; the Log says why.", true);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!changed);
  if (ImGui::Button("Revert")) {
    layers = g_window.savedLayers;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Restore Defaults")) {
    layers = content::ProjectCollisionLayers{};
  }
  ImGui::PopID();
}

} // namespace

bool project_physics_problem(const content::ProjectCollisionLayers &layers,
                             char *out, std::size_t capacity) noexcept {
  const auto valid = content::validate_collision_layers(layers);
  if (valid.has_value()) {
    return false;
  }
  if ((out != nullptr) && (capacity > 0U)) {
    // The document's field names a layer by its bit, which is how the
    // window numbers them.
    constexpr const char *kPrefix = "physics.layers[";
    const char *field = valid.error().field;
    const std::size_t prefixLength = std::strlen(kPrefix);
    if (std::strncmp(field, kPrefix, prefixLength) == 0) {
      const char *digits = field + prefixLength;
      const char *close = std::strchr(digits, ']');
      const int length =
          (close != nullptr) ? static_cast<int>(close - digits) : 0;
      std::snprintf(out, capacity, "Layer %.*s's name %s.", length, digits,
                    valid.error().reason);
    } else {
      std::snprintf(out, capacity, "The collision matrix %s.",
                    valid.error().reason);
    }
  }
  return true;
}

bool save_project_physics(const char *projectFile,
                          const content::ProjectCollisionLayers &layers,
                          runtime::World *world) noexcept {
  if ((projectFile == nullptr) || (projectFile[0] == '\0')) {
    return false;
  }
  char problem[160] = {};
  if (project_physics_problem(layers, problem, sizeof(problem))) {
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
  document->collisionLayers = layers;
  if (!content::write_project_document(projectFile, *document)) {
    return false; // the writer logged why
  }
  runtime::set_project_collision_layers(layers);
  if (world != nullptr) {
    static_cast<void>(runtime::apply_project_collision_layers(*world));
  }
  char message[400] = {};
  std::snprintf(message, sizeof(message),
                "collision layers saved to %.300s and applied", projectFile);
  core::log_message(core::LogLevel::Info, kLogChannel, message);
  return true;
}

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

const char *project_saves_problem(int maxSlotMiB) noexcept {
  if ((maxSlotMiB < static_cast<int>(content::kProjectMinSaveSlotMiB)) ||
      (maxSlotMiB > static_cast<int>(content::kProjectMaxSaveSlotMiB))) {
    return "The largest save must be from 1 to 256 MiB.";
  }
  return nullptr;
}

int project_saves_draft(const content::ProjectSaveSettings &settings) noexcept {
  return settings.maxSlotMiBSet
             ? static_cast<int>(settings.maxSlotMiB)
             : static_cast<int>(runtime::kDefaultSaveSlotLimitBytes /
                                kBytesPerMiB);
}

bool save_project_saves(const char *projectFile, int maxSlotMiB) noexcept {
  if ((projectFile == nullptr) || (projectFile[0] == '\0')) {
    return false;
  }
  if (const char *problem = project_saves_problem(maxSlotMiB)) {
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
  content::ProjectSaveSettings &settings = document->saveSettings;
  settings.maxSlotMiBSet =
      maxSlotMiB != project_saves_draft(content::ProjectSaveSettings{});
  settings.maxSlotMiB =
      settings.maxSlotMiBSet ? static_cast<std::uint32_t>(maxSlotMiB) : 0U;
  if (!content::write_project_document(projectFile, *document)) {
    return false; // the writer logged why
  }
  static_cast<void>(runtime::set_save_slot_limit(
      static_cast<std::size_t>(maxSlotMiB) * kBytesPerMiB));
  char message[400] = {};
  std::snprintf(message, sizeof(message),
                "save limit saved to %.300s and applied", projectFile);
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
  ImGui::SetNextWindowSize(ImVec2(editor_px(560.0F), editor_px(460.0F)),
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
      draw_physics_section();
      draw_saves_section();
    }
  }
  ImGui::End();
  if (!open) {
    static_cast<void>(core::cvar_set_bool(kShowProjectSettingsCvar, false));
    g_window.seeded = false;
  }
}

} // namespace engine::editor
