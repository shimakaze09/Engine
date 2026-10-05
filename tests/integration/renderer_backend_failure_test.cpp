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
//
// Regression for #1218: the failure is not only logged. The renderer
// reports why it draws no scene, naming the cooked shader folder it read
// and the target that cooks it, for the editor's views to show in place of
// a black image; the report is clear before the backend tries and after
// shutdown.

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/renderer/command_buffer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
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

  if (engine::renderer::scene_rendering_failure() != nullptr) {
    std::fprintf(stderr, "FAIL: a failure is reported before the backend "
                         "has tried to build\n");
    return 5;
  }
  const engine::RunResult result = engine::run(3U);
  const char *failure = engine::renderer::scene_rendering_failure();
  const bool namesCause =
      (failure != nullptr) &&
      (std::strstr(failure, "default shader program") != nullptr) &&
      (std::strstr(failure, "no_cooked_shaders_here/bgfx/cooked") != nullptr) &&
      (std::strstr(failure, "bgfx_shader_cook") != nullptr);
  std::printf("scene rendering failure: %s\n",
              (failure != nullptr) ? failure : "(none)");
  engine::shutdown();
  if (!namesCause) {
    std::fprintf(stderr,
                 "FAIL: the renderer does not report why it draws no scene, "
                 "naming the cooked folder and the target that cooks it\n");
    return 6;
  }
  if (engine::renderer::scene_rendering_failure() != nullptr) {
    std::fprintf(stderr, "FAIL: shutdown leaves the failure reported\n");
    return 7;
  }

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
