// on_fixed_tick sees each fixed step's own input (#653). A frame that
// catches up several fixed steps used to hand every step the same input,
// so a gameplay reaction to a tap -- a jump impulse -- ran once per step:
// three times as strong at 20 frames a second as at 60, and not
// reproducible from what the simulation read.
//
// Drives the production pipeline and pump with native events pushed into
// the queue, and a Lua script whose on_fixed_tick counts presses of a key
// and of an action mapped to it. The same tap runs at one fixed step per
// frame and at three; both must count one press, and both must have run
// the same number of steps.

#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/scripting/bindable_api.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

constexpr const char *kScriptPath = "fixed_tick_input_test.lua";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

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
        std::filesystem::exists(
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// Counts, per fixed step, the key presses and the presses of an action
/// mapped to the same key, and publishes them with the step count.
bool write_script() noexcept {
  std::ofstream out(kScriptPath, std::ios::trunc);
  out << "local M = {}\n"
         "local ticks, keys, actions = 0, 0, 0\n"
         "function M.on_begin_play(self)\n"
         "  engine.register_action('fixed_tick_jump', engine.KEY_SPACE)\n"
         "end\n"
         "function M.on_fixed_tick(self, dt)\n"
         "  ticks = ticks + 1\n"
         "  if engine.is_key_pressed(engine.KEY_SPACE) then\n"
         "    keys = keys + 1\n"
         "  end\n"
         "  if engine.is_action_pressed('fixed_tick_jump') then\n"
         "    actions = actions + 1\n"
         "  end\n"
         "  engine.set_game_state(string.format('%d %d %d', ticks, keys,\n"
         "                                      actions))\n"
         "end\n"
         "return M\n";
  return static_cast<bool>(out);
}

bool push_key(bool down) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.scancode = SDL_SCANCODE_SPACE;
  event.key.down = down;
  return SDL_PushEvent(&event);
}

struct Counts final {
  int ticks = -1;
  int keys = -1;
  int actions = -1;
};

/// Parses the "ticks keys actions" game state the script publishes.
Counts read_counts() noexcept {
  const char *cursor = engine::scripting::bindable_get_game_state();
  if (cursor == nullptr) {
    return Counts{};
  }
  int values[3] = {};
  for (int &value : values) {
    char *end = nullptr;
    const long parsed = std::strtol(cursor, &end, 10);
    if (end == cursor) {
      return Counts{};
    }
    value = static_cast<int>(parsed);
    cursor = end;
  }
  Counts counts{};
  counts.ticks = values[0];
  counts.keys = values[1];
  counts.actions = values[2];
  return counts;
}

/// One run: settles, taps the key inside one frame's batch, then runs on
/// until `totalSteps` fixed steps have simulated since the settle frame.
/// Every frame simulates `stepsPerFrame` steps through the delta override.
Counts run(int stepsPerFrame, int totalSteps) noexcept {
  Counts counts{};
  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    pipeline.teardown();
    return counts;
  }
  CHECK(pipeline.set_frame_delta_override(stepsPerFrame / 60.0),
        "delta override");
  CHECK(pipeline.execute_frame(), "settle frame");
  const Counts settled = read_counts();
  CHECK(settled.ticks == stepsPerFrame, "the settle frame ran its steps");

  CHECK(push_key(true), "push Space down");
  CHECK(push_key(false), "push Space up");
  for (int steps = 0; steps < totalSteps; steps += stepsPerFrame) {
    CHECK(pipeline.execute_frame(), "frame");
  }
  counts = read_counts();
  counts.ticks -= settled.ticks;
  pipeline.teardown();
  return counts;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_script()) {
    std::fprintf(stderr, "FAIL: write the test script\n");
    return 1;
  }
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  config.mainScriptPath = kScriptPath;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    std::remove(kScriptPath);
    return 2;
  }

  constexpr int kSteps = 6;
  const Counts one = run(1, kSteps);
  const Counts three = run(3, kSteps);

  CHECK(one.ticks == kSteps, "one step per frame ran every step");
  CHECK(three.ticks == kSteps, "three steps per frame ran every step");
  CHECK(one.keys == 1, "at one step per frame the tap is one key press");
  CHECK(three.keys == 1,
        "at three steps per frame the tap is still one key press, not one "
        "per step");
  CHECK(one.actions == 1, "at one step per frame the tap is one action press");
  CHECK(three.actions == 1,
        "at three steps per frame the tap is still one action press");
  CHECK((one.ticks == three.ticks) && (one.keys == three.keys) &&
            (one.actions == three.actions),
        "the same input reaches the same state at either frame rate");

  engine::shutdown();
  std::remove(kScriptPath);

  if (g_failures != 0) {
    std::fprintf(stderr, "fixed_tick_input_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fixed_tick_input_test: all checks passed\n");
  return 0;
}
