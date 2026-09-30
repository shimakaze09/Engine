// Regression for #803: a play session owns the audio its scripts load.
// Drives real engine::bootstrap() + EnginePipeline frames through 40 Play
// and Stop cycles, the editor's transitions recorded by a bridge, with a
// script that loads the island sample's sounds, plays one on a loop and
// starts the looping music in on_begin_play. After every Stop the sound
// registry is back to what it held before Play and no music is open;
// before the fix each session left its sounds loaded (the 37th Play of the
// sample filled the registry) and the music played on in edit mode.

#include "../asset_root.h"
#include "audio_diagnostics.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char *kScriptPath = "play_session_audio_test.lua";
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr int kCycles = 40;

engine::runtime::World *g_world = nullptr;

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool g_playing = false;
bool bridge_is_playing() noexcept { return g_playing; }
bool bridge_is_paused() noexcept { return false; }

constexpr std::size_t kMaxQueued = 4U;
std::array<engine::runtime::PlayTransition, kMaxQueued> g_queued{};
std::size_t g_queuedCount = 0U;

bool bridge_consume_play_transition(
    engine::runtime::PlayTransition *outTransition) noexcept {
  if ((outTransition == nullptr) || (g_queuedCount == 0U)) {
    return false;
  }
  *outTransition = g_queued[0];
  for (std::size_t i = 1U; i < g_queuedCount; ++i) {
    g_queued[i - 1U] = g_queued[i];
  }
  --g_queuedCount;
  return true;
}

void queue_transition(engine::runtime::PlayTransition transition) noexcept {
  if (g_queuedCount < kMaxQueued) {
    g_queued[g_queuedCount++] = transition;
  }
}

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

// What a game script does at the start of play: load its sounds, keep one
// looping, and start the level's music.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    local pickup = engine.load_sound('assets/sounds/pickup.wav')\n"
    "    engine.load_sound('assets/sounds/win.wav')\n"
    "    engine.load_sound('assets/sounds/splash.wav')\n"
    "    engine.play_sound(pickup, 1.0, 1.0, true)\n"
    "    engine.play_music('assets/sounds/waves.wav', 1.0, true)\n"
    "end\n"
    "return M\n";

bool write_script_file(const char *path, const char *text) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(text);
  const bool ok = (std::fwrite(text, 1U, length, file) == length);
  static_cast<void>(std::fclose(file));
  return ok;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_script_file(kScriptPath, kScript)) {
    std::fprintf(stderr, "FAIL: write the script file\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  bridge.consume_play_transition = &bridge_consume_play_transition;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    static_cast<void>(std::remove(kScriptPath));
    return 2;
  }

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      static_cast<void>(std::remove(kScriptPath));
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(kFrameSeconds),
          "frame delta override");
    CHECK(pipeline.execute_frame(), "settle frame");
    const engine::runtime::Entity entity = g_world->create_scene_object();
    engine::runtime::ScriptComponent script{};
    std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                  kScriptPath);
    CHECK((entity != engine::runtime::kInvalidEntity) &&
              g_world->add_script_component(entity, script),
          "spawn the scripted entity");
    CHECK(pipeline.execute_frame(), "stopped frame with the entity");

    const std::size_t loadedBeforePlay =
        engine::audio::audio_loaded_sound_count();
    bool everySessionLoaded = true;
    bool everyStopReleased = true;
    int firstLeak = -1;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
      queue_transition(engine::runtime::PlayTransition::Start);
      g_playing = true;
      for (int frame = 0; frame < 3; ++frame) {
        static_cast<void>(pipeline.execute_frame());
      }
      everySessionLoaded =
          everySessionLoaded &&
          (engine::audio::audio_loaded_sound_count() == loadedBeforePlay + 3U) &&
          engine::audio::audio_music_active();

      queue_transition(engine::runtime::PlayTransition::Stop);
      g_playing = false;
      static_cast<void>(pipeline.execute_frame());
      const bool released =
          (engine::audio::audio_loaded_sound_count() == loadedBeforePlay) &&
          !engine::audio::audio_music_active();
      if (!released && (firstLeak < 0)) {
        firstLeak = cycle;
        std::fprintf(stderr,
                     "       after Stop %d: %zu sounds loaded (was %zu), "
                     "music %s\n",
                     cycle + 1,
                     engine::audio::audio_loaded_sound_count(),
                     loadedBeforePlay,
                     engine::audio::audio_music_active() ? "open" : "closed");
      }
      everyStopReleased = everyStopReleased && released;
    }
    CHECK(everySessionLoaded,
          "every session loads its three sounds and starts its music");
    CHECK(everyStopReleased,
          "every Stop returns the registry to its state before Play and "
          "closes the music");

    pipeline.teardown();
  }
  engine::shutdown();
  static_cast<void>(std::remove(kScriptPath));
  if (g_failures != 0) {
    return 10;
  }
  std::printf("play session audio: %d Play/Stop cycles released their audio\n",
              kCycles);
  return 0;
}
