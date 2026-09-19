// Regression for #578: when the renderer's lazily built backend fails after
// bootstrap opened the render device (here through a shader root that
// holds no cooked shaders), the device stays up for its owner, the run
// completes its frames with the failure logged, and shutdown releases the
// device once — instead of the backend tearing the device down under the
// editor overlay, which then submitted to a bgfx that was gone. The ctest
// registration runs this on SDL's dummy video driver: a window with no
// native handle selects bgfx's Noop renderer, so the windowed bootstrap
// and the editor bridge come up with no display attached. Base segfaults
// inside ImGui_ImplBgfx_RenderDrawData on the first frame.

#include "engine/engine.h"

#include <cstdio>
#include <filesystem>

namespace {

/// Walks upward from the current path until the bundled assets are found.
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: bundled assets not found\n");
    return 1;
  }

  engine::EngineConfig config{};
  // Deliberately absent: the device initializes at bootstrap, and the
  // backend's first flush then fails to load its default program.
  config.shaderRootPath = "tests/data/no_cooked_shaders_here";
  std::error_code ec{};
  if (std::filesystem::exists(config.shaderRootPath, ec)) {
    std::fprintf(stderr, "FAIL: the absent shader root exists\n");
    return 2;
  }

  if (!engine::bootstrap(config)) {
    std::fprintf(stderr,
                 "FAIL: bootstrap refused; the backend failure this test "
                 "drives happens after bootstrap, so this is a different "
                 "failure\n");
    return 3;
  }

  const engine::RunResult result = engine::run(3U);
  engine::shutdown();

  if (result != engine::RunResult::Stopped) {
    std::fprintf(stderr,
                 "FAIL: run returned %d; a backend that failed to build must "
                 "leave the run able to finish its frames\n",
                 static_cast<int>(result));
    return 4;
  }
  std::printf("renderer backend failure left the device to its owner; the "
              "run finished and shut down cleanly\n");
  return 0;
}
