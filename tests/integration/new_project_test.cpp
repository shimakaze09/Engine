// Verifies create_project makes a project from the engine's empty-project
// template, whole or not at all: the new directory holds the template's
// content and a "<name>.project" with a fresh GUID, open_project accepts
// it, and it bootstraps and runs headless with no Error, none on any
// channel naming one of its files (the engine loads nothing from a
// project by name). Names a project
// document refuses, an existing destination, a missing location and a
// template that is not one are refused with nothing written; a template
// holding a file too large to copy fails part-way and leaves nothing at
// the destination and no staging directory; a stage an interrupted
// attempt left behind is replaced.

#include "../asset_root.h"
#include "engine/content/project_document.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/project.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_errors = 0;

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

/// The new project's directory while it runs; an Error on any channel
/// naming a file under it is the project's.
std::string g_projectDirectory;
int g_projectFileErrors = 0;

/// Counts every Error from what creating and running a project involves,
/// and every Error naming one of the project's files.
void count_errors(engine::core::LogLevel level, const char *channel,
                  const char *message, void *) noexcept {
  if (level != engine::core::LogLevel::Error) {
    return;
  }
  if (is_project_channel(channel)) {
    ++g_errors;
  }
  if (!g_projectDirectory.empty() && (message != nullptr) &&
      (std::strstr(message, g_projectDirectory.c_str()) != nullptr)) {
    ++g_projectFileErrors;
  }
}

/// True when `directory` holds nothing at all.
bool is_empty_directory(const fs::path &directory) {
  std::error_code ec{};
  return fs::is_directory(directory, ec) && fs::is_empty(directory, ec) && !ec;
}

/// Creates `name` in `location` from `templateDir`, expecting `expected`,
/// and checks a refusal wrote nothing into `location`.
void expect_refused(const fs::path &location, const char *name,
                    const std::string &templateDir,
                    engine::ProjectCreateFailureKind expected,
                    const char *what) {
  char out[engine::kProjectOsPathCapacity] = {'x', '\0'};
  const auto created = engine::create_project(
      location.string().c_str(), name, templateDir.c_str(), out, sizeof(out));
  CHECK(!created.has_value() && (created.error() == expected), what);
  CHECK(out[0] == '\0', "a refusal writes no project path");
}

} // namespace

