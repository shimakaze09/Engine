// The acceptance demo for the first product slice: the collect-a-thon
// authored in the editor from the bundled kit (assets/coin_run.scene)
// is loaded through the pipeline's deferred scene operation and played
// start to finish, headless, by real key events through the production
// input stage — eight coins, the moving platform across the gap, the flag.
// Every frame carries an injected fixed delta, so the run is one fixed
// step a frame and the route is counted in steps, never in time.
//
// It passes when all eight coins are gone, the controller script has
// announced the win exactly once, and a second play of the same scene ends
// on the same World::state_hash. Run with --windowed
// (engine_integration_coin_run_acceptance_gpu) it then plays the route again
// in a real window with a render device drawing every frame, and that play
// must end on the headless play's hash. What the frame looks like and what
// a speaker would play stay an observation on real hardware.

#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

constexpr const char *kScene = "assets/coin_run.scene";
constexpr const char *kMainScript = "coin_run_acceptance_main.lua";
constexpr const char *kSaveRoot = "coin_run_acceptance_profile";
constexpr double kStepSeconds = 1.0 / 60.0;
constexpr int kCoinCount = 8;

engine::runtime::World *g_world = nullptr;
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool g_playing = false;
bool bridge_is_playing() noexcept { return g_playing; }
bool bridge_is_paused() noexcept { return false; }

int g_winsAnnounced = 0;
int g_scriptErrors = 0;

/// The controller script's only outward sign of the win is its log line.
void watch_log(engine::core::LogLevel, const char *, const char *message,
               void *) noexcept {
  if (message == nullptr) {
    return;
  }
  if (std::strstr(message, "YOU WIN!") != nullptr) {
    ++g_winsAnnounced;
  }
  if (std::strstr(message, "lua error") != nullptr) {
    ++g_scriptErrors;
  }
}

bool set_working_directory_with_assets() noexcept {
  std::error_code ec{};
  const std::filesystem::path original = std::filesystem::current_path(ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    ec.clear();
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec || !std::filesystem::exists(normalized / kScene, ec)) {
      continue;
    }
    std::filesystem::current_path(normalized, ec);
    return !ec;
  }
  return false;
}

/// The controller keeps a best time through engine.save_data, which writes
/// under the per-user save directory. The run gets a profile of its own so
/// it neither reads nor replaces the developer's.
bool redirect_user_profile() noexcept {
  std::error_code ec{};
  const std::filesystem::path root =
      std::filesystem::absolute(std::filesystem::path(kSaveRoot), ec);
  if (ec) {
    return false;
  }
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return false;
  }
  const std::string text = root.string();
#if defined(_WIN32)
  return _putenv_s("APPDATA", text.c_str()) == 0;
#elif defined(__APPLE__)
  return setenv("HOME", text.c_str(), 1) == 0;
#else
  return setenv("XDG_DATA_HOME", text.c_str(), 1) == 0;
#endif
}

bool write_empty_main_script() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kMainScript, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kMainScript, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const char text[] =
      "-- Empty on purpose: the scene's own scripts are the game.\n";
  const bool ok = std::fwrite(text, 1U, sizeof(text) - 1U, file) ==
                  (sizeof(text) - 1U);
  return (std::fclose(file) == 0) && ok;
}

void remove_files() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kMainScript, ec);
  std::filesystem::remove_all(kSaveRoot, ec);
}

bool frame(engine::EnginePipeline &pipeline) noexcept {
  return pipeline.set_frame_delta_override(kStepSeconds) &&
         pipeline.execute_frame();
}

bool push_key(SDL_Scancode scancode, bool down) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.scancode = scancode;
  event.key.down = down;
  return SDL_PushEvent(&event);
}

/// Holds one key for `steps` fixed steps, then lets a step pass with it up
/// so the next leg starts from rest. The player moves 4.5 m/s, 0.075 m a
/// step.
bool hold(engine::EnginePipeline &pipeline, SDL_Scancode scancode,
          int steps) noexcept {
  if (!push_key(scancode, true)) {
    return false;
  }
  for (int i = 0; i < steps; ++i) {
    if (!frame(pipeline)) {
      return false;
    }
  }
  return push_key(scancode, false) && frame(pipeline);
}

