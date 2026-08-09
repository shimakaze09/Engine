// Host for the runtime cooked-asset staleness regression: loads a cooked
// mesh twice through the production loader so the driving CMake script can
// assert the once-per-asset staleness warning on stdout (issue #81).

#include <cstdio>

#include "engine/core/logging.h"
#include "engine/renderer/mesh_loader.h"

/// Runs this executable or test program.
int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: asset_stale_load_host <cooked.mesh>\n");
    return 1;
  }

  if (!engine::core::initialize_logging()) {
    return 2;
  }

  engine::renderer::CpuMeshData first{};
  if (!engine::renderer::load_mesh_data_from_file(argv[1], &first)) {
    std::fprintf(stderr, "error: first mesh load failed: %s\n", argv[1]);
    return 3;
  }

  engine::renderer::CpuMeshData second{};
  if (!engine::renderer::load_mesh_data_from_file(argv[1], &second)) {
    std::fprintf(stderr, "error: second mesh load failed: %s\n", argv[1]);
    return 4;
  }

  std::printf("LOAD_RESULT ok ok\n");
  engine::core::shutdown_logging();
  return 0;
}
