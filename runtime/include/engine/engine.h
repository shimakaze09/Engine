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
  /// Player mode (#138): run the pure gameplay loop — the editor bridge
  /// is cleared at bootstrap and the renderer presents the scene straight
  /// to the back buffer. ENGINE_PLAYER=1 in the environment also enables
  /// it (the web share page's default).
  bool playerMode = false;
};

/// Outcome of engine::run for process exit-code mapping.
enum class RunResult : std::uint8_t {
  /// Graceful stop: quit request or the max-frame budget was reached.
  Stopped = 0,
  /// Runtime pipeline initialization failed before the first frame.
  FatalInitialization,
  /// A frame stage terminated the loop fatally.
  FatalFrame,
};

/// Boots the engine with the default configuration.
bool bootstrap() noexcept;
/// Boots the engine with explicit app/runtime configuration.
bool bootstrap(const EngineConfig &config) noexcept;
/// Returns the active engine configuration for runtime/editor systems.
const EngineConfig &active_config() noexcept;
/// Runs the main loop; reports whether it stopped gracefully or fatally.
RunResult run(std::uint32_t maxFrames = 0U) noexcept;
/// Maps a run result to the process exit code (0 only for Stopped).
int run_result_exit_code(RunResult result) noexcept;
/// Shuts down the owning system.
void shutdown() noexcept;

} // namespace engine