bool idle(engine::EnginePipeline &pipeline, int steps) noexcept {
  for (int i = 0; i < steps; ++i) {
    if (!frame(pipeline)) {
      return false;
    }
  }
  return true;
}

int coins_left() noexcept {
  int left = 0;
  for (int i = 1; i <= kCoinCount; ++i) {
    char name[16] = {};
    std::snprintf(name, sizeof(name), "Coin%d", i);
    if (g_world->find_entity_by_name(name) != engine::runtime::kInvalidEntity) {
      ++left;
    }
  }
  return left;
}

bool player_position(engine::math::Vec3 *out) noexcept {
  const engine::runtime::Entity player = g_world->find_entity_by_name("Player");
  engine::runtime::Transform transform{};
  if ((player == engine::runtime::kInvalidEntity) ||
      !g_world->get_transform(player, &transform)) {
    return false;
  }
  *out = transform.position;
  return true;
}

struct PlayResult final {
  bool ran = false;
  int coinsLeft = kCoinCount;
  int wins = 0;
  std::uint64_t hash = 0U;
  engine::math::Vec3 finalPosition{};
};

/// Loads the scene and plays the route. The platform sweeps on a four
/// second cycle from the first played step: near end at 0 s, far end at
/// 2 s. Its phase is counted in the steps played since begin-play.
PlayResult play_once(engine::EnginePipeline &pipeline) noexcept {
  PlayResult result{};
  g_playing = false;
  engine::runtime::reset_world(*g_world);
  if (!engine::scripting::request_scene_load(kScene) || !frame(pipeline)) {
    return result;
  }
  if (coins_left() != kCoinCount) {
    std::fprintf(stderr, "FAIL: the loaded scene holds %d of %d coins\n",
                 coins_left(), kCoinCount);
    return result;
  }

  g_winsAnnounced = 0;
  g_playing = true;
  int played = 0;
  const auto run = [&](SDL_Scancode key, int steps) noexcept {
    played += steps + 1;
    return hold(pipeline, key, steps);
  };
  const auto wait = [&](int steps) noexcept {
    played += steps;
    return idle(pipeline, steps);
  };
  const auto wait_for_phase = [&](int phaseStep) noexcept {
    const int now = played % 240;
    return wait((phaseStep - now + 240) % 240);
  };

  // Settle onto the ground, then the six coins of the start island.
  bool ok = wait(30);
  ok = ok && run(SDL_SCANCODE_W, 27);  // Coin1 (0, 3)
  ok = ok && run(SDL_SCANCODE_A, 40) && run(SDL_SCANCODE_W, 27);  // Coin2
  ok = ok && run(SDL_SCANCODE_A, 27) && run(SDL_SCANCODE_W, 40);  // Coin3
  ok = ok && run(SDL_SCANCODE_D, 40) && run(SDL_SCANCODE_W, 27);  // Coin4
  ok = ok && run(SDL_SCANCODE_D, 67) && run(SDL_SCANCODE_S, 13);  // Coin5
  ok = ok && run(SDL_SCANCODE_S, 40) && run(SDL_SCANCODE_D, 33);  // Coin6
  ok = ok && run(SDL_SCANCODE_D, 28);  // to the edge, past the falling rock
  // Board as the platform reaches the near end, ride to the far end.
  ok = ok && wait_for_phase(225) && run(SDL_SCANCODE_D, 19);
  ok = ok && wait_for_phase(120) && run(SDL_SCANCODE_D, 23);
  // The goal island: two coins, then the flag.
  ok = ok && run(SDL_SCANCODE_D, 11) && run(SDL_SCANCODE_S, 20);  // Coin7
  ok = ok && run(SDL_SCANCODE_D, 13) && run(SDL_SCANCODE_W, 40);  // Coin8
  ok = ok && run(SDL_SCANCODE_S, 13) && wait(30);
  if (!ok) {
    return result;
  }

  result.ran = true;
  result.coinsLeft = coins_left();
  result.wins = g_winsAnnounced;
  result.hash = g_world->state_hash();
  static_cast<void>(player_position(&result.finalPosition));

  // Stop: end play before the next load replaces the world.
  g_playing = false;
  result.ran = frame(pipeline);
  return result;
}

