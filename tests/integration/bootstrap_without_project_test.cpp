// Verifies the engine runs with no project open, as the project hub does:
// a config from configure_without_project bootstraps headless with only
// the engine's content mounted (nothing at assets/), builds the startup
// world with a Scene Controller that runs no script, and runs frames
// without logging an Error. A config with no asset root that still names
// a main script, a startup scene or an editor asset root is refused by
// name. The project-switch handoff carries a request out of a run (and
// ends it), is taken once, and refuses a path that does not fit. After
// all of that the same process opens the sample project and runs it,
// with its content mounted and its .project file in the active config.

#include "../asset_root.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/world.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_errors = 0;
bool g_namedField = false;
engine::runtime::World *g_world = nullptr;

/// True for a channel whose Errors this test owns: the engine, the
/// project, its scenes, scripts and assets. A lane built without cooked
/// shaders logs the renderer's missing programs, which say nothing about
/// projects.
bool is_project_channel(const char *channel) noexcept {
  static constexpr const char *kChannels[] = {"engine", "project", "scene",
                                              "scripting", "assets"};
  for (const char *owned : kChannels) {
    if ((channel != nullptr) && (std::strcmp(channel, owned) == 0)) {
      return true;
    }
  }
  return false;
}

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Counts every Error from what running a project involves, and notes
/// one naming the refused field.
void count_errors(engine::core::LogLevel level, const char *channel,
                  const char *message, void *) noexcept {
  if ((level != engine::core::LogLevel::Error) ||
      !is_project_channel(channel)) {
    return;
  }
  ++g_errors;
  if ((message != nullptr) &&
      (std::strstr(message, "'mainScriptPath' but no project") != nullptr)) {
    g_namedField = true;
  }
}

/// What a frame saw of the startup world.
struct WorldSeen final {
  bool seen = false;
  std::size_t entities = 0U;
  bool camera = false;
  bool controller = false;
  bool controllerScript = false;
};
WorldSeen g_seen{};

/// Captures the pipeline's world while it lives.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

/// Looks at the world once a frame has begun.
void inspect_world() noexcept {
  if ((g_world == nullptr) || g_seen.seen) {
    return;
  }
  g_seen.seen = true;
  g_seen.entities = g_world->alive_entity_count();
  g_seen.camera = g_world->find_entity_by_name("Main Camera") !=
                  engine::runtime::kInvalidEntity;
  const engine::runtime::Entity controller =
      g_world->find_entity_by_name("Scene Controller");
  g_seen.controller = controller != engine::runtime::kInvalidEntity;
  engine::runtime::ScriptComponent script{};
  g_seen.controllerScript =
      g_seen.controller && g_world->get_script_component(controller, &script);
}

/// Logging drops messages until it is initialized and forgets its sinks
/// at every shutdown, so each phase starts it and registers again.
void watch_errors() noexcept {
  static_cast<void>(engine::core::initialize_logging());
  static_cast<void>(engine::core::log_register_sink(&count_errors, nullptr));
  g_errors = 0;
}

/// A headless config with no project.
engine::EngineConfig projectless_config() noexcept {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  engine::configure_without_project(&config);
  return config;
}

/// Bootstrap and run with no project: only engine/ is mounted, the world
/// is the startup template without a script, and no Error is logged.
void check_runs_without_project() {
  watch_errors();
  g_world = nullptr;
  g_seen = WorldSeen{};
  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.new_frame = &inspect_world;
  engine::runtime::set_editor_bridge(&bridge);
  const engine::EngineConfig config = projectless_config();
  CHECK(engine::bootstrap(config), "a config with no project bootstraps");
  CHECK(!engine::has_open_project(), "and runs no project");
  CHECK(engine::active_config().projectFile[0] == '\0',
        "with no .project file");
  CHECK(engine::core::vfs_file_exists("engine/shaders/bgfx/shaders.manifest"),
        "the engine's content is mounted");
  CHECK(!engine::core::vfs_directory_exists("assets"),
        "and nothing is mounted at assets/");
  CHECK(engine::run(30U) == engine::RunResult::Stopped, "it runs 30 frames");
  CHECK(g_seen.seen, "a frame saw the pipeline's world");
  CHECK((g_seen.entities == 3U) && g_seen.camera && g_seen.controller,
        "the world is the startup template");
  CHECK(!g_seen.controllerScript, "whose Scene Controller runs no script");
  engine::shutdown();
  engine::runtime::set_editor_bridge(nullptr);
  CHECK(g_errors == 0, "no Error is logged running without a project");
}

