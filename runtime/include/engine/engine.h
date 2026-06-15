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
};

/// Handles bootstrap.
bool bootstrap() noexcept;
/// Boots the engine with explicit app/runtime configuration.
bool bootstrap(const EngineConfig &config) noexcept;
/// Returns the active engine configuration for runtime/editor systems.
const EngineConfig &active_config() noexcept;
/// Runs the configured command, loop, or tool.
void run(std::uint32_t maxFrames = 0U) noexcept;
/// Shuts down the owning system.
void shutdown() noexcept;

} // namespace engine
