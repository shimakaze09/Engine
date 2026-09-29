// Declares engine types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/bootstrap.h"
#include "engine/scripting/script_limits.h"

namespace engine {

/// One content directory mounted beside the project's own: the virtual
/// prefix it answers to and the OS directory it reads.
struct ContentMount final {
  const char *mount = "";
  const char *root = "";
};

/// Most package mounts a configuration carries; the project document
/// refuses a list longer than this.
inline constexpr std::size_t kMaxPackageMounts = 16U;

/// Describes app/runtime startup paths and core ownership.
struct EngineConfig final {
  core::CoreConfig core{};
  /// The game's content: mounted at `assetMount` from the OS directory
  /// `assetRoot`, and addressed as `assets/...` by every authored path.
  /// An empty `assetRoot` runs with no project, as the project hub does:
  /// only the engine's content is mounted, and no main script, startup
  /// scene or per-project data is used. Bootstrap refuses such a config
  /// if it still names a main script, a startup scene or an editor asset
  /// root (configure_without_project in engine/project.h clears them).
  const char *assetMount = "assets";
  const char *assetRoot = "assets";
  /// The project's .project document, absolute, when it was opened from
  /// one (open_project); empty otherwise.
  const char *projectFile = "";
  /// The engine's own content (shaders, fonts, the web shell, the
  /// bootstrap mesh): mounted at `engineMount` from the OS directory
  /// `engineRoot`, kept apart from any project's content as Unreal's
  /// Engine/Content is. A relative root resolves against the working
  /// directory. Empty (the default) finds it the way an installed engine
  /// is found, independent of where it was started: the ENGINE_ROOT
  /// environment variable, then engine_assets beside the executable, then
  /// engine_assets in the working directory. Bootstrap refuses a root that
  /// is not a directory.
  const char *engineMount = "engine";
  const char *engineRoot = "";
  /// The packages the project depends on, each mounted at its own prefix
  /// ("packages/<name>") from its own directory, catalogued like the
  /// project's content. open_project fills them; bootstrap refuses more
  /// than kMaxPackageMounts, a null table with a count, packages with no
  /// project, and a root that is not a directory.
  const ContentMount *packages = nullptr;
  std::size_t packageCount = 0U;
  const char *mainScriptPath = "assets/main.lua";
  const char *bootstrapMeshPath = "engine/triangle.mesh";
  const char *shaderRootPath = "engine/shaders";
  const char *editorScenePath = "assets/main.scene";
  const char *editorAssetRoot = "assets";
  /// Player mode: run the pure gameplay loop — the editor bridge
  /// is cleared at bootstrap and the renderer presents the scene straight
  /// to the back buffer. The app.player_mode cvar also enables it (the
  /// web share page's default, seeded through ENGINE_CVAR_app_player_mode).
  bool playerMode = false;
  /// The Lua sandbox limits the run starts with, applied before the VM is
  /// created: the instruction budget every script shares per frame and the
  /// allocator's byte cap, 0 unlimited for both. open_project sets them
  /// from the project's document.
  int scriptInstructionLimit = scripting::kDefaultInstructionLimit;
  std::size_t scriptMemoryLimitBytes = scripting::kDefaultMemoryLimit;
  /// Mix audio into no device; a headless platform forces this on, the
  /// way it forces the null render device.
  bool audioNullDevice = false;
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
/// True when the active configuration runs a project: its asset root is
/// not empty.
bool has_open_project() noexcept;
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
