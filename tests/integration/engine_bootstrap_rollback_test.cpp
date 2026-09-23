// Integration tests for the engine-tier bootstrap rollback: an injected
// failure at the scripting, audio or texture stage closes every stage
// already opened (the editor bridge's shutdown pairs with its initialize),
// leaves the active configuration at its defaults and the editor bridge
// registered, and a retry bootstraps cleanly; player mode hands the
// displaced bridge back on shutdown so an editor bootstrap in the same
// process still finds it; run() without a bootstrap is refused; shutdown
// is idempotent; exit codes are distinct.

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/job_system.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"

namespace {

int g_failures = 0;
int g_bridgeInitialized = 0;
int g_bridgeShutdown = 0;

void check(bool condition, const char *name) noexcept {
  if (!condition) {
    std::printf("FAIL: %s\n", name);
    ++g_failures;
  }
}

bool bridge_initialize(void *) noexcept {
  ++g_bridgeInitialized;
  return true;
}
void bridge_shutdown() noexcept { ++g_bridgeShutdown; }

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
    if (std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

engine::EngineConfig headless_config(bool playerMode) noexcept {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.playerMode = playerMode;
  return config;
}

/// A bootstrap that fails at `stage` closes what it opened and leaves the
/// process ready for a retry that succeeds.
void check_failed_stage_rolls_back(engine::BootstrapStage stage,
                                   const engine::runtime::EditorBridge *bridge,
                                   const char *name) noexcept {
  const int initializedBefore = g_bridgeInitialized;
  const int shutdownBefore = g_bridgeShutdown;
  engine::EngineConfig config = headless_config(false);
  config.mainScriptPath = "assets/probe_main.lua";

  engine::inject_bootstrap_failure(stage);
  const bool booted = engine::bootstrap(config);
  std::printf("%s\n", name);
  check(!booted, "injected stage failure fails the bootstrap");
  check(!engine::is_bootstrapped(), "a failed bootstrap is not bootstrapped");
  check(!engine::core::is_core_initialized(),
        "a failed bootstrap closes core again");
  check(std::strcmp(engine::active_config().mainScriptPath,
                    "assets/main.lua") == 0,
        "a failed bootstrap returns the active configuration to defaults");
  check(engine::runtime::editor_bridge() == bridge,
        "a failed bootstrap leaves the editor bridge registered");
  check((g_bridgeInitialized - initializedBefore) ==
            (g_bridgeShutdown - shutdownBefore),
        "the bridge's shutdown pairs with its initialize on failure");
  check(engine::run(1U) == engine::RunResult::FatalInitialization,
        "run without a bootstrap is refused");

  check(engine::bootstrap(config), "the retry bootstraps");
  check(engine::is_bootstrapped(), "the retry is bootstrapped");
  check(std::strcmp(engine::active_config().mainScriptPath,
                    "assets/probe_main.lua") == 0,
        "the retry adopts its own configuration");
  engine::shutdown();
  check(!engine::is_bootstrapped(), "shutdown clears the bootstrap");
  check(!engine::core::is_core_initialized(), "shutdown closes core");
  check((g_bridgeInitialized - initializedBefore) ==
            (g_bridgeShutdown - shutdownBefore),
        "the bridge's shutdown pairs with its initialize on shutdown");
}

} // namespace

int main() {
  if (!set_working_directory_with_assets()) {
    std::printf("FAIL: assets not found\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.initialize = &bridge_initialize;
  bridge.shutdown = &bridge_shutdown;
  engine::runtime::set_editor_bridge(&bridge);

  check(engine::run(1U) == engine::RunResult::FatalInitialization,
        "run before any bootstrap is refused");
  engine::shutdown();
  check(g_bridgeShutdown == 0, "shutdown before any bootstrap does nothing");

  check_failed_stage_rolls_back(engine::BootstrapStage::Scripting, &bridge,
                                "scripting stage");
  check_failed_stage_rolls_back(engine::BootstrapStage::Audio, &bridge,
                                "audio stage");
  check_failed_stage_rolls_back(engine::BootstrapStage::TextureSystem, &bridge,
                                "texture stage");
  check_failed_stage_rolls_back(engine::BootstrapStage::EditorBridge, &bridge,
                                "editor bridge stage");
  check_failed_stage_rolls_back(engine::BootstrapStage::Mount, &bridge,
                                "mount stage");

  // --- Player mode displaces the bridge for the run and hands it back ---
  {
    const int initializedBefore = g_bridgeInitialized;
    engine::inject_bootstrap_failure(engine::BootstrapStage::Audio);
    check(!engine::bootstrap(headless_config(true)),
          "player-mode bootstrap fails at the injected stage");
    check(engine::runtime::editor_bridge() == &bridge,
          "a failed player-mode bootstrap hands the bridge back");
    check(g_bridgeInitialized == initializedBefore,
          "player mode never initializes the bridge");

    check(engine::bootstrap(headless_config(true)), "player-mode bootstrap");
    check(engine::runtime::editor_bridge() == nullptr,
          "player mode runs without the editor bridge");
    check(engine::active_config().playerMode, "player mode is active");
    engine::shutdown();
    check(engine::runtime::editor_bridge() == &bridge,
          "shutdown hands the displaced bridge back");

    check(engine::bootstrap(headless_config(false)),
          "an editor bootstrap follows the player run");
    check(g_bridgeInitialized == initializedBefore + 1,
          "the editor bootstrap initializes the bridge it got back");
    engine::shutdown();
    engine::shutdown();
    check(g_bridgeShutdown == g_bridgeInitialized,
          "a second shutdown closes nothing twice");
  }

  // --- A bootstrap while one is running is refused, not re-entered ---
  {
    check(engine::bootstrap(headless_config(false)), "first bootstrap");
    const int initialized = g_bridgeInitialized;
    check(!engine::bootstrap(headless_config(false)),
          "a second bootstrap while running is refused");
    check(engine::is_bootstrapped(), "the running bootstrap survives");
    check(g_bridgeInitialized == initialized,
          "the refused bootstrap opened nothing");
    engine::shutdown();
  }

  // --- Headless is honoured by every tier the config can reach ---
  {
    engine::EngineConfig config = headless_config(false);
    config.core.workerThreads = 1U;
    check(engine::bootstrap(config), "headless bootstrap with one worker");
    check(engine::core::thread_count() == 2U,
          "the configured worker count is the job system's (plus main)");
    check(engine::audio::audio_uses_null_device(),
          "headless mixes audio into no device");
    check(engine::audio::audio_is_initialized(), "bootstrap opens audio");
    engine::shutdown();
    check(!engine::audio::audio_uses_null_device(),
          "shutdown closes the device-less audio engine");
    check(!engine::audio::audio_is_initialized(), "shutdown closes audio");
  }

  // --- Exit codes name every outcome distinctly ---
  check(engine::run_result_exit_code(engine::RunResult::Stopped) ==
            static_cast<int>(engine::ExitCode::Ok),
        "Stopped maps to Ok");
  check(engine::run_result_exit_code(
            engine::RunResult::FatalInitialization) ==
            static_cast<int>(engine::ExitCode::FatalInitialization),
        "FatalInitialization maps to its own code");
  check(engine::run_result_exit_code(engine::RunResult::FatalFrame) ==
            static_cast<int>(engine::ExitCode::FatalFrame),
        "FatalFrame maps to its own code");
  check(static_cast<int>(engine::ExitCode::BootstrapFailed) !=
            static_cast<int>(engine::ExitCode::FatalInitialization),
        "a refused bootstrap and a fatal initialization differ");

  engine::runtime::set_editor_bridge(nullptr);
  if (g_failures != 0) {
    std::printf("engine bootstrap rollback: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
