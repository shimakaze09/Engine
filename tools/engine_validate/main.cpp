// Validates authored scenes from the command line: each scene loads
// through the production loader with the assets mount in place, every
// validation finding is printed one per line, and the exit code is
// non-zero when a scene fails to load or reports an Error, so CI catches
// a dangling reference before an author does.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/validation_report.h"
#include "engine/core/vfs.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage: engine_validate [--assets <dir>] <scene.json>...\n");
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
  const char *assetsDirectory = "assets";
  int first = 1;
  if ((argc >= 3) && (std::strcmp(argv[1], "--assets") == 0)) {
    assetsDirectory = argv[2];
    first = 3;
  }
  if (first >= argc) {
    print_usage();
    return 2;
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
  for (int i = first; i < argc; ++i) {
    failures += validate_scene(*world, argv[i]);
  }

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return (failures == 0) ? 0 : 1;
}
