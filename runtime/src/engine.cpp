#include "engine/engine.h"

#include <cstddef>
#include <cstdint>

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/scripting/dap_server.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

constexpr std::size_t kFrameAllocatorBytes = 1024U * 1024U;

} // namespace

bool bootstrap() noexcept {
  if (!core::initialize_core(kFrameAllocatorBytes)) {
    return false;
  }

  static_cast<void>(core::cvar_register_bool(
      "r_showStats", true,
      "Toggle in-game stats and profiling overlays in the editor"));

  static_cast<void>(core::cvar_register_int(
      "debug_dap_port", 0,
      "DAP debugger port (0 = disabled). Set to e.g. 4711 to enable."));

  // Mount the assets directory relative to CWD so that VFS paths like
  // "assets/shaders/pbr.vert" resolve correctly when running from build/.
  static_cast<void>(core::mount("assets", "assets"));

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

void run(std::uint32_t maxFrames) noexcept {
  EnginePipeline pipeline;
  if (!pipeline.initialize(maxFrames)) {
    return;
  }

  while (pipeline.execute_frame()) {
  }

  pipeline.teardown();
}

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
