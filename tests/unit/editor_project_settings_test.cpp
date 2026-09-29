// Verifies the Project Settings window's Scripting section through its
// production draft, checks and save:
// - the draft shows the limits in force, the engine's defaults where the
//   document sets none;
// - a limit outside the document's range, or a memory limit the running
//   scripts already exceed, is refused with a reason;
// - Apply writes the limits into the .project document, keeping every
//   other field, and puts them on the running VM; saving the defaults
//   back leaves the document byte-identical to one that never set them;
// - a refused draft (out of range, or below what running scripts hold), a
//   missing document and a malformed one each change neither the file nor
//   the running limits;
// - the window, drawn on a headless ImGui frame, shows the project and
//   its limits, or says the document will not read.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "editor_project_settings.h"
#include "engine/content/project_document.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/scripting/script_limits.h"
#include "engine/scripting/scripting.h"

namespace {

namespace fs = std::filesystem;
namespace ct = engine::content;
namespace sc = engine::scripting;
using namespace engine::editor;

constexpr std::size_t kMiB = 1024U * 1024U;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

std::string read_all(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// Writes a valid document setting no limits and returns its bytes.
std::string write_plain_project(const fs::path &file) {
  ct::ProjectDocument doc{};
  std::snprintf(doc.name, sizeof(doc.name), "%s", "Island");
  std::snprintf(doc.version, sizeof(doc.version), "%s", "0.1.0");
  doc.guid = engine::core::AssetGuid{0x5a17f00d0badcafeULL,
                                     0x1234567890abcdefULL};
  std::snprintf(doc.contentRoot, sizeof(doc.contentRoot), "%s", "assets");
  std::snprintf(doc.cacheRoot, sizeof(doc.cacheRoot), "%s", ".cache");
  std::snprintf(doc.scenes[0], sizeof(doc.scenes[0]), "%s",
                "assets/main.scene");
  std::snprintf(doc.scenes[1], sizeof(doc.scenes[1]), "%s",
                "assets/level.scene");
  doc.sceneCount = 2U;
  std::snprintf(doc.startupScene, sizeof(doc.startupScene), "%s",
                "assets/main.scene");
  std::snprintf(doc.mainScript, sizeof(doc.mainScript), "%s",
                "assets/main.lua");
  const std::string path = file.string();
  if (!ct::write_project_document(path.c_str(), doc)) {
    return {};
  }
  return read_all(file);
}

void set_running_defaults() noexcept {
  sc::set_instruction_limit(sc::kDefaultInstructionLimit);
  sc::set_memory_limit(sc::kDefaultMemoryLimit);
}

bool running_is(int instructions, std::size_t bytes) noexcept {
  return (sc::get_instruction_limit() == instructions) &&
         (sc::get_memory_limit() == bytes);
}

void check_draft() {
  const ProjectSettingsDraft defaults =
      project_settings_draft(ct::ProjectScriptLimits{});
  check((defaults.instructionLimit == sc::kDefaultInstructionLimit) &&
            (static_cast<std::size_t>(defaults.memoryLimitMiB) * kMiB ==
             sc::kDefaultMemoryLimit),
        "a document setting no limits shows the engine's defaults");
  const ProjectSettingsDraft restored = default_project_settings_draft();
  check((restored.instructionLimit == defaults.instructionLimit) &&
            (restored.memoryLimitMiB == defaults.memoryLimitMiB),
        "Restore Defaults is the engine's defaults");
  const ProjectSettingsDraft set =
      project_settings_draft(ct::ProjectScriptLimits{true, 0U, true, 256U});
  check((set.instructionLimit == 0) && (set.memoryLimitMiB == 256),
        "a document's own limits, 0 for unlimited, show as set");
}

void check_problems() {
  struct Case final {
    int instructions;
    int memoryMiB;
    std::size_t used;
    bool accepted;
    const char *name;
  };
  const Case cases[] = {
      {1000000, 64, 0U, true, "the defaults are accepted"},
      {0, 0, 0U, true, "0 (unlimited) is accepted for both"},
      {100000, 16, 0U, true, "the lowest limits are accepted"},
      {1000000000, 2048, 0U, true, "the highest limits are accepted"},
      {99999, 64, 0U, false, "an instruction limit below the range"},
      {1000000001, 64, 0U, false, "an instruction limit above the range"},
      {-1, 64, 0U, false, "a negative instruction limit"},
      {1000000, 15, 0U, false, "a memory limit below the range"},
      {1000000, 2049, 0U, false, "a memory limit above the range"},
      {1000000, -16, 0U, false, "a negative memory limit"},
      {1000000, 16, 16U * kMiB, false,
       "a memory limit the scripts already reach"},
      {1000000, 16, (16U * kMiB) - 1U, true,
       "a memory limit just above what the scripts hold"},
      {1000000, 0, 4096U * kMiB, true,
       "unlimited memory whatever the scripts hold"},
  };
  for (const Case &c : cases) {
    const char *problem = project_settings_problem(
        ProjectSettingsDraft{c.instructions, c.memoryMiB}, c.used);
    check(c.accepted ? (problem == nullptr)
                     : ((problem != nullptr) && (problem[0] != '\0')),
          c.name);
  }
}

void check_save(const fs::path &root) {
  const fs::path file = root / "Island.project";
  const std::string original = write_plain_project(file);
  check(!original.empty(), "write a project that sets no limits");
  const std::string path = file.string();
  set_running_defaults();

  check(save_project_settings(path.c_str(),
                              ProjectSettingsDraft{2500000, 128}),
        "Apply saves new limits");
  ct::ProjectDocument reread{};
  check(ct::read_project_document(path.c_str(), &reread).has_value() &&
            reread.scriptLimits.instructionLimitSet &&
            (reread.scriptLimits.instructionLimit == 2500000U) &&
            reread.scriptLimits.memoryLimitSet &&
            (reread.scriptLimits.memoryLimitMiB == 128U),
        "the document holds the new limits");
  check((std::strcmp(reread.name, "Island") == 0) &&
            (reread.sceneCount == 2U) &&
            (std::strcmp(reread.scenes[1], "assets/level.scene") == 0) &&
            (std::strcmp(reread.mainScript, "assets/main.lua") == 0),
        "every other field is kept");
  check(running_is(2500000, 128U * kMiB),
        "the running scripts are under the new limits at once");

  check(save_project_settings(path.c_str(),
                              ProjectSettingsDraft{2500000, 64}),
        "Apply saves one limit back to its default");
  check(ct::read_project_document(path.c_str(), &reread).has_value() &&
            reread.scriptLimits.instructionLimitSet &&
            !reread.scriptLimits.memoryLimitSet,
        "a limit equal to the default is left unset");

  check(save_project_settings(path.c_str(), default_project_settings_draft()),
        "Apply saves the defaults");
  check(read_all(file) == original,
        "the defaults leave the document byte-identical to one that never "
        "set limits");
  check(running_is(sc::kDefaultInstructionLimit, sc::kDefaultMemoryLimit),
        "and the running scripts are back on the defaults");

  check(!save_project_settings(path.c_str(), ProjectSettingsDraft{1000000, 8}),
        "a draft out of range is not saved");
  check((read_all(file) == original) &&
            running_is(sc::kDefaultInstructionLimit, sc::kDefaultMemoryLimit),
        "and neither the document nor the running limits change");

  // Scripts that already hold 17 MiB: a 16 MiB limit would starve them,
  // so it is refused even though it is inside the document's range.
  const fs::path hog = root / "hog.lua";
  {
    std::ofstream out(hog, std::ios::binary);
    out << "held = string.rep('x', 17 * 1024 * 1024)\n";
  }
  const std::string hogPath = hog.string();
  if (sc::initialize_scripting() && sc::load_script(hogPath.c_str()) &&
      (sc::get_memory_used() > 16U * kMiB)) {
    check(!save_project_settings(path.c_str(),
                                 ProjectSettingsDraft{1000000, 16}),
          "a memory limit the running scripts already exceed is not saved");
    check((read_all(file) == original) &&
              running_is(sc::kDefaultInstructionLimit, sc::kDefaultMemoryLimit),
          "and neither the document nor the running limits change");
  } else {
    g_tests.fail("run a script holding 17 MiB");
  }
  sc::shutdown_scripting();

  const fs::path missing = root / "Missing.project";
  const std::string missingPath = missing.string();
  std::error_code ec{};
  check(!save_project_settings(missingPath.c_str(),
                               ProjectSettingsDraft{2500000, 128}) &&
            !fs::exists(missing, ec) &&
            running_is(sc::kDefaultInstructionLimit, sc::kDefaultMemoryLimit),
        "a missing document is not created, and the limits do not change");

  const fs::path broken = root / "Broken.project";
  {
    std::ofstream out(broken, std::ios::binary);
    out << "{\"schemaVersion\": 1, \"identity\": ";
  }
  const std::string brokenText = read_all(broken);
  const std::string brokenPath = broken.string();
  check(!save_project_settings(brokenPath.c_str(),
                               ProjectSettingsDraft{2500000, 128}) &&
            (read_all(broken) == brokenText) &&
            running_is(sc::kDefaultInstructionLimit, sc::kDefaultMemoryLimit),
        "a malformed document is left as it was, and the limits do not "
        "change");
}

std::string draw_frame(const char *projectFile) noexcept {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(700.0F, 500.0F));
  draw_project_settings_window(projectFile);
  const std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

void check_window(const fs::path &root) {
  const fs::path file = root / "Island.project";
  const std::string path = file.string();
  check(save_project_settings(path.c_str(), ProjectSettingsDraft{2500000, 0}),
        "set limits for the window to show");
  const std::string text = draw_frame(path.c_str());
  check(text.find("Project: Island") != std::string::npos,
        "the window names the project");
  check((text.find("Instructions per frame") != std::string::npos) &&
            (text.find("Memory limit (MiB)") != std::string::npos),
        "the window shows both limits");
  check((text.find("Apply") != std::string::npos) &&
            (text.find("Restore Defaults") != std::string::npos),
        "the window offers Apply and Restore Defaults");
  const std::string broken = (root / "Broken.project").string();
  const std::string refused = draw_frame(broken.c_str());
  check((refused.find("will not read") != std::string::npos) &&
            (refused.find("Instructions per frame") == std::string::npos),
        "a document that will not read is reported, and nothing is "
        "editable");
  set_running_defaults();
}

} // namespace

int main() {
  static_cast<void>(engine::core::initialize_logging());
  static_cast<void>(engine::core::initialize_cvars());
  register_project_settings();
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  const fs::path root =
      fs::temp_directory_path() / "engine_editor_project_settings_test";
  std::error_code ec{};
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  check_draft();
  check_problems();
  check_save(root);
  check_window(root);

  fs::remove_all(root, ec);
  ImGui::DestroyContext();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  return g_tests.finish("editor_project_settings");
}
