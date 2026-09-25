// Browser lifecycle harness for the web frame loop (#341) and the web save
// slot (#695). The page runs one case, named by its URL: a bounded smoke
// run, maxFrames=1, a platform quit mid-run, a fatal frame stage, or
// save_persists, where the driver saves, reloads the page and loads. Every
// run case ends its loop inside web_frame; the driver (run_lifecycle.mjs)
// then asks which engine tiers are still open, starts a second bootstrap in
// the same page, and asks again once that run ends too. Lines prefixed
// "[web-lifecycle]" are the protocol the driver reads.

#include <emscripten.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/cvar.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/engine.h"
#include "engine/renderer/render_device.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/save_data.h"
#include "engine/scripting/scripting.h"

namespace {

enum class LifecycleCase : std::uint8_t { Frames, MaxFramesOne, Quit, FatalStage };

// The smoke case's bound: past the first frame's one-time setup and into
// steady-state frames.
constexpr std::uint32_t kSmokeFrames = 30U;
// The second bootstrap's run, long enough to prove the page renders again.
constexpr std::uint32_t kSecondRunFrames = 3U;

LifecycleCase g_case = LifecycleCase::Frames;
// The editor's bridge as registered before any bootstrap; player mode
// displaces it for a run and shutdown must hand it back.
const engine::runtime::EditorBridge *g_registeredBridge = nullptr;

struct Tier final {
  const char *name;
  bool (*open)() noexcept;
};

// One probe per engine-tier owner #341 names: the renderer's device, audio,
// Lua, the job system, and core, which owns platform, VFS and logging.
constexpr Tier kTiers[] = {
    {"engine", [] () noexcept { return engine::is_bootstrapped(); }},
    {"core", [] () noexcept { return engine::core::is_core_initialized(); }},
    {"jobs",
     [] () noexcept { return engine::core::is_job_system_initialized(); }},
    {"renderer",
     [] () noexcept { return engine::renderer::render_device() != nullptr; }},
    {"scripting",
     [] () noexcept { return engine::scripting::get_memory_used() != 0U; }},
    {"audio", [] () noexcept { return engine::audio::audio_is_initialized(); }},
};

/// Prints which tiers are open and returns them as a bit per kTiers entry.
unsigned report_tiers(const char *label, int run) noexcept {
  unsigned mask = 0U;
  char names[128] = {};
  std::size_t used = 0U;
  for (std::size_t i = 0U; i < (sizeof(kTiers) / sizeof(kTiers[0])); ++i) {
    if (!kTiers[i].open()) {
      continue;
    }
    mask |= (1U << i);
    const int written =
        std::snprintf(names + used, sizeof(names) - used, "%s%s",
                      (used == 0U) ? "" : ",", kTiers[i].name);
    if ((written > 0) &&
        (static_cast<std::size_t>(written) < (sizeof(names) - used))) {
      used += static_cast<std::size_t>(written);
    }
  }
  std::printf("[web-lifecycle] tiers %s run=%d mask=%u open=%s\n", label, run,
              mask, (used == 0U) ? "none" : names);
  return mask;
}

/// Bootstraps and hands the loop to the browser. run() only returns when
/// it refuses to start; a started run unwinds out of this call.
void start_run(int run, std::uint32_t maxFrames) noexcept {
  if (!engine::bootstrap()) {
    std::printf("[web-lifecycle] bootstrap refused run=%d\n", run);
    return;
  }
  report_tiers("opened", run);
  if ((run == 1) && (g_case == LifecycleCase::FatalStage)) {
    static_cast<void>(engine::core::cvar_set_string("dbg_fail_frame_stage",
                                                    "simulation_graph"));
  }
  std::printf("[web-lifecycle] run started run=%d maxFrames=%u\n", run,
              maxFrames);
  const engine::RunResult result = engine::run(maxFrames);
  std::printf("[web-lifecycle] run refused run=%d result=%d\n", run,
              static_cast<int>(result));
}

void second_run(void *) noexcept { start_run(2, kSecondRunFrames); }

} // namespace

extern "C" {

/// True once emscripten_cancel_main_loop has run: the loop is over,
/// whatever web_frame did after cancelling it.
EMSCRIPTEN_KEEPALIVE int web_lifecycle_loop_ended() {
  return EM_ASM_INT({ return MainLoop.func ? 0 : 1; });
}

/// Frame index the running pipeline last published.
EMSCRIPTEN_KEEPALIVE unsigned web_lifecycle_frame() {
  return engine::core::log_current_frame_index();
}

/// Asks the platform to quit, as a closing page or a quit event would.
EMSCRIPTEN_KEEPALIVE void web_lifecycle_request_quit() {
  engine::core::request_platform_quit();
}

/// Reports the tiers still open after run `run` ended, and whether the
/// editor bridge player mode displaced came back; returns the open mask.
EMSCRIPTEN_KEEPALIVE unsigned web_lifecycle_report(int run) {
  std::printf("[web-lifecycle] bridge run=%d restored=%d\n", run,
              (engine::runtime::editor_bridge() == g_registeredBridge) ? 1
                                                                       : 0);
  return report_tiers("after", run);
}

/// Starts the second bootstrap from a fresh browser task, so its loop
/// hand-off unwinds into the event loop rather than into the caller.
EMSCRIPTEN_KEEPALIVE void web_lifecycle_second_run() {
  emscripten_async_call(&second_run, nullptr, 0);
}

/// Writes the save slot through the production path; 1 when it committed.
EMSCRIPTEN_KEEPALIVE int web_lifecycle_save() {
  static constexpr char kSave[] = "{\"coins\":8,\"won\":true}";
  return engine::runtime::save_game_data(kSave, sizeof(kSave) - 1U) ? 1 : 0;
}

/// Prints what the save slot holds, or that it holds nothing.
EMSCRIPTEN_KEEPALIVE void web_lifecycle_load() {
  char buffer[256] = {};
  std::size_t length = 0U;
  if (engine::runtime::load_game_data(buffer, sizeof(buffer), &length)) {
    std::printf("[web-lifecycle] loaded=%s\n", buffer);
  } else {
    std::printf("[web-lifecycle] loaded=none\n");
  }
}

} // extern "C"

/// Runs the case the page's URL named (the shell passes it through ENV).
int main() {
  char name[32] = {};
  static_cast<void>(
      engine::core::non_empty_env("ENGINE_WEB_TEST_CASE", name, sizeof(name)));
  std::uint32_t maxFrames = kSmokeFrames;
  if (std::strcmp(name, "save_persists") == 0) {
    // No run: the driver saves and loads through the exported calls.
    std::printf("[web-lifecycle] case=%s\n", name);
    return 0;
  }
  if (std::strcmp(name, "max_frames_1") == 0) {
    g_case = LifecycleCase::MaxFramesOne;
    maxFrames = 1U;
  } else if (std::strcmp(name, "quit") == 0) {
    g_case = LifecycleCase::Quit;
    maxFrames = 0U;
  } else if (std::strcmp(name, "fatal_stage") == 0) {
    g_case = LifecycleCase::FatalStage;
    maxFrames = 0U;
  } else if (std::strcmp(name, "frames") != 0) {
    std::printf("[web-lifecycle] unknown case '%s'\n", name);
    return 1;
  }
  std::printf("[web-lifecycle] case=%s\n", name);
  g_registeredBridge = engine::runtime::editor_bridge();
  start_run(1, maxFrames);
  return 0;
}
