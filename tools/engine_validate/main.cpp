// Validates authored scenes from the command line: the engine's content,
// the project's content and its packages are mounted and catalogued as
// the engine catalogues them at boot, each scene loads through the
// production loader, and every asset reference it carries is checked
// against that catalog. Every finding is printed one per line, and the
// exit code is non-zero when a mount does not index cleanly, a scene
// fails to load, or a scene reports any finding, so CI catches a dangling
// reference before an author does. Given --project, it opens the project
// through engine::open_project and validates every scene the project
// lists.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/command_line.h"
#include "engine/core/logging.h"
#include "engine/core/validation_report.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/project.h"
#include "engine/runtime/content_catalog.h"
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

/// Loads one scene, checks its asset references against `catalog` and
/// prints its findings; returns the number of findings, or 1 when the
/// scene did not load at all. Every finding counts: each is a reference
/// that resolves to nothing, which the runtime survives but an author
/// must fix.
int validate_scene(engine::runtime::World &world,
                   const engine::content::AssetCatalog &catalog,
                   const char *path) {
  engine::core::ValidationReport report{};
  if (!engine::runtime::load_scene(world, path, nullptr, &report)) {
    std::printf("%s: error: scene did not load\n", path);
    return 1;
  }
  engine::runtime::validate_scene_asset_references(world, catalog, &report);
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
  const std::size_t findings = report.count + report.dropped;
  std::printf("%s: %zu finding(s)\n", path, findings);
  return static_cast<int>(findings);
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
  // What the run of this content would mount: the project's own when one
  // is named, the given assets directory otherwise.
  engine::EngineConfig config{};
  config.assetRoot = assetsDirectory;
  if (byProject) {
    const auto opened =
        engine::open_project(commandLine->value("project"), &project, &config);
    if (!opened.has_value()) {
      std::fprintf(stderr, "error: %s: %s\n", commandLine->value("project"),
                   engine::project_open_failure_text(opened.error().kind));
      return 2;
    }
  }

  engine::runtime::ensure_runtime_reflection_registered();
  if (!engine::core::initialize_logging() || !engine::core::initialize_vfs()) {
    std::fprintf(stderr, "error: could not start logging and the VFS\n");
    return 2;
  }
  // Engine content that cannot be found is not fatal (a project need not
  // name any), but its references will not resolve, and that is said once.
  char engineRoot[1024] = {};
  engine::resolve_engine_root(engineRoot, sizeof(engineRoot));
  if (engine::core::os_directory_exists(engineRoot)) {
    config.engineRoot = engineRoot;
  } else {
    std::printf("note: engine content not found at %s (set ENGINE_ROOT); "
                "references to it will not resolve\n",
                engineRoot);
  }
  // Mounted as the engine mounts them, so a path a scene names (a script,
  // a controller) is judged against the same tree.
  bool mounted = (config.engineRoot[0] == '\0') ||
                 engine::core::mount(config.engineMount, config.engineRoot);
  mounted = mounted && engine::core::mount(config.assetMount, config.assetRoot);
  for (std::size_t i = 0U; mounted && (i < config.packageCount); ++i) {
    mounted =
        engine::core::mount(config.packages[i].mount, config.packages[i].root);
  }
  if (!mounted) {
    std::fprintf(stderr, "error: could not mount %s as %s\n", config.assetRoot,
                 config.assetMount);
    return 2;
  }
  // Catalogued as the engine catalogues them at boot, so an identity
  // resolves here exactly as it does in the engine.
  const std::unique_ptr<engine::content::AssetCatalog> catalog =
      engine::runtime::create_asset_catalog();
  if (catalog == nullptr) {
    std::fprintf(stderr, "error: could not allocate an asset catalog\n");
    return 2;
  }
  int failures = 0;
  if (!engine::runtime::catalogue_engine_content(catalog.get(), config)) {
    std::printf("error: a content mount does not index cleanly\n");
    failures = 1;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    std::fprintf(stderr, "error: could not allocate a world\n");
    return 2;
  }

  if (byProject) {
    // The document lists virtual paths under the assets mount; the loader
    // reads OS paths, so each is taken from the content root.
    const std::size_t mountLength = std::strlen("assets/");
    for (std::size_t i = 0U; i < project.document.sceneCount; ++i) {
      char osPath[engine::kProjectOsPathCapacity * 2U] = {};
      std::snprintf(osPath, sizeof(osPath), "%s/%s", project.contentRoot,
                    project.document.scenes[i] + mountLength);
      failures += validate_scene(*world, *catalog, osPath);
    }
  }
  for (std::size_t i = 0U; i < commandLine->positional_count(); ++i) {
    failures += validate_scene(*world, *catalog, commandLine->positional(i));
  }

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return (failures == 0) ? 0 : 1;
}
