// Implements the engine tier: bootstrap opens each subsystem as one stage
// on a fixed stack and a failure unwinds the stages already opened in
// reverse, so every failure path and shutdown share one rollback; run()
// drives the pipeline (or hands it to the browser loop on the web) and
// shutdown() unwinds the same stack once.

#include "engine/engine.h"

#if defined(ENGINE_PLATFORM_WEB)
#include <emscripten.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/audio/audio.h"
#include "engine/core/atomic_file.h"
#include "engine/core/bootstrap.h"
#include "engine/core/crash_report.h"
#include "engine/core/cvar.h"
#include "engine/core/fatal_exit.h"
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
#include "engine_config_strings.h"

namespace engine {

namespace {

constexpr std::size_t kFrameAllocatorBytes = 1024U * 1024U;
EngineConfig g_activeConfig{};

/// Opens this run's log file under the per-user data directory
/// (logs/editor.log, or logs/player.log in player mode), keeping the last
/// run's beside it, as Unity's Editor.log and Player.log are kept: a
/// windowed build has no console, so this is where its messages outlive
/// it. A headless run (tests, tools) writes none. Failing to open one is
/// said and not fatal: the run goes on with the in-app log and stdout.
void open_run_log() noexcept {
  if (g_activeConfig.core.platform.headless) {
    return;
  }
  char saveDir[512] = {};
  if (!core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
    core::log_message(core::LogLevel::Warning, "engine",
                      "no per-user data directory; this run keeps no log "
                      "file");
    return;
  }
  const std::size_t length = std::strlen(saveDir);
  const bool endsInSeparator =
      (length > 0U) &&
      ((saveDir[length - 1U] == '/') || (saveDir[length - 1U] == '\\'));
  char logDir[600] = {};
  std::snprintf(logDir, sizeof(logDir), "%s%slogs", saveDir,
                endsInSeparator ? "" : "/");
  if (!core::create_directories_durably(logDir)) {
    core::log_message(core::LogLevel::Warning, "engine",
                      "could not create the log directory; this run keeps "
                      "no log file");
    return;
  }
  const char *name = g_activeConfig.playerMode ? "player" : "editor";
  char path[640] = {};
  char previous[640] = {};
  std::snprintf(path, sizeof(path), "%s/%s.log", logDir, name);
  std::snprintf(previous, sizeof(previous), "%s/%s-prev.log", logDir, name);
  static_cast<void>(core::log_open_file(path, previous));
}
bool g_bootstrapped = false;

// The bootstrap stack: each opened stage pushes the function that closes
// it, and unwinding pops them in reverse. Ordering constraints the closers
// rely on: the texture registry and the editor bridge release device
// objects while the render device is still live, so both sit above the
// renderer; animation controllers hold renderer palette slots and reset
// above it too.
using StageCloser = void (*)() noexcept;
// Headroom over the stages bootstrap opens today, so adding one does not
// need this constant touched in the same breath. The guard in open_stage
// is what actually keeps it honest -- the count cannot be checked at
// compile time, because the calls are spread across the success path.
constexpr std::size_t kMaxBootstrapStages = 16U;
StageCloser g_openedStages[kMaxBootstrapStages]{};
std::size_t g_openedStageCount = 0U;

// Player mode clears the editor bridge for the run; the pointer it
// replaced comes back when the run closes so a later editor bootstrap in
// the same process still finds it.
const runtime::EditorBridge *g_displacedBridge = nullptr;
bool g_bridgeDisplaced = false;

BootstrapStage g_injectedFailure = BootstrapStage::None;

/// True once when the named stage is the injected failure.
bool consume_injected_failure(BootstrapStage stage) noexcept {
  if (g_injectedFailure != stage) {
    return false;
  }
  g_injectedFailure = BootstrapStage::None;
  core::log_message(core::LogLevel::Warning, "engine",
                    "bootstrap: injected stage failure");
  return true;
}

// Set when open_stage had to refuse. Bootstrap fails on it rather than
// running with a subsystem nothing will close.
bool g_stageOverflow = false;

void open_stage(StageCloser closer) noexcept {
  if (g_openedStageCount >= kMaxBootstrapStages) {
    // Writing past the array corrupts whatever follows it, which shows up
    // later as a fault somewhere unrelated -- exactly the debugging cost
    // this refusal exists to avoid. The stage stays unregistered and
    // bootstrap fails, because a subsystem whose closer was dropped would
    // leak on every unwind after it.
    g_stageOverflow = true;
    core::log_message(core::LogLevel::Error, "engine",
                      "bootstrap stage stack is full; raise "
                      "kMaxBootstrapStages");
    return;
  }
  g_openedStages[g_openedStageCount] = closer;
  ++g_openedStageCount;
}

void unwind_stages() noexcept {
  while (g_openedStageCount > 0U) {
    --g_openedStageCount;
    g_openedStages[g_openedStageCount]();
    g_openedStages[g_openedStageCount] = nullptr;
  }
}

/// Bootstrap failure: every opened stage closes in reverse and the active
/// configuration returns to its defaults, so a retry starts clean.
bool fail_bootstrap() noexcept {
  unwind_stages();
  g_activeConfig = EngineConfig{};
  g_stageOverflow = false;
  return false;
}

// ---- Stage closers, in bootstrap order ----------------------------------

void close_core() noexcept { core::shutdown_core(); }

void close_crash_report() noexcept { core::shutdown_crash_report(); }

// The note a fatal's recovery left for the user, kept past shutdown.
char g_fatalRecoveryNote[640] = {};

/// The fatal recovery hook: asks the editor, when one is attached, to
/// save a recovery copy of the unsaved scene, and says where it went. The
/// player has no document to lose, and its bridge is displaced.
void write_fatal_recovery(char *note, std::size_t capacity) noexcept {
  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge == nullptr) || (bridge->write_recovery_copy == nullptr)) {
    return;
  }
  char path[runtime::kRecoveryPathCapacity] = {};
  if (!bridge->write_recovery_copy(path, sizeof(path))) {
    return;
  }
  std::snprintf(note, capacity,
                "The unsaved scene was saved to:\n%s\nOpen it with "
                "File > Open Scene.",
                path);
}

