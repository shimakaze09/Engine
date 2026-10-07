// Verifies that the editor's per-user state stays out of the project it
// edits. On a project made from the engine's template, opened and
// bootstrapped as the editor opens one, every per-user writer the editor has runs through its
// production entry point: the layout and the preferences saved with it,
// Recent Projects, Recent Scenes, the content browser's folder and
// filter, and the autosave session marker. Each file must land in the
// per-user save directory or the project's per-user data directory, never
// below the project, and the project's files must hold byte for byte what
// they held before.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>

#include "../asset_root.h"
#include "../test_harness.h"
#include "editor_autosave.h"
#include "editor_layout.h"
#include "editor_project_hub.h"
#include "editor_recent_list.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/core/platform.h"
#include "engine/core/project_data.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/runtime/editor_bridge.h"

namespace {

namespace fs = std::filesystem;
using namespace engine::editor;

engine::tests::TestContext g_tests;

/// Every regular file below `root`, by its path relative to `root`, with
/// its bytes.
std::map<std::string, std::string> snapshot(const fs::path &root) {
  std::map<std::string, std::string> files{};
  std::error_code ec{};
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && (it != end);
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    std::ifstream in(it->path(), std::ios::binary);
    files[fs::relative(it->path(), root, ec).generic_string()] =
        std::string((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  }
  return files;
}

/// True when `path` exists below `directory`.
bool lies_below(const fs::path &path, const fs::path &directory) {
  std::error_code ec{};
  const fs::path file = fs::weakly_canonical(path, ec);
  const fs::path base = fs::weakly_canonical(directory, ec);
  const std::string relative = fs::relative(file, base, ec).generic_string();
  return fs::exists(file, ec) && !relative.empty() &&
         (relative.rfind("..", 0) != 0);
}

} // namespace

int main() {
  std::error_code ec{};
  const std::string engineRoot = engine::tests::engine_root_path();
  if (engineRoot.empty()) {
    return 2;
  }
  // A new project from the engine's template, as New Project makes one.
  const fs::path location =
      fs::temp_directory_path(ec) / "engine_editor_user_data_test";
  fs::remove_all(location, ec);
  fs::create_directories(location, ec);
  char projectFile[engine::kProjectOsPathCapacity] = {};
  const std::string locationText = location.string();
  const std::string templateDir =
      (fs::path(engineRoot) / "templates~" / "empty_project").string();
  if (ec || !engine::create_project(locationText.c_str(), "Game",
                                    templateDir.c_str(), projectFile,
                                    sizeof(projectFile))
                 .has_value()) {
    return 3;
  }
  const fs::path project = location / "Game";

  // Opened and bootstrapped as the editor opens a project.
  engine::runtime::set_editor_bridge(nullptr);
  engine::EngineConfig config{};
  engine::ProjectStorage storage{};
  if (!engine::open_project(projectFile, &storage, &config).has_value()) {
    return 4;
  }
  config.core.platform.headless = true;
  config.engineRoot = engineRoot.c_str();
  if (!engine::bootstrap(config)) {
    return 5;
  }
  char saveText[1024] = {};
  char dataText[1024] = {};
  if (!engine::core::platform_get_save_dir(saveText, sizeof(saveText)) ||
      !engine::core::project_data_dir(dataText, sizeof(dataText))) {
    engine::shutdown();
    return 6;
  }
  const fs::path saveDir = saveText;
  const fs::path dataDir = dataText;
  g_tests.check(!lies_below(dataDir, project) && !lies_below(saveDir, project),
                "the per-user directories lie outside the project");

  const std::map<std::string, std::string> before = snapshot(project);
  g_tests.check(before.size() > 3U, "the project's files are read");

  // Every per-user writer, through its production entry point.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  g_tests.check(editor_layout_initialize(), "the layout is initialized");
  // One frame with a window, so the layout has something to describe.
  ImGui::NewFrame();
  ImGui::Begin("Entities");
  ImGui::End();
  ImGui::Render();
  set_autosave_minutes(7);
  g_tests.check(editor_layout_save(), "the layout and preferences are saved");
  recent_list_add(&recent_projects(), storage.projectFile);
  recent_scenes_add(
      (fs::path(storage.contentRoot) / "main.scene").string().c_str());
  content_browser_state_persist();
  g_tests.check(autosave_begin_session(), "an autosave session begins");

  char layoutPath[1024] = {};
  g_tests.check(editor_layout_path(layoutPath, sizeof(layoutPath)) &&
                    lies_below(layoutPath, saveDir),
                "the layout and preferences are in the per-user save "
                "directory");
  g_tests.check(lies_below(saveDir / "editor_recent_projects.json", saveDir),
                "Recent Projects is in the per-user save directory");
  g_tests.check(lies_below(dataDir / "editor_recent_scenes.json", dataDir),
                "Recent Scenes is in the project's per-user data");
  g_tests.check(lies_below(dataDir / "Autosave" / "session.lock", dataDir),
                "the autosave session marker is in the project's per-user "
                "data");
  g_tests.check(
      lies_below(dataDir / "editor_content_browser_state.json", dataDir),
      "the content browser's state is in the project's per-user data");

  autosave_end_session();
  ImGui::DestroyContext();
  g_tests.check(snapshot(project) == before,
                "the project holds byte for byte what it held before, and "
                "nothing more");

  autosave_reset();
  recent_list_remove(&recent_projects(), storage.projectFile);
  project_hub_reset();
  engine::shutdown();
  fs::remove_all(location, ec);
  return g_tests.finish("editor_user_data");
}
