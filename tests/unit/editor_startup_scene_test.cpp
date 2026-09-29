// Verifies the editor opens a project on its startup scene: a session
// started inside the sample project opens assets/main.scene into the
// bound world as the document, named by its file, once. A session with a
// document already open keeps it; a startup scene that does not open
// leaves the built-in world untitled with a Warning naming it; and a run
// with no project arms nothing.

#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "../asset_root.h"
#include "../test_harness.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/world.h"

namespace {

namespace fs = std::filesystem;
using namespace engine::editor;

engine::tests::TestContext g_tests;
int g_startupWarnings = 0;

void count_startup_warnings(engine::core::LogLevel level, const char *,
                            const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "startup scene") != nullptr)) {
    ++g_startupWarnings;
  }
}

/// Bootstraps headless with `config`, binds a fresh world, arms the
/// startup scene and opens it. Returns the world, still bound.
std::unique_ptr<engine::runtime::World>
start_session(const engine::EngineConfig &config) {
  if (!engine::bootstrap(config)) {
    return nullptr;
  }
  static_cast<void>(
      engine::core::log_register_sink(&count_startup_warnings, nullptr));
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  editor_set_world(world.get());
  scene_document_arm_startup_scene();
  return world;
}

void end_session() {
  editor_set_world(nullptr);
  engine::shutdown();
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root()) {
    return 2;
  }
  // The functions under test need the engine running, not the editor's
  // device objects, which a headless run has no device for.
  engine::runtime::set_editor_bridge(nullptr);
  std::error_code ec{};
  const fs::path scratch =
      fs::temp_directory_path(ec) / "engine_editor_startup_scene_test";
  fs::create_directories(scratch, ec);
  recent_scenes_set_directory_override_for_tests(scratch.string().c_str());

  const std::string sample = engine::tests::sample_project_path();
  static engine::ProjectStorage storage{};
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (sample.empty() ||
      !engine::open_project(sample.c_str(), &storage, &config).has_value()) {
    return 3;
  }

  // A project opens on its startup scene, named by its file, once.
  {
    auto world = start_session(config);
    g_tests.check(world != nullptr, "a session starts in the sample");
    g_tests.check(!scene_document_has_path(),
                  "nothing is open before the first frame");
    scene_document_open_startup_scene();
    const fs::path expected = fs::path(storage.contentRoot) / "main.scene";
    g_tests.check(scene_document_has_path() &&
                      fs::equivalent(scene_document_path(), expected, ec),
                  "the startup scene is the document, by its file");
    g_tests.check(std::strcmp(scene_document_display_name(), "main.scene") ==
                          0 &&
                      !scene_document_is_dirty(),
                  "named main.scene, and clean");
    g_tests.check((world != nullptr) &&
                      (world->find_entity_by_name("Main Camera") !=
                       engine::runtime::kInvalidEntity),
                  "with its entities in the world");
    const std::size_t loaded =
        (world != nullptr) ? world->alive_entity_count() : 0U;
    scene_document_open_startup_scene();
    g_tests.check((world != nullptr) && (world->alive_entity_count() == loaded),
                  "and only once");
    end_session();
  }

  // A document already open is kept.
  {
    auto world = start_session(config);
    const std::string other =
        (fs::path(storage.contentRoot) / "coin_run.scene").string();
    g_tests.check(perform_scene_open(other.c_str()),
                  "another scene is opened first");
    scene_document_open_startup_scene();
    g_tests.check(fs::equivalent(scene_document_path(), other, ec),
                  "and stays the document");
    end_session();
  }

  // A startup scene that does not open leaves the built-in world.
  {
    engine::EngineConfig broken = config;
    broken.editorScenePath = "assets/no_such.scene";
    g_startupWarnings = 0;
    auto world = start_session(broken);
    scene_document_open_startup_scene();
    scene_document_open_startup_scene();
    g_tests.check(!scene_document_has_path() && (g_startupWarnings == 1),
                  "a startup scene that does not open leaves the document "
                  "untitled, with one Warning, and is not retried");
    end_session();
  }

  // No project, nothing armed.
  {
    engine::EngineConfig none{};
    none.core.platform.headless = true;
    engine::configure_without_project(&none);
    g_startupWarnings = 0;
    auto world = start_session(none);
    scene_document_open_startup_scene();
    g_tests.check(!scene_document_has_path() && (g_startupWarnings == 0),
                  "with no project nothing is opened or reported");
    end_session();
  }

  recent_scenes_set_directory_override_for_tests("");
  fs::remove_all(scratch, ec);
  return g_tests.finish("editor_startup_scene");
}
