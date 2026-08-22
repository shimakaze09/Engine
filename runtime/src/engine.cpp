// Implements engine behavior for the Engine runtime world.

#include "engine/engine.h"

#if defined(ENGINE_PLATFORM_WEB)
#include <emscripten.h>
#endif

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
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/animation_system.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/scripting/dap_server.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

constexpr std::size_t kFrameAllocatorBytes = 1024U * 1024U;
EngineConfig g_activeConfig{};

/// Shuts down editor GL resources while the platform context is current.
void shutdown_editor_bridge(const runtime::EditorBridge *bridge) noexcept {
  if ((bridge == nullptr) || (bridge->shutdown == nullptr)) {
    return;
  }

  if (!core::make_render_context_current()) {
    core::log_message(core::LogLevel::Error, "editor",
                      "failed to acquire render context for editor shutdown");
    return;
  }

  bridge->shutdown();
  core::release_render_context();
}

} // namespace

bool bootstrap() noexcept {
  EngineConfig config{};
  config.core.frameAllocatorBytes = kFrameAllocatorBytes;
  return bootstrap(config);
}

/// Boots the engine with explicit app/runtime configuration. The
/// configured project asset root is mounted before any runtime or
/// editor path resolves through the VFS.
bool bootstrap(const EngineConfig &config) noexcept {
  g_activeConfig = config;

  // #138: a backend that owns its swapchain (bgfx) needs the platform
  // window created without an OpenGL context; headless keeps priority.
  if (!g_activeConfig.core.platform.headless) {
    g_activeConfig.core.platform.externalRenderContext =
        renderer::render_backend_owns_swapchain();
  }

  if (!core::initialize_core(g_activeConfig.core)) {
    return false;
  }

  static_cast<void>(core::cvar_register_bool(
      "r_showStats", true,
      "Toggle in-game stats and profiling overlays in the editor"));

  static_cast<void>(core::cvar_register_int(
      "debug_dap_port", 0,
      "DAP debugger port (0 = disabled). Set to e.g. 4711 to enable."));

  static_cast<void>(core::cvar_register_bool(
      "r_null_device", false,
      "Test/CI: replace the render device with a null backend so "
      "pipeline init and frame stages run headless (#196)"));
  // One authored intent: a headless platform has no GL, so the render
  // device must be the null backend.
  if (g_activeConfig.core.platform.headless) {
    static_cast<void>(core::cvar_set_bool("r_null_device", true));
  }

  static_cast<void>(core::cvar_register_string(
      "dbg_fail_frame_stage", "",
      "Test-only fault injection: fail the named frame stage once "
      "(simulation_graph or render_prep_graph); self-clears when consumed"));

  static_cast<void>(core::cvar_register_string(
      "r_bgfx_renderer", "auto",
      "bgfx backend only: renderer API (auto, vulkan, opengl, metal, "
      "noop); read once at device initialization (#138)"));

  static_cast<void>(core::cvar_register_bool(
      "r_bgfx_trace", false,
      "bgfx backend only: route bgfx trace output into the engine log"));

  // Device reach (#138 v0.5): render scale, its dynamic controller,
  // and the named quality tiers.
  static_cast<void>(core::cvar_register_float(
      "r_render_scale", 1.0F,
      "Scene render scale (0.25-1); the present upsamples to full size"));
  static_cast<void>(core::cvar_register_bool(
      "r_dynamic_resolution", false,
      "Adjust the render scale automatically from frame time"));
  static_cast<void>(core::cvar_register_float(
      "r_dynamic_resolution_min", 0.5F,
      "Lowest render scale the dynamic controller may reach"));
  static_cast<void>(core::cvar_register_string(
      "r_quality", "",
      "Quality preset: low, medium, or high; empty keeps custom cvars"));

  static_cast<void>(core::cvar_register_bool(
      "r_bgfx_debug", false,
      "bgfx backend only: init with bgfx debug checks (read once at "
      "device initialization)"));

  static_cast<void>(physics::register_physics_cvars());

  if (!core::mount(g_activeConfig.assetMount, g_activeConfig.assetRoot)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to mount configured asset root");
    core::shutdown_core();
    return false;
  }
  renderer::set_shader_root_path(g_activeConfig.shaderRootPath);

  // #138: the swapchain-owning backend initializes its device here,
  // before the editor bridge — the bgfx ImGui renderer creates device
  // objects during bridge init (initialize_render_device is idempotent,
  // so the pipeline's later call is a no-op).
  if (g_activeConfig.core.platform.externalRenderContext &&
      !renderer::initialize_render_device()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "render device initialization failed at bootstrap");
    core::shutdown_core();
    return false;
  }

  // Player mode (#138): the pure gameplay loop for shared creations —
  // clearing the bridge before its init makes the pipeline treat the run
  // as always-playing, and r_present_scene has the renderer draw the
  // final image to the back buffer in the editor overlay's place.
  static_cast<void>(core::cvar_register_bool(
      "r_present_scene", false,
      "Present the post chain's final image on the back buffer (player "
      "mode; the editor overlay presents otherwise)"));
  const char *playerEnv = core::non_empty_env("ENGINE_PLAYER");
  if ((playerEnv != nullptr) && (playerEnv[0] == '1')) {
    g_activeConfig.playerMode = true;
  }
  if (g_activeConfig.playerMode) {
    runtime::set_editor_bridge(nullptr);
    static_cast<void>(core::cvar_set_bool("r_present_scene", true));
  }

  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge != nullptr) && (bridge->initialize != nullptr)) {
    if (!core::make_render_context_current()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to acquire render context for editor init");
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
    shutdown_editor_bridge(bridge);
    core::shutdown_core();
    return false;
  }

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
    shutdown_editor_bridge(bridge);
    core::shutdown_core();
    return false;
  }

  // Bootstrap owns the texture registry's lifetime (#234); every production
  // texture consumer is gated on it and engine::shutdown tears it down.
  if (!renderer::initialize_texture_system()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to initialize texture system");
    audio::shutdown_audio();
    scripting::dap_stop();
    scripting::shutdown_scripting();
    shutdown_editor_bridge(bridge);
    core::shutdown_core();
    return false;
  }

  core::log_message(core::LogLevel::Info, "engine", "bootstrap complete");
  return true;
}