void close_fatal_recovery() noexcept { core::set_fatal_recovery_hook(nullptr); }

void close_renderer() noexcept { renderer::shutdown_renderer(); }

void restore_editor_bridge() noexcept {
  if (g_bridgeDisplaced) {
    runtime::set_editor_bridge(g_displacedBridge);
    g_displacedBridge = nullptr;
    g_bridgeDisplaced = false;
  }
}

/// Closes the editor bridge's device resources while the device is still
/// live.
void close_editor_bridge() noexcept {
  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
    bridge->shutdown();
  }
}

void close_scripting() noexcept {
  scripting::dap_stop();
  scripting::shutdown_scripting();
}

void close_audio() noexcept { audio::shutdown_audio(); }

/// The bootstrap-owned texture registry owns the device object behind
/// every texture it loaded, so it closes while the device is still live.
/// Scene-capture textures are registered as external aliases the registry
/// never destroys; their creator releases them inside shutdown_renderer,
/// also ahead of the device.
void close_texture_system() noexcept { renderer::shutdown_texture_system(); }

void close_run_registries() noexcept { runtime::reset_anim_controllers(); }

} // namespace

namespace {

/// Where the engine's own content lives when the config leaves engineRoot
/// empty; see EngineConfig::engineRoot. Staged into `out`; the last
/// candidate is returned even when missing, so the mount stage refuses it
/// with the path in its message.
const char *resolve_engine_root(char *out, std::size_t capacity) noexcept {
  constexpr const char *kEngineDirectory = "engine_assets";
  if (core::non_empty_env("ENGINE_ROOT", out, capacity)) {
    return out;
  }
  char appDir[kMaxConfigStringLength + 1U] = {};
  if (core::platform_get_app_dir(appDir, sizeof(appDir))) {
    const std::size_t length = std::strlen(appDir);
    const bool slash = (length > 0U) && ((appDir[length - 1U] == '/') ||
                                         (appDir[length - 1U] == '\\'));
    const int written = std::snprintf(out, capacity, "%s%s%s", appDir,
                                      slash ? "" : "/", kEngineDirectory);
    if ((written > 0) && (static_cast<std::size_t>(written) < capacity) &&
        core::os_directory_exists(out)) {
      return out;
    }
  }
  std::snprintf(out, capacity, "%s", kEngineDirectory);
  return out;
}

/// False, with an Error naming the field, when `config` runs no project
/// (an empty asset root) yet names project content: a main script, a
/// startup scene, an editor asset root or packages would resolve against
/// whatever happens to be in the working directory. Its strings are the
/// adopted, non-null copies.
bool project_fields_consistent(const EngineConfig &config) noexcept {
  if (config.assetRoot[0] != '\0') {
    return true;
  }
  const char *field = nullptr;
  if (config.packageCount > 0U) {
    field = "packages";
  } else if (config.mainScriptPath[0] != '\0') {
    field = "mainScriptPath";
  } else if (config.editorScenePath[0] != '\0') {
    field = "editorScenePath";
  } else if (config.editorAssetRoot[0] != '\0') {
    field = "editorAssetRoot";
  }
  if (field == nullptr) {
    return true;
  }
  char message[160] = {};
  static_cast<void>(std::snprintf(
      message, sizeof(message),
      "configuration names '%s' but no project (assetRoot is empty)", field));
  core::log_message(core::LogLevel::Error, "engine", message);
  return false;
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
  if (g_bootstrapped) {
    core::log_message(core::LogLevel::Error, "engine",
                      "bootstrap: the engine is already running");
    return false;
  }

  // The caller's strings are borrowed for the duration of this call only,
  // while runtime and editor systems keep reading active_config() frames
  // later, so the engine takes its own copies first. Staging into a local
  // keeps the active configuration intact when a string is rejected.
  EngineConfig adopted = config;
  char engineRoot[kMaxConfigStringLength + 1U] = {};
  if ((adopted.engineRoot != nullptr) && (adopted.engineRoot[0] == '\0')) {
    adopted.engineRoot = resolve_engine_root(engineRoot, sizeof(engineRoot));
  }
  if (!adopt_config_strings(adopted) || !project_fields_consistent(adopted)) {
    return false;
  }
  // A project opened from its document is named by its GUID (see
  // open_project); the mounted root names one that was not, so two
  // projects never share their saves and rebound input map. A run with no
  // project has no per-project data.
  adopted.core.projectRoot =
      (adopted.assetRoot[0] != '\0') ? adopted.assetRoot : nullptr;
  g_activeConfig = adopted;

  if (consume_injected_failure(BootstrapStage::Core) ||
      !core::initialize_core(g_activeConfig.core)) {
    return fail_bootstrap();
  }
  open_stage(&close_core);
  open_run_log();

  // Installed straight after core, so a fault in anything opened below
  // still names the build, frame and stage. A refusal is not fatal: the
  // engine runs without crash reporting, having said so, rather than
  // refusing to start over a diagnostic.
  if (core::install_crash_report()) {
    open_stage(&close_crash_report);
  } else {
    core::log_message(core::LogLevel::Warning, "core",
                      "crash reporting unavailable; a fault will leave no "
                      "build, frame or stage");
  }
  g_fatalRecoveryNote[0] = '\0';
  core::set_fatal_recovery_hook(&write_fatal_recovery);
  open_stage(&close_fatal_recovery);

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
      "bgfx backend only: renderer API (auto, vulkan, opengl, gles, metal, "
      "d3d11, d3d12, noop); read once at device initialization"));

  static_cast<void>(core::cvar_register_bool(
      "r_bgfx_trace", false,
      "bgfx backend only: route bgfx trace output into the engine log"));

  // Device reach: render scale, its dynamic controller,
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

  // The mounts live and die with core, so they open no stage of their
  // own. The engine's content must be there: without it no shader, font
  // or bootstrap mesh loads, so a missing root refuses here rather than
  // as a string of load failures later.
  if (consume_injected_failure(BootstrapStage::Mount) ||
      (has_open_project() &&
       !core::mount(g_activeConfig.assetMount, g_activeConfig.assetRoot))) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to mount configured asset root");
    return fail_bootstrap();
  }
  if (!core::mount(g_activeConfig.engineMount, g_activeConfig.engineRoot) ||
      !core::vfs_directory_exists(g_activeConfig.engineMount)) {
    char message[kMaxConfigStringLength + 96U] = {};
    static_cast<void>(std::snprintf(
        message, sizeof(message),
        "engine content root '%s' is not a directory; nothing the engine "
        "ships (shaders, fonts) can load",
        g_activeConfig.engineRoot));
    core::log_message(core::LogLevel::Error, "engine", message);
    return fail_bootstrap();
  }
  for (std::size_t i = 0U; i < g_activeConfig.packageCount; ++i) {
    const ContentMount &package = g_activeConfig.packages[i];
    if (!core::mount(package.mount, package.root) ||
        !core::vfs_directory_exists(package.mount)) {
      char message[(2U * kMaxConfigStringLength) + 96U] = {};
      static_cast<void>(std::snprintf(
          message, sizeof(message),
          "package '%s' could not be mounted from '%s', which is not a "
          "directory",
          package.mount, package.root));
      core::log_message(core::LogLevel::Error, "engine", message);
      return fail_bootstrap();
    }
  }
  if (!renderer::set_shader_root_path(g_activeConfig.shaderRootPath)) {
    return fail_bootstrap();
  }
  // Opens the renderer lifetime this bootstrap owns. A process that
  // bootstraps again after shutting down gets a renderer that initializes
  // on demand once more, instead of one still latched off by the previous
  // teardown.
  renderer::initialize_renderer();
  open_stage(&close_renderer);

  // A windowed run initializes the swapchain-owning device here, before
  // the editor bridge — the bgfx ImGui renderer creates device objects
  // during bridge init (initialize_render_device is idempotent, so the
  // pipeline's later call is a no-op). Headless runs keep the pipeline's
  // lazy null-device initialization. The device closes with the renderer.
  if (consume_injected_failure(BootstrapStage::RenderDevice) ||
      (!g_activeConfig.core.platform.headless &&
       !renderer::initialize_render_device())) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "render device initialization failed at bootstrap");
    return fail_bootstrap();
  }

  // Player mode: the pure gameplay loop for shared creations —
  // clearing the bridge before its init makes the pipeline treat the run
  // as always-playing, and r_present_scene has the renderer draw the
  // final image to the back buffer in the editor overlay's place. The
  // displaced bridge returns when the run closes.
  static_cast<void>(core::cvar_register_bool(
      "r_present_scene", false,
      "Present the post chain's final image on the back buffer (player "
      "mode; the editor overlay presents otherwise)"));
  static_cast<void>(core::cvar_register_bool(
      "app.player_mode", false,
      "Boot straight into the gameplay loop with no editor (the web share "
      "page sets it through ENGINE_CVAR_app_player_mode)"));
  if (core::cvar_get_bool("app.player_mode", false)) {
    g_activeConfig.playerMode = true;
  }
  if (g_activeConfig.playerMode) {
    g_displacedBridge = runtime::editor_bridge();
    g_bridgeDisplaced = true;
    runtime::set_editor_bridge(nullptr);
    static_cast<void>(core::cvar_set_bool("r_present_scene", true));
  }
  open_stage(&restore_editor_bridge);

  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge != nullptr) && (bridge->initialize != nullptr)) {
    if (consume_injected_failure(BootstrapStage::EditorBridge) ||
        !bridge->initialize(core::get_sdl_window())) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to initialize editor bridge");
      return fail_bootstrap();
    }
    open_stage(&close_editor_bridge);
  }

  // The limits persist across VMs, so every run sets its own before the
  // VM exists: the allocator enforces the byte cap from state creation,
  // and a previous run's project must not leak its limits into this one.
  scripting::set_memory_limit(g_activeConfig.scriptMemoryLimitBytes);
  scripting::set_instruction_limit(g_activeConfig.scriptInstructionLimit);
  if (consume_injected_failure(BootstrapStage::Scripting) ||
      !scripting::initialize_scripting()) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to initialize scripting");
    return fail_bootstrap();
  }
  open_stage(&close_scripting);

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

  audio::AudioConfig audioConfig{};
  audioConfig.nullDevice = g_activeConfig.audioNullDevice ||
                           g_activeConfig.core.platform.headless;
  if (consume_injected_failure(BootstrapStage::Audio) ||
      !audio::initialize_audio(audioConfig)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to initialize audio");
    return fail_bootstrap();
  }
  open_stage(&close_audio);

  // Bootstrap owns the texture registry's lifetime; every production
  // texture consumer is gated on it and shutdown tears it down.
  if (consume_injected_failure(BootstrapStage::TextureSystem) ||
      !renderer::initialize_texture_system()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to initialize texture system");
    return fail_bootstrap();
  }
  open_stage(&close_texture_system);
  open_stage(&close_run_registries);

  // Checked once, here, rather than at nine call sites: a refused stage
  // means the unwind is already incomplete, so the run must not start.
  if (g_stageOverflow) {
    return fail_bootstrap();
  }

  g_bootstrapped = true;
  core::log_message(core::LogLevel::Info, "engine", "bootstrap complete");
  return true;
}