/// Runs this test program.
int main() {
  const std::string engineRoot = engine::tests::engine_root_path();
  // Logging drops messages until it is initialized, and create_project is
  // called here before any bootstrap has started it.
  if (engineRoot.empty() || !engine::tests::enter_asset_root() ||
      !engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_errors, nullptr)) {
    std::fprintf(stderr, "FAIL: the engine's content was not found\n");
    return 1;
  }
  const std::string templateDir =
      (fs::path(engineRoot) / "templates~/empty_project").string();
  std::error_code ec{};
  const fs::path scratch =
      fs::temp_directory_path(ec) / "engine_new_project_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  if (ec) {
    return 2;
  }

  // A project created from the shipped template opens and runs.
  char projectFile[engine::kProjectOsPathCapacity] = {};
  const auto created = engine::create_project(scratch.string().c_str(),
                                              "My Game", templateDir.c_str(),
                                              projectFile, sizeof(projectFile));
  CHECK(created.has_value(), "a project is created from the template");
  const fs::path project = scratch / "My Game";
  CHECK(fs::path(projectFile) == (project / "My Game.project"),
        "the path given back is the new .project file");
  CHECK(fs::is_regular_file(project / "assets/main.scene", ec) &&
            fs::is_regular_file(project / "assets/main.scene.meta", ec) &&
            fs::is_regular_file(project / "assets/main.lua", ec) &&
            fs::is_regular_file(project / ".gitignore", ec),
        "it holds the template's content and sidecars");
  CHECK(!fs::exists(scratch / ".My Game.creating", ec),
        "and no staging directory is left");
  engine::content::ProjectDocument document{};
  CHECK(engine::content::read_project_document(projectFile, &document)
                .has_value() &&
            (std::strcmp(document.name, "My Game") == 0) &&
            (std::strcmp(document.startupScene, "assets/main.scene") == 0) &&
            (std::strcmp(document.mainScript, "assets/main.lua") == 0) &&
            engine::core::asset_guid_is_valid(document.guid),
        "its document names it, its startup scene and main script, and has "
        "a GUID");

  static engine::ProjectStorage storage{};
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  g_errors = 0;
  g_projectFileErrors = 0;
  g_projectDirectory = project.generic_string();
  CHECK(engine::open_project(projectFile, &storage, &config).has_value(),
        "open_project accepts it");
  CHECK(engine::bootstrap(config), "it bootstraps");
  CHECK(engine::core::vfs_file_exists("assets/main.lua"),
        "with its content mounted");
  CHECK(engine::run(10U) == engine::RunResult::Stopped, "and runs");
  engine::shutdown();
  CHECK(g_errors == 0, "no Error is logged creating and running it");
  // The engine loads nothing from a project by name, so a project without
  // the sample's files runs without an Error about them.
  CHECK(g_projectFileErrors == 0, "and no Error names a file of the project");
  g_projectDirectory.clear();
  // Shutdown stops logging and forgets its sinks.
  static_cast<void>(engine::core::initialize_logging());
  static_cast<void>(engine::core::log_register_sink(&count_errors, nullptr));

  // A second project gets its own identity.
  char second[engine::kProjectOsPathCapacity] = {};
  engine::content::ProjectDocument secondDocument{};
  CHECK(engine::create_project(scratch.string().c_str(), "Other",
                               templateDir.c_str(), second, sizeof(second))
                .has_value() &&
            engine::content::read_project_document(second, &secondDocument)
                .has_value() &&
            !(secondDocument.guid == document.guid),
        "each project gets a GUID of its own");

  // Refusals write nothing.
  const fs::path empty = scratch / "empty";
  fs::create_directories(empty, ec);
  expect_refused(empty, "", templateDir,
                 engine::ProjectCreateFailureKind::InvalidName,
                 "an empty name is refused");
  expect_refused(empty, "a/b", templateDir,
                 engine::ProjectCreateFailureKind::InvalidName,
                 "a name with a separator is refused");
  expect_refused(empty, "trailing.", templateDir,
                 engine::ProjectCreateFailureKind::InvalidName,
                 "a name a filesystem refuses is refused");
  expect_refused(
      empty, std::string(engine::content::kProjectNameCapacity, 'n').c_str(),
      templateDir, engine::ProjectCreateFailureKind::InvalidName,
      "a name too long for the document is refused");
  expect_refused(empty, "Game", (scratch / "no_template").string(),
                 engine::ProjectCreateFailureKind::TemplateUnusable,
                 "a template that is not there is refused");
  expect_refused(empty / "nowhere", "Game", templateDir,
                 engine::ProjectCreateFailureKind::LocationMissing,
                 "a location that is not there is refused");
  CHECK(is_empty_directory(empty), "no refusal wrote anything");
  expect_refused(scratch, "My Game", templateDir,
                 engine::ProjectCreateFailureKind::AlreadyExists,
                 "an existing project's name is refused");
  CHECK(fs::is_regular_file(project / "My Game.project", ec),
        "and the existing project is untouched");

  // A template whose copy fails part-way leaves nothing behind.
  const fs::path broken = scratch / "broken_template";
  fs::create_directories(broken / "assets", ec);
  fs::copy(fs::path(templateDir) / "assets", broken / "assets",
           fs::copy_options::recursive, ec);
  {
    std::ofstream big(broken / "assets/zz_big.bin", std::ios::binary);
    const std::string block(64U * 1024U, 'b');
    for (std::size_t written = 0U;
         written <= engine::kMaxProjectTemplateFileBytes;
         written += block.size()) {
      big.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
  }
  expect_refused(empty, "Game", broken.string(),
                 engine::ProjectCreateFailureKind::TemplateUnusable,
                 "a template file too large to copy fails the creation");
  CHECK(is_empty_directory(empty),
        "leaving no project and no staging directory");

  // A stage an interrupted attempt left behind is replaced.
  fs::create_directories(empty / ".Game.creating/leftover", ec);
  char resumed[engine::kProjectOsPathCapacity] = {};
  CHECK(engine::create_project(empty.string().c_str(), "Game",
                               templateDir.c_str(), resumed, sizeof(resumed))
                .has_value() &&
            !fs::exists(empty / ".Game.creating", ec) &&
            !fs::exists(empty / "Game/leftover", ec),
        "a leftover stage is replaced, not merged");

  engine::core::log_unregister_sink(&count_errors, nullptr);
  fs::remove_all(scratch, ec);
  if (g_failures == 0) {
    std::printf("new_project: all checks passed\n");
    return 0;
  }
  return 1;
}
