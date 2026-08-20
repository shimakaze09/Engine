// Verifies engine::run frame-stage fatal propagation (issue #120): an
// injected frame-stage failure must surface as RunResult::FatalFrame with a
// nonzero mapped exit code, and the injection cvar must self-clear.

#include "engine/engine.h"

#include "engine/core/cvar.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

/// Walks up from the launch directory until the asset tree is found.
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
        std::filesystem::exists(normalized / "assets/shaders/default.vert",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

/// One bootstrapped run with the named stage injected to fail.
int check_injected_stage_fatal(const char *stageName) noexcept {
  if (!engine::core::cvar_set_string("dbg_fail_frame_stage", stageName)) {
    std::fprintf(stderr, "failed to arm injection for %s\n", stageName);
    return 30;
  }

  const engine::RunResult result = engine::run(10U);
  if (result != engine::RunResult::FatalFrame) {
    std::fprintf(stderr, "injected %s failure returned %d\n", stageName,
                 static_cast<int>(result));
    return 31;
  }
  if (engine::run_result_exit_code(result) == 0) {
    return 32;
  }

  // The seam consumed the injection: the cvar self-cleared, proving the
  // fatal came from the injected stage rather than another failure site.
  const char *remaining =
      engine::core::cvar_get_string("dbg_fail_frame_stage", "unread");
  if ((remaining == nullptr) || (remaining[0] != '\0')) {
    std::fprintf(stderr, "injection cvar not consumed for %s\n", stageName);
    return 33;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    return 1;
  }

  // Full production bootstrap in headless mode (#196): the platform
  // creates no GL context and bootstrap selects the null render device,
  // so pipeline initialization and the frame stages run on every CI lane
  // instead of gpu-labeled runs only.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    return 2;
  }

  // The injection cvar must be registered by bootstrap before arming.
  if (engine::core::cvar_get_string("dbg_fail_frame_stage", nullptr) ==
      nullptr) {
    engine::shutdown();
    return 3;
  }

  // Control: with the seam disarmed a bounded run stops gracefully, so the
  // fatal results below can only come from the injected failures.
  const engine::RunResult control = engine::run(5U);
  if ((control != engine::RunResult::Stopped) ||
      (engine::run_result_exit_code(control) != 0)) {
    std::fprintf(stderr, "control run returned %d\n",
                 static_cast<int>(control));
    engine::shutdown();
    return 4;
  }

  int result = check_injected_stage_fatal("simulation_graph");
  if (result != 0) {
    engine::shutdown();
    return result;
  }

  result = check_injected_stage_fatal("render_prep_graph");
  if (result != 0) {
    engine::shutdown();
    return result;
  }

  engine::shutdown();
  return 0;
}