/// Returns the active engine configuration for runtime/editor systems.
const EngineConfig &active_config() noexcept { return g_activeConfig; }

bool has_open_project() noexcept {
  return (g_activeConfig.assetRoot != nullptr) &&
         (g_activeConfig.assetRoot[0] != '\0');
}

bool is_bootstrapped() noexcept { return g_bootstrapped; }

void inject_bootstrap_failure(BootstrapStage stage) noexcept {
  g_injectedFailure = stage;
}

#if defined(ENGINE_PLATFORM_WEB)
namespace {

/// Browser frame callback: one engine frame per requestAnimationFrame
/// tick. When the loop ends the pipeline tears down and the engine tier
/// closes after it, exactly as a native run returning to main() would;
/// the pipeline outlives run()'s unwound stack as a static.
void web_frame(void *arg) noexcept {
  auto *pipeline = static_cast<EnginePipeline *>(arg);
  if (!pipeline->execute_frame()) {
    if (pipeline->had_fatal_error()) {
      core::log_message(core::LogLevel::Error, "engine",
                        "engine stopped on a fatal frame error");
      static_cast<void>(core::run_fatal_recovery_hook(
          g_fatalRecoveryNote, sizeof(g_fatalRecoveryNote)));
    }
    pipeline->teardown();
    emscripten_cancel_main_loop();
    shutdown();
  }
}

} // namespace
#endif

