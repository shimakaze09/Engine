// Validates authored scenes from the command line: each scene loads
// through the production loader with the assets mount in place, every
// validation finding is printed one per line, and the exit code is
// non-zero when a scene fails to load or reports an Error, so CI catches
// a dangling reference before an author does. Given --project, it opens
// the project through engine::open_project and validates every scene the
// project lists, with the project's content root mounted.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/command_line.h"
#include "engine/core/logging.h"
#include "engine/core/validation_report.h"
#include "engine/core/vfs.h"
#include "engine/project.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr engine::core::CommandLineOption kOptions[] = {
    {"assets", engine::core::CommandLineOptionKind::Value},
    {"project", engine::core::CommandLineOptionKind::Value},
};

void print_usage() {
  std::fprintf(stderr,
               "usage: engine_validate [--assets <dir>] <scene.json>...\n"
               "       engine_validate --project <dir or .project> "
               "[<scene.json>...]\n");
}

/// Loads one scene and prints its findings; returns the number of Error
/// findings, or 1 when the scene did not load at all.
int validate_scene(engine::runtime::World &world, const char *path) {
  engine::core::ValidationReport report{};
  if (!engine::runtime::load_scene(world, path, nullptr, &report)) {
    std::printf("%s: error: scene did not load\n", path);
    return 1;
  }
  for (std::size_t i = 0U; i < report.count; ++i) {
    const engine::core::ValidationEntry &entry = report.entries[i];
    std::printf("%s: %s: %s %s (entity %u)\n", path,
                (entry.severity == engine::core::ValidationSeverity::Error)
                    ? "error"
                    : "warning",
                entry.code, entry.key, entry.entityPersistentId);
  }
  if (report.dropped > 0U) {
    std::printf("%s: warning: %zu further finding(s) not listed\n", path,
                report.dropped);
  }
  const std::size_t errors =
      report.count_of(engine::core::ValidationSeverity::Error);
  std::printf("%s: %zu warning(s), %zu error(s)\n", path,
              report.count_of(engine::core::ValidationSeverity::Warning),
              errors);
  return static_cast<int>(errors);
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  const auto commandLine = engine::core::parse_command_line(
      argc, argv, kOptions, sizeof(kOptions) / sizeof(kOptions[0]),
      engine::core::kMaxCommandLinePositionals);
  if (!commandLine.has_value()) {
    const engine::core::CommandLineFailure failure = commandLine.error();
    std::fprintf(stderr, "error: %s: %s\n",
                 (failure.argumentIndex > 0) ? argv[failure.argumentIndex]
                                             : "engine_validate",
                 engine::core::command_line_failure_text(failure.kind));
    print_usage();
    return 2;
  }
  const bool byProject = commandLine->has("project");
  if ((byProject && commandLine->has("assets")) ||
      (!byProject && (commandLine->positional_count() == 0U))) {
    print_usage();
    return 2;
  }
  const char *assetsDirectory =
      commandLine->has("assets") ? commandLine->value("assets") : "assets";
  // Static: about 18 KB, and it must outlive the mount that points at it.
  static engine::ProjectStorage project{};
  if (byProject) {
    engine::EngineConfig config{};
    const auto opened =
        engine::open_project(commandLine->value("project"), &project, &config);
    if (!opened.has_value()) {
      std::fprintf(stderr, "error: %s: %s\n", commandLine->value("project"),
                   engine::project_open_failure_text(opened.error().kind));
      return 2;
    }
    assetsDirectory = project.contentRoot;
  }

  engine::runtime::ensure_runtime_reflection_registered();
  if (!engine::core::initialize_logging() || !engine::core::initialize_vfs() ||
      !engine::core::mount("assets", assetsDirectory)) {
    std::fprintf(stderr, "error: could not mount %s as assets\n",
                 assetsDirectory);
    return 2;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    std::fprintf(stderr, "error: could not allocate a world\n");
    return 2;
  }

  int failures = 0;
  if (byProject) {
    // The document lists virtual paths under the assets mount; the loader
    // reads OS paths, so each is taken from the content root.
    const std::size_t mountLength = std::strlen("assets/");
    for (std::size_t i = 0U; i < project.document.sceneCount; ++i) {
      char osPath[engine::kProjectOsPathCapacity * 2U] = {};
      std::snprintf(osPath, sizeof(osPath), "%s/%s", project.contentRoot,
                    project.document.scenes[i] + mountLength);
      failures += validate_scene(*world, osPath);
    }
  }
  for (std::size_t i = 0U; i < commandLine->positional_count(); ++i) {
    failures += validate_scene(*world, commandLine->positional(i));
  }

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return (failures == 0) ? 0 : 1;
}