/// Returns the active engine configuration for runtime/editor systems.
const EngineConfig &active_config() noexcept { return g_activeConfig; }

#if defined(ENGINE_PLATFORM_WEB)
namespace {

/// Browser frame callback (#138 web): one engine frame per
/// requestAnimationFrame tick, tearing the pipeline down when the loop
/// ends (the pipeline outlives run()'s unwound stack as a static).
void web_frame(void *arg) noexcept {
  auto *pipeline = static_cast<EnginePipeline *>(arg);
  if (!pipeline->execute_frame()) {
    if (pipeline->had_fatal_error()) {
      core::log_message(core::LogLevel::Error, "engine",
                        "engine stopped on a fatal frame error");
    }
    pipeline->teardown();
    emscripten_cancel_main_loop();
  }
}

} // namespace
#endif

/// Runs the main loop; reports whether it stopped gracefully or fatally.
RunResult run(std::uint32_t maxFrames) noexcept {
#if defined(ENGINE_PLATFORM_WEB)
  // The browser owns the loop: hand execute_frame to
  // requestAnimationFrame and unwind out of run() (simulate_infinite
  // unwinds via the JS event loop, so the static pipeline must own the
  // state; frame pacing collapses into RAF).
  static EnginePipeline pipeline;
  if (!pipeline.initialize(maxFrames)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "runtime pipeline initialization failed");
    pipeline.teardown();
    return RunResult::FatalInitialization;
  }
  emscripten_set_main_loop_arg(&web_frame, &pipeline, 0, 1);
  return RunResult::Stopped; // unreachable: the call above unwinds
#else
  EnginePipeline pipeline;
  if (!pipeline.initialize(maxFrames)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "runtime pipeline initialization failed");
    pipeline.teardown();
    return RunResult::FatalInitialization;
  }

  while (pipeline.execute_frame()) {
  }

  const RunResult result = pipeline.had_fatal_error() ? RunResult::FatalFrame
                                                      : RunResult::Stopped;
  pipeline.teardown();
  return result;
#endif
}

/// Maps a run result to the process exit code (0 only for Stopped).
int run_result_exit_code(RunResult result) noexcept {
  return (result == RunResult::Stopped) ? 0 : 1;
}

/// Shuts down the owning system.
void shutdown() noexcept {
  core::log_message(core::LogLevel::Info, "engine", "shutdown complete");

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  shutdown_editor_bridge(bridge);
  runtime::reset_anim_controllers();
  renderer::shutdown_renderer();
  // Close the bootstrap-owned texture registry after the renderer unloads
  // its capture handles; remaining GL objects die with the context below.
  renderer::shutdown_texture_system();
  audio::shutdown_audio();
  scripting::dap_stop();
  scripting::shutdown_scripting();
  core::shutdown_core();
}

} // namespace engine
