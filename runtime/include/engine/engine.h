// Declares engine types and APIs for the Engine runtime world.

#pragma once

#include <cstdint>

#include "engine/core/bootstrap.h"

namespace engine {

/// Describes app/runtime startup paths and core ownership.
struct EngineConfig final {
  core::CoreConfig core{};
  const char *assetMount = "assets";
  const char *assetRoot = "assets";
  const char *mainScriptPath = "assets/main.lua";
  const char *bootstrapMeshPath = "assets/triangle.mesh";
  const char *shaderRootPath = "assets/shaders";
  const char *editorScenePath = "assets/scene.json";
  const char *editorAssetRoot = "assets";
  /// Player mode: run the pure gameplay loop — the editor bridge
  /// is cleared at bootstrap and the renderer presents the scene straight
  /// to the back buffer. ENGINE_PLAYER=1 in the environment also enables
  /// it (the web share page's default).
  bool playerMode = false;
};

/// Outcome of engine::run for process exit-code mapping.
enum class RunResult : std::uint8_t {
  /// Graceful stop: quit request or the max-frame budget was reached.
  Stopped = 0,
  /// Runtime pipeline initialization failed before the first frame, or
  /// run() was called without a bootstrap.
  FatalInitialization,
  /// A frame stage terminated the loop fatally.
  FatalFrame,
};

/// Process exit codes: one per way a run can end, so a launcher or CI
/// step can tell a refused bootstrap from a fatal frame.
enum class ExitCode : int {
  Ok = 0,
  BootstrapFailed = 1,
  FatalInitialization = 2,
  FatalFrame = 3,
};

/// Bootstrap stages a test may fail on purpose; None injects nothing.
enum class BootstrapStage : std::uint8_t {
  None = 0,
  Core,
  Mount,
  RenderDevice,
  EditorBridge,
  Scripting,
  Audio,
  TextureSystem,
};

/// Boots the engine with the default configuration.
bool bootstrap() noexcept;
/// Boots the engine with explicit app/runtime configuration. A failure at
/// any stage closes the stages already opened, in reverse, and returns
/// the active configuration to its defaults; a second bootstrap while one
/// is running is refused.
bool bootstrap(const EngineConfig &config) noexcept;
/// True between a successful bootstrap and its shutdown.
bool is_bootstrapped() noexcept;
/// Returns the active engine configuration for runtime/editor systems.
const EngineConfig &active_config() noexcept;
/// Test-only fault injection: the next bootstrap fails at `stage` through
/// that stage's production failure path; consumed once.
void inject_bootstrap_failure(BootstrapStage stage) noexcept;
/// Runs the main loop; reports whether it stopped gracefully or fatally.
RunResult run(std::uint32_t maxFrames = 0U) noexcept;
/// Maps a run result to its ExitCode value (0 only for Stopped).
int run_result_exit_code(RunResult result) noexcept;
/// Closes every stage bootstrap opened; idempotent.
void shutdown() noexcept;

} // namespace engine
