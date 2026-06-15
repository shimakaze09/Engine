// Implements engine behavior for the Engine runtime world.

#include "engine/engine.h"

#include <cstddef>
#include <cstdint>

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
#include "engine/physics/physics.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/scripting/dap_server.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

constexpr std::size_t kFrameAllocatorBytes = 1024U * 1024U;
EngineConfig g_activeConfig{};

} // namespace

/// Handles bootstrap.
bool bootstrap() noexcept {
  EngineConfig config{};
  config.core.frameAllocatorBytes = kFrameAllocatorBytes;
  return bootstrap(config);
}

/// Boots the engine with explicit app/runtime configuration.
bool bootstrap(const EngineConfig &config) noexcept {
  g_activeConfig = config;

  if (!core::initialize_core(g_activeConfig.core)) {
    return false;
  }

  static_cast<void>(core::cvar_register_bool(
      "r_showStats", true,
      "Toggle in-game stats and profiling overlays in the editor"));

  static_cast<void>(core::cvar_register_int(
      "debug_dap_port", 0,
      "DAP debugger port (0 = disabled). Set to e.g. 4711 to enable."));

  static_cast<void>(physics::register_physics_cvars());

  // Mount the configured project asset root before runtime/editor paths are
  // resolved through the VFS.
  if (!core::mount(g_activeConfig.assetMount, g_activeConfig.assetRoot)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to mount configured asset root");
    core::shutdown_core();
    return false;
  }
  renderer::set_shader_root_path(g_activeConfig.shaderRootPath);

  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge != nullptr) && (bridge->initialize != nullptr)) {
    if (!core::make_render_context_current()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to acquire OpenGL context for editor init");
      core::shutdown_core();
      return false;
    }

    if (!bridge->initialize(core::get_sdl_window(),
                            core::get_sdl_gl_context())) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to initialize editor bridge");
      core::release_render_context();
      core::shutdown_core();
      return false;
    }

    core::release_render_context();
  }

  if (!scripting::initialize_scripting()) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to initialize scripting");
    if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
      bridge->shutdown();
    }
    core::shutdown_core();
    return false;
  }

  // Start DAP debugger server if CVar port is set.
  {
    const int dapPort = core::cvar_get_int("debug_dap_port");
    if (dapPort > 0) {
      if (scripting::dap_start(static_cast<std::uint16_t>(dapPort))) {
        core::log_message(core::LogLevel::Info, "scripting",
                          "DAP debugger listening");
      } else {
        core::log_message(core::LogLevel::Warning, "scripting",
                          "failed to start DAP debugger");
      }
    }
  }

  if (!audio::initialize_audio()) {
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to initialize audio");
    scripting::dap_stop();
    scripting::shutdown_scripting();
    if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
      bridge->shutdown();
    }
    core::shutdown_core();
    return false;
  }

  core::log_message(core::LogLevel::Info, "engine", "bootstrap complete");
  return true;
}

/// Returns the active engine configuration for runtime/editor systems.
const EngineConfig &active_config() noexcept { return g_activeConfig; }

/// Runs the configured command, loop, or tool.
void run(std::uint32_t maxFrames) noexcept {
  EnginePipeline pipeline;
  if (!pipeline.initialize(maxFrames)) {
    pipeline.teardown();
    return;
  }

  while (pipeline.execute_frame()) {
  }

  pipeline.teardown();
}

/// Shuts down the owning system.
void shutdown() noexcept {
  core::log_message(core::LogLevel::Info, "engine", "shutdown complete");

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
    bridge->shutdown();
  }
  renderer::shutdown_renderer();
  audio::shutdown_audio();
  scripting::dap_stop();
  scripting::shutdown_scripting();
  core::shutdown_core();
}

} // namespace engine
