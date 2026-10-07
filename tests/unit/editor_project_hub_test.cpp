// Verifies the project hub over an engine running with no project: a
// project is shown under its document's stem or its directory's name; a
// path that opens no project is refused with the reason and its detail in
// the hub and requests no switch, and the modal that shows it while a
// project is open names it until acknowledged; a document the codec
// refuses shows the codec's reason; creating a project from the engine's
// template lists it by its .project document and requests the switch to it; a
// project opened by its directory is listed by its document, once; a name the
// document refuses creates nothing; and the hub draws its title, actions
// and every recent project.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "../asset_root.h"
#include "../test_harness.h"
#include "editor_project_hub.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/runtime/editor_bridge.h"

namespace {

namespace fs = std::filesystem;
using namespace engine::editor;

engine::tests::TestContext g_tests;

/// The pending switch, taken: its path, or "(none)" when none is pending.
std::string take_switch() {
  char path[engine::kProjectOsPathCapacity] = {};
  bool toHub = false;
  if (!engine::take_project_switch(path, sizeof(path), &toHub)) {
    return "(none)";
  }
  return path;
}

std::string name_of(const char *path) {
  char name[64] = {};
  return project_display_name(path, name, sizeof(name)) ? name : "(refused)";
}

/// Draws one hub frame and returns what it rendered as text.
std::string hub_frame() {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  draw_project_hub();
  std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

/// Draws one frame of the failed-open modal and returns what it rendered.
std::string error_popup_frame() {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  draw_project_open_error_popup();
  std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

} // namespace

int main() {
  // The engine's content root, which the template lives under, found the
  // way an installed engine finds it.
  if (!engine::tests::enter_asset_root()) {
    return 2;
  }
  // The hub's functions need the engine running, not the editor's device
  // objects, which a headless run has no device for.
  engine::runtime::set_editor_bridge(nullptr);
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  engine::configure_without_project(&config);
  if (!engine::bootstrap(config)) {
    return 3;
  }
  std::error_code ec{};
  const fs::path scratch =
      fs::temp_directory_path(ec) / "engine_editor_project_hub_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch / "projects", ec);
  if (ec) {
    return 4;
  }
  recent_lists_set_directory_override_for_tests(
      (scratch / "store").string().c_str());
  project_hub_reset();
  // A context for the frames the modal and the hub draw.
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

  g_tests.check(name_of("/work/My Game/My Game.project") == "My Game",
                "a document is shown under its stem");
  g_tests.check(name_of("/work/Other") == "Other" &&
                    name_of("/work/Other/") == "Other",
                "a directory is shown under its name");
  g_tests.check(name_of("") == "(refused)" &&
                    name_of(("/w/" + std::string(80, 'n')).c_str()) ==
                        "(refused)",
                "an empty path or a name too long for the buffer is refused");

  // A path that is no project.
  const std::string nowhere = (scratch / "nowhere").string();
  g_tests.check(
      !project_hub_open(nowhere.c_str()) &&
          (std::strstr(project_hub_error(), "no project found") != nullptr),
      "a path that is no project is refused with the reason");
  g_tests.check(take_switch() == "(none)" &&
                    (recent_list_count(&recent_projects()) == 0U),
                "and neither switches nor is listed");
  g_tests.check(std::strstr(project_hub_error(),
                            "not a directory or a .project document") !=
                    nullptr,
                "the reason carries the refusal's detail");

  // With a project open the hub is not drawn: the failure is a modal
  // until acknowledged, then forgotten.
  g_tests.check(project_hub_open_error_pending(),
                "a failed open waits to be acknowledged");
  const std::string popup = error_popup_frame();
  g_tests.check((popup.find("Could not open project") != std::string::npos) &&
                    (popup.find("this one stays open") != std::string::npos) &&
                    (popup.find(nowhere) != std::string::npos) &&
                    (popup.find("not a directory or a .project document") !=
                     std::string::npos),
                "the modal names the path, the reason and its detail");
  project_hub_acknowledge_error();
  // The frame that closes a modal still draws its title; the next draws
  // nothing.
  static_cast<void>(error_popup_frame());
  const std::string closed = error_popup_frame();
  g_tests.check(
      !project_hub_open_error_pending() && (project_hub_error()[0] == '\0') &&
          (closed.find("Could not open project") == std::string::npos),
      "acknowledging closes it and forgets the reason, so the hub "
      "does not show it later");

  // A document the codec refuses: its own reason reaches the author.
  const fs::path broken = scratch / "broken";
  fs::create_directories(broken, ec);
  {
    std::ofstream out(broken / "Broken.project", std::ios::binary);
    out << "{\"schema";
  }
  engine::ProjectStorage probeStorage{};
  engine::EngineConfig probeConfig{};
  const auto refused = engine::open_project(broken.string().c_str(),
                                            &probeStorage, &probeConfig);
  g_tests.check(
      !refused.has_value() && (refused.error().document.reason[0] != '\0') &&
          !project_hub_open(broken.string().c_str()) &&
          (std::strstr(project_hub_error(),
                       engine::project_open_failure_text(
                           engine::ProjectOpenFailureKind::DocumentRefused)) !=
           nullptr) &&
          (std::strstr(project_hub_error(), refused.error().document.reason) !=
           nullptr),
      "a refused document shows the kind and the codec's reason");
  project_hub_acknowledge_error();

  // Create and open.
  const std::string location = (scratch / "projects").string();
  g_tests.check(project_hub_create(location.c_str(), "Game") &&
                    (project_hub_error()[0] == '\0'),
                "a project is created from the template");
  const std::string gameFile =
      (fs::path(location) / "Game" / "Game.project").generic_string();
  g_tests.check((recent_list_count(&recent_projects()) == 1U) &&
                    (gameFile == recent_list_at(&recent_projects(), 0U)),
                "it is listed by its document");
  g_tests.check(take_switch() == gameFile, "and the switch to it is asked");

  // Opened by its directory: listed once, by its document.
  const std::string gameDir = (fs::path(location) / "Game").string();
  recent_list_add(&recent_projects(), gameDir.c_str());
  g_tests.check(project_hub_open(gameDir.c_str()) &&
                    (recent_list_count(&recent_projects()) == 1U) &&
                    (gameFile == recent_list_at(&recent_projects(), 0U)),
                "a project opened by its directory is listed once, by its "
                "document");
  g_tests.check(take_switch() == gameFile, "and switches to its document");

  // A refused name creates nothing.
  g_tests.check(!project_hub_create(location.c_str(), "bad/name") &&
                    (std::strstr(project_hub_error(), "name") != nullptr),
                "a name the document refuses is refused with the reason");
  g_tests.check(!fs::exists(fs::path(location) / "bad", ec) &&
                    (take_switch() == "(none)"),
                "and creates and switches to nothing");

  // The screen.
  const std::string text = hub_frame();
  g_tests.check((text.find("Projects") != std::string::npos) &&
                    (text.find("New Project...") != std::string::npos) &&
                    (text.find("Open...") != std::string::npos),
                "the hub offers New Project and Open");
  g_tests.check((text.find("Game") != std::string::npos) &&
                    (text.find(gameFile) != std::string::npos),
                "and lists the recent project with its location");
  const ImGuiWindow *window = ImGui::FindWindowByName(kProjectHubWindow);
  g_tests.check((window != nullptr) && window->Active &&
                    (window->Size.x == io.DisplaySize.x),
                "over the whole viewport");
  ImGui::DestroyContext();

  project_hub_reset();
  recent_lists_set_directory_override_for_tests("");
  engine::shutdown();
  fs::remove_all(scratch, ec);
  return g_tests.finish("editor_project_hub");
}
