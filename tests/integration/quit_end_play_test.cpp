// Regression for #241 (owner decision 2026-08-19): a quit that ends a live
// play session must dispatch on_end_play before teardown, exactly like
// editor Stop and scene transitions — before the fix, closing the window
// mid-play silently skipped every script's end hook (and any save it
// performs). Drives real engine::bootstrap() + EnginePipeline frames (the
// pipeline_tick_cadence_test.cpp pattern) and delivers the quit as a real
// SDL_EVENT_QUIT through the production input stage.

#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

constexpr const char *kScriptPath = "quit_end_play_test.lua";

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can author entities between frames.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as pipeline_tick_cadence_test.cpp).
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

    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

// on_begin_play marks the session live; on_end_play marks the quit-time
// dispatch this regression exists to prove.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.set_game_state(\"in_progress\")\n"
    "end\n"
    "function M.on_end_play(self)\n"
    "    engine.set_game_state(\"ended\")\n"
    "end\n"
    "return M\n";

bool write_script_file() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::char_traits<char>::length(kScript);
  const std::size_t written = std::fwrite(kScript, 1U, length, file);
  std::fclose(file);
  return written == length;
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kScriptPath));
}

/// Runs one playing frame guaranteed to simulate at least one fixed step
/// (see pipeline_tick_cadence_test.cpp on the wall-clock accumulator).
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

/// Compares the live game-state label against an expected value.
bool game_state_is(const char *expected) noexcept {
  const char *actual = engine::scripting::bindable_get_game_state();
  return (actual != nullptr) && (std::strcmp(actual, expected) == 0);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_script_file()) {
    std::fprintf(stderr, "FAIL: write script file\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  if (!engine::bootstrap()) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_script_file();
    return 2;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U) || (g_world == nullptr)) {
    std::fprintf(stderr, "FAIL: pipeline initialize\n");
    pipeline.teardown();
    engine::shutdown();
    remove_script_file();
    return 3;
  }

  CHECK(ticking_frame(pipeline), "settle frame 1");
  CHECK(ticking_frame(pipeline), "settle frame 2");
  const engine::runtime::Entity scripted = g_world->create_scene_object();
  CHECK(scripted != engine::runtime::kInvalidEntity, "spawn scripted");
  engine::runtime::ScriptComponent sc{};
  std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kScriptPath);
  CHECK(g_world->add_script_component(scripted, sc), "attach script");
  CHECK(ticking_frame(pipeline), "script frame 1");
  CHECK(ticking_frame(pipeline), "script frame 2");
  CHECK(game_state_is("in_progress"), "session is live before quit");

  // The production quit path: a real SDL_EVENT_QUIT through stage_input.
  SDL_Event quitEvent{};
  quitEvent.type = SDL_EVENT_QUIT;
  CHECK(SDL_PushEvent(&quitEvent), "push quit event");

  // The loop must terminate on its own within a bounded number of frames.
  bool exited = false;
  for (int i = 0; i < 10; ++i) {
    if (!pipeline.execute_frame()) {
      exited = true;
      break;
    }
  }
  CHECK(exited, "quit terminates the frame loop");

  // The regression proper: on_end_play ran on the way out.
  CHECK(game_state_is("ended"), "quit dispatched on_end_play");

  pipeline.teardown();
  engine::shutdown();
  remove_script_file();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("quit_end_play_test passed");
  return 0;
}