/// Runs the main loop; reports whether it stopped gracefully or fatally.
RunResult run(std::uint32_t maxFrames) noexcept {
  if (!g_bootstrapped) {
    core::log_message(core::LogLevel::Error, "engine",
                      "run: the engine has not been bootstrapped");
    return RunResult::FatalInitialization;
  }
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
  // Before teardown, which detaches the editor from its World: a fatal
  // frame ends the run, so this is the last moment the unsaved scene can
  // be saved.
  if (result == RunResult::FatalFrame) {
    static_cast<void>(core::run_fatal_recovery_hook(
        g_fatalRecoveryNote, sizeof(g_fatalRecoveryNote)));
  }
  pipeline.teardown();
  return result;
#endif
}

static_assert(static_cast<int>(ExitCode::FatalDevice) ==
                  core::kFatalDeviceExitCode,
              "the renderer exits with core's device-fatal code");

const char *fatal_recovery_note() noexcept { return g_fatalRecoveryNote; }

int run_result_exit_code(RunResult result) noexcept {
  switch (result) {
  case RunResult::Stopped:
    return static_cast<int>(ExitCode::Ok);
  case RunResult::FatalInitialization:
    return static_cast<int>(ExitCode::FatalInitialization);
  case RunResult::FatalFrame:
    break;
  }
  return static_cast<int>(ExitCode::FatalFrame);
}

/// Closes every stage bootstrap opened, in reverse; a second call, or a
/// call without a bootstrap, does nothing.
void shutdown() noexcept {
  if (!g_bootstrapped) {
    return;
  }
  g_bootstrapped = false;
  unwind_stages();
  core::log_message(core::LogLevel::Info, "engine", "shutdown complete");
}

} // namespace engine