struct SessionResult final {
  bool bootstrapped = false;
  bool pipelineReady = false;
  bool sinkRegistered = false;
  PlayResult first{};
  PlayResult second{};
};

/// Bootstraps the engine, plays the route twice and shuts the engine down.
/// `headless` selects the platform: a hidden dummy window with no device
/// work, or a real window and render device that draws every frame.
SessionResult run_session(bool headless) noexcept {
  SessionResult session{};
  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = headless;
  config.mainScriptPath = kMainScript;
  session.bootstrapped = engine::bootstrap(config);
  if (!session.bootstrapped) {
    engine::runtime::set_editor_bridge(nullptr);
    return session;
  }
  session.sinkRegistered = engine::core::log_register_sink(&watch_log, nullptr);
  g_winsAnnounced = 0;
  g_scriptErrors = 0;
  {
    engine::EnginePipeline pipeline;
    session.pipelineReady = pipeline.initialize(0U) && (g_world != nullptr);
    if (session.pipelineReady) {
      session.first = play_once(pipeline);
      session.second = play_once(pipeline);
    }
    pipeline.teardown();
  }
  engine::core::log_unregister_sink(&watch_log, nullptr);
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  g_world = nullptr;
  return session;
}

void print_session(const char *label, const SessionResult &session) noexcept {
  const PlayResult &first = session.first;
  const PlayResult &second = session.second;
  std::printf("coin_run_acceptance_test (%s): first run %d coins left, %d "
              "win(s), player at (%.3f, %.3f, %.3f), hash %llu; second run "
              "%d left, %d win(s), hash %llu\n",
              label, first.coinsLeft, first.wins,
              static_cast<double>(first.finalPosition.x),
              static_cast<double>(first.finalPosition.y),
              static_cast<double>(first.finalPosition.z),
              static_cast<unsigned long long>(first.hash), second.coinsLeft,
              second.wins, static_cast<unsigned long long>(second.hash));
}

/// Checks one session's plays; returns the number of failed checks.
int check_session(const SessionResult &session) noexcept {
  int failures = 0;
  const auto check = [&failures](bool condition, const char *what) noexcept {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  };
  check(session.bootstrapped, "bootstrap");
  check(session.pipelineReady, "pipeline initialize");
  check(session.sinkRegistered, "the log sink registered");
  check(session.first.ran && session.second.ran,
        "both plays ran to the end of the route");
  check(session.first.coinsLeft == 0,
        "the first play collected all eight coins");
  check(session.first.wins == 1,
        "the first play announced the win exactly once");
  check(session.second.coinsLeft == 0,
        "the second play collected all eight coins");
  check(session.second.wins == 1,
        "the second play announced the win exactly once");
  check(session.first.hash == session.second.hash,
        "two plays of the scene end on the same state hash");
  check(g_scriptErrors == 0, "no script raised an error");
  return failures;
}

} // namespace

/// Runs this executable or test program. With `--windowed` the route is
/// played headless and then again in a real window with a render device,
/// and both must end on one state hash: drawing frames must not change
/// what the simulation computes.
int main(int argc, char **argv) {
  const bool windowed = (argc > 1) && (std::strcmp(argv[1], "--windowed") == 0);
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate %s\n", kScene);
    return 1;
  }
  if (!redirect_user_profile() || !write_empty_main_script()) {
    std::fprintf(stderr, "FAIL: could not prepare the run's files\n");
    remove_files();
    return 1;
  }

  const SessionResult headless = run_session(true);
  print_session("headless", headless);
  int failures = check_session(headless);
  if (windowed && (failures == 0)) {
    const SessionResult rendered = run_session(false);
    print_session("windowed", rendered);
    failures += check_session(rendered);
    if (rendered.first.hash != headless.first.hash) {
      std::fprintf(stderr,
                   "FAIL: the windowed play ends on hash %llu, the "
                   "headless play on %llu\n",
                   static_cast<unsigned long long>(rendered.first.hash),
                   static_cast<unsigned long long>(headless.first.hash));
      ++failures;
    }
  }
  remove_files();

  if (failures != 0) {
    return 10;
  }
  std::printf("coin_run_acceptance_test: all checks passed\n");
  return 0;
}