/// A config with no asset root that names project content is refused.
void check_inconsistent_config_refused() {
  engine::EngineConfig config = projectless_config();
  config.mainScriptPath = "assets/main.lua";
  watch_errors();
  g_namedField = false;
  CHECK(!engine::bootstrap(config), "no project with a main script is refused");
  CHECK(g_namedField, "and the refusal names the field");
  CHECK(!engine::is_bootstrapped(), "leaving nothing running");

  config = projectless_config();
  config.editorScenePath = "assets/main.scene";
  CHECK(!engine::bootstrap(config), "no project with a startup scene too");
  config = projectless_config();
  config.editorAssetRoot = "assets";
  CHECK(!engine::bootstrap(config), "no project with an editor asset root too");
}

/// The switch handoff: requested in a run (which ends it), taken once.
void check_project_switch() {
  char taken[engine::kProjectOsPathCapacity] = {};
  bool toHub = true;
  CHECK(!engine::take_project_switch(taken, sizeof(taken), &toHub) &&
            (taken[0] == '\0'),
        "with nothing requested there is nothing to take");

  const engine::EngineConfig config = projectless_config();
  CHECK(engine::bootstrap(config), "bootstrap for the switch run");
  CHECK(engine::request_project_switch("somewhere/game"),
        "a switch is requested");
  // Ended by the request's quit: a run that ignored it would reach the
  // frame budget instead, which this cannot tell apart, so the platform's
  // own state is checked too.
  CHECK(!engine::core::is_platform_running(),
        "the request asks the platform to quit");
  CHECK(engine::run(600U) == engine::RunResult::Stopped, "the run ends");
  engine::shutdown();

  char tiny[4] = {};
  CHECK(!engine::take_project_switch(tiny, sizeof(tiny), &toHub),
        "a buffer too small takes nothing");
  CHECK(engine::take_project_switch(taken, sizeof(taken), &toHub) &&
            (std::strcmp(taken, "somewhere/game") == 0) && !toHub,
        "and leaves it pending for one that fits");
  CHECK(!engine::take_project_switch(taken, sizeof(taken), &toHub),
        "a switch is taken once");

  CHECK(engine::request_project_switch("somewhere/first") &&
            engine::request_project_switch(""),
        "a later request replaces an earlier one");
  CHECK(engine::take_project_switch(taken, sizeof(taken), &toHub) &&
            (taken[0] == '\0') && toHub,
        "an empty path goes back to the hub");

  const std::string overlong(engine::kProjectOsPathCapacity, 'x');
  CHECK(!engine::request_project_switch(overlong.c_str()) &&
            !engine::request_project_switch(nullptr),
        "an overlong or null path is refused");
  CHECK(!engine::take_project_switch(taken, sizeof(taken), &toHub),
        "and requests nothing");
}

/// The same process then opens and runs the sample project.
void check_then_opens_sample() {
  const std::string sample = engine::tests::sample_project_path();
  static engine::ProjectStorage storage{};
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  CHECK(!sample.empty() &&
            engine::open_project(sample.c_str(), &storage, &config).has_value(),
        "the sample project opens");
  watch_errors();
  CHECK(engine::bootstrap(config), "and bootstraps after the projectless runs");
  CHECK(engine::has_open_project(), "with a project open");
  CHECK(std::strcmp(engine::active_config().projectFile, storage.projectFile) ==
            0,
        "whose .project file the active config keeps");
  CHECK(engine::core::vfs_file_exists("assets/main.scene"),
        "and whose content is mounted at assets/");
  CHECK(engine::run(10U) == engine::RunResult::Stopped, "it runs");
  engine::shutdown();
  CHECK(g_errors == 0, "no Error is logged running the sample after");
}

} // namespace

/// Runs this test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: the sample project was not found\n");
    return 1;
  }
  check_runs_without_project();
  check_inconsistent_config_refused();
  check_project_switch();
  check_then_opens_sample();

  if (g_failures == 0) {
    std::printf("bootstrap_without_project: all checks passed\n");
    return 0;
  }
  return 1;
}
