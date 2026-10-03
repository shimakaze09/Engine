// Verifies Recent Scenes keeps one entry per scene file and per project
// (#1216): a scene inside the open project's content root is stored by its
// path below it ("samples/playground.scene"), so every spelling of one file
// is one entry and two scenes of one name stay apart; a scene outside the
// root keeps its absolute path; an entry resolves back to the file it
// names; and each project reads only its own list, from its per-user data
// directory.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "../asset_root.h"
#include "../test_harness.h"
#include "editor_scene_document.h"
#include "engine/core/project_data.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/runtime/editor_bridge.h"

namespace {

namespace fs = std::filesystem;
using namespace engine::editor;

engine::tests::TestContext g_tests;

/// Writes an empty scene document at `path`; false on failure.
bool write_scene(const fs::path &path) {
  std::error_code ec{};
  fs::create_directories(path.parent_path(), ec);
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool written = std::fputs("{}", file) >= 0;
  return (std::fclose(file) == 0) && written;
}

/// The Recent Scenes entry for `path`, or "<none>".
std::string entry_of(const std::string &path) {
  char entry[kMaxRecentPathLength] = {};
  return recent_scene_entry(path.c_str(), entry, sizeof(entry))
             ? std::string(entry)
             : std::string("<none>");
}

/// True when the entries at `first` and `second` differ.
bool entries_differ(std::size_t first, std::size_t second) {
  return std::strcmp(recent_scene_at(first), recent_scene_at(second)) != 0;
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root()) {
    return 2;
  }
  engine::runtime::set_editor_bridge(nullptr);
  std::error_code ec{};
  const fs::path scratch =
      fs::temp_directory_path(ec) / "engine_editor_recent_scenes_test";
  fs::remove_all(scratch, ec);
  const fs::path outsideA = scratch / "elsewhere" / "a" / "main.scene";
  const fs::path outsideB = scratch / "elsewhere" / "b" / "main.scene";
  if (!write_scene(outsideA) || !write_scene(outsideB)) {
    return 3;
  }
  recent_scenes_set_directory_override_for_tests(
      (scratch / "lists").string().c_str());

  const std::string sample = engine::tests::sample_project_path();
  static engine::ProjectStorage storage{};
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (sample.empty() ||
      !engine::open_project(sample.c_str(), &storage, &config).has_value() ||
      !engine::bootstrap(config)) {
    return 4;
  }
  const std::string root = storage.contentRoot;
  const std::string playground = root + "/samples/playground.scene";
  const std::string mainScene = root + "/main.scene";

  // One file, one entry: the path below the content root, generic form.
  g_tests.check(entry_of(playground) == "samples/playground.scene",
                "a scene in the project is named by its path below the "
                "content root");
  g_tests.check(entry_of(root + "/samples/../samples/./playground.scene") ==
                    "samples/playground.scene",
                "a spelling with . and .. names the same entry");
#ifdef _WIN32
  std::string backslashed = playground;
  for (char &c : backslashed) {
    if (c == '/') {
      c = '\\';
    }
  }
  g_tests.check(entry_of(backslashed) == "samples/playground.scene",
                "a backslash spelling names the same entry");
#endif
  const std::string outsideEntry =
      fs::weakly_canonical(outsideA, ec).lexically_normal().generic_string();
  g_tests.check(entry_of(outsideA.string()) == outsideEntry,
                "a scene outside the content root keeps its absolute path");

  // Adding one file through several spellings keeps one entry.
  recent_scenes_add(playground.c_str());
  recent_scenes_add((root + "/samples/../samples/./playground.scene").c_str());
#ifdef _WIN32
  recent_scenes_add(backslashed.c_str());
#endif
  g_tests.check(
      (recent_scene_count() == 1U) &&
          (std::strcmp(recent_scene_at(0U), "samples/playground.scene") == 0),
      "every spelling of one scene is one entry");
  char resolved[kMaxDocumentPathLength] = {};
  g_tests.check(
      recent_scene_os_path(recent_scene_at(0U), resolved, sizeof(resolved)) &&
          fs::equivalent(resolved, playground, ec),
      "an entry resolves back to the file it names");

  // Three scenes named main.scene are three distinct entries, so their
  // menu labels differ.
  recent_scenes_add(mainScene.c_str());
  recent_scenes_add(outsideA.string().c_str());
  recent_scenes_add(outsideB.string().c_str());
  g_tests.check((recent_scene_count() == 4U) && entries_differ(0U, 1U) &&
                    entries_differ(0U, 2U) && entries_differ(1U, 2U),
                "scenes of one file name in different folders stay apart");
  g_tests.check(std::strcmp(recent_scene_at(2U), "main.scene") == 0,
                "the project's main.scene is labelled by its project path");

  // A failed open drops the entry by any spelling of the path.
  recent_scenes_remove((root + "/./main.scene").c_str());
  g_tests.check(recent_scene_count() == 3U,
                "removing by another spelling drops the entry");

  // The list survives a restart.
  recent_scenes_forget();
  g_tests.check(recent_scene_count() == 3U, "the stored list reads back");

  // Each project reads only its own list, through the production directory.
  recent_scenes_set_directory_override_for_tests("");
  const fs::path projectA = scratch / "project_a";
  const fs::path projectB = scratch / "project_b";
  fs::create_directories(projectA, ec);
  fs::create_directories(projectB, ec);
  g_tests.check(engine::core::set_project_data_root(projectA.string().c_str()),
                "project A is open");
  recent_scenes_forget();
  g_tests.check(recent_scene_count() == 0U, "project A starts empty");
  recent_scenes_add(playground.c_str());
  g_tests.check(engine::core::set_project_data_root(projectB.string().c_str()),
                "project B is open");
  recent_scenes_forget();
  g_tests.check(recent_scene_count() == 0U,
                "project B does not see project A's scenes");
  g_tests.check(engine::core::set_project_data_root(projectA.string().c_str()),
                "project A is open again");
  recent_scenes_forget();
  g_tests.check(
      (recent_scene_count() == 1U) &&
          (std::strcmp(recent_scene_at(0U), "samples/playground.scene") == 0),
      "project A reads its own list back");

  char dataDirectory[1024] = {};
  if (engine::core::project_data_dir(dataDirectory, sizeof(dataDirectory))) {
    fs::remove(fs::path(dataDirectory) / "editor_recent_scenes.json", ec);
  }
  recent_scenes_forget();
  engine::shutdown();
  fs::remove_all(scratch, ec);
  return g_tests.finish("editor_recent_scenes");
}
