// A recorded run replays step for step at another frame schedule (#679).
// Which fixed step an event lands in follows its wall-clock timestamp, so
// before the input log the same key sequence could simulate differently on
// two runs, and nothing recorded what each step read.
//
// Drives the production pipeline and pump with native events pushed into
// the queue. The script's on_fixed_tick spawns one marker entity per step
// and places it by what the step read -- the key held and pressed, and an
// action mapped to it -- so every step's input is in the world, and
// World::state_hash at a frame boundary covers every step before it.
//
// The recording runs three steps a frame: a tap stamped at the window's two
// ends (so its press and release land in different steps of one frame) and
// a press and release stamped by SDL at push time, whose steps depend on the
// wall clock. Two replays then run with no input but live noise: one at one
// step a frame, hashed at every tick, and one at three. Both must reach the
// recorded hash at every tick the recording was observed, and each other's
// at every tick they share. A control runs the same pushes live at one step
// a frame and must diverge, which is what a replay that fell back to live
// events would do.

#include "engine/core/input.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {

constexpr const char *kScriptPath = "input_replay_test.lua";
constexpr const char *kLogPath = "input_replay_test.inputlog";
constexpr int kTicks = 18;

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

/// One marker entity per step, at (what the step read, steps held so far,
/// 0): the read is down + 2 pressed + 4 action pressed.
bool write_script() noexcept {
  std::ofstream out(kScriptPath, std::ios::trunc);
  out << "local M = {}\n"
         "local held = 0\n"
         "function M.on_begin_play(self)\n"
         "  engine.register_action('input_replay_jump', engine.KEY_SPACE)\n"
         "end\n"
         "local function flag(value, weight)\n"
         "  if value then return weight end\n"
         "  return 0\n"
         "end\n"
         "function M.on_fixed_tick(self, dt)\n"
         "  local read = flag(engine.is_key_down(engine.KEY_SPACE), 1) +\n"
         "      flag(engine.is_key_pressed(engine.KEY_SPACE), 2) +\n"
         "      flag(engine.is_action_pressed('input_replay_jump'), 4)\n"
         "  held = held + flag(engine.is_key_down(engine.KEY_SPACE), 1)\n"
         "  local marker = engine.spawn_entity()\n"
         "  if marker ~= nil then\n"
         "    engine.set_position(marker, read, held, 0)\n"
         "  end\n"
         "end\n"
         "return M\n";
  return static_cast<bool>(out);
}

/// Pushes a Space event. A zero stamp lets SDL stamp it at push time.
bool push_key(bool down, std::uint64_t timestampNs) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.common.timestamp = timestampNs;
  event.key.scancode = SDL_SCANCODE_SPACE;
  event.key.down = down;
  return SDL_PushEvent(&event);
}

constexpr std::uint64_t kWindowStart = 1U;
constexpr std::uint64_t kWindowEnd = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kNow = 0U;

/// The input the recording and the control push, by the tick the frame
/// after the push starts at.
void push_input_before_tick(int tick) noexcept {
  switch (tick) {
  case 3:
    CHECK(push_key(true, kWindowStart), "push the tap's press");
    CHECK(push_key(false, kWindowEnd), "push the tap's release");
    break;
  case 6:
    CHECK(push_key(true, kNow), "push the held press");
    break;
  case 12:
    CHECK(push_key(false, kNow), "push the held release");
    break;
  default:
    break;
  }
}

/// World::state_hash after each tick a run observed, or none.
struct TickHashes final {
  std::vector<std::uint64_t> hash = std::vector<std::uint64_t>(kTicks + 1, 0U);
  std::vector<bool> seen = std::vector<bool>(kTicks + 1, false);
};

enum class Mode { Record, Replay, Live };

/// One run of kTicks steps at `stepsPerFrame`, hashed after every frame.
/// Record pushes the input and records it; Replay pushes only noise the
/// replay must ignore; Live pushes the input with no log at all.
TickHashes run(Mode mode, int stepsPerFrame) noexcept {
  TickHashes hashes{};
  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    CHECK(false, "pipeline initialize");
    pipeline.teardown();
    return hashes;
  }
  CHECK(pipeline.set_frame_delta_override(stepsPerFrame / 60.0),
        "delta override");
  if (mode == Mode::Record) {
    CHECK(engine::core::begin_input_recording(kLogPath), "begin recording");
  } else if (mode == Mode::Replay) {
    CHECK(engine::core::begin_input_replay(kLogPath), "begin replay");
  }
  const std::size_t aliveBefore = pipeline.world()->alive_entity_count();
  for (int tick = 0; tick < kTicks; tick += stepsPerFrame) {
    if (mode == Mode::Replay) {
      // Live input the replayed steps must not read.
      CHECK(push_key((tick % 2) == 0, kNow), "push live noise");
      CHECK(engine::core::input_replay_active(),
            "the replay runs until its last tick");
    } else {
      for (int t = tick; t < tick + stepsPerFrame; ++t) {
        push_input_before_tick(t);
      }
    }
    CHECK(pipeline.execute_frame(), "frame");
    const int reached = tick + stepsPerFrame;
    CHECK(pipeline.world()->alive_entity_count() ==
              aliveBefore + static_cast<std::size_t>(reached),
          "every step spawned its marker");
    hashes.hash[reached] = pipeline.world()->state_hash();
    hashes.seen[reached] = true;
  }
  if (mode == Mode::Record) {
    CHECK(engine::core::end_input_recording(), "the recording commits");
  } else if (mode == Mode::Replay) {
    CHECK(!engine::core::input_replay_active(),
          "the replay ends with the log's last tick");
  }
  pipeline.teardown();
  // Leave the key up for the next run.
  CHECK(push_key(false, kNow), "release the key");
  return hashes;
}

/// Checks `a` against `b` at every tick both observed; the count of shared
/// ticks is returned so the caller can require coverage.
int compare(const TickHashes &a, const TickHashes &b,
            const char *what) noexcept {
  int shared = 0;
  for (int tick = 1; tick <= kTicks; ++tick) {
    if (!a.seen[tick] || !b.seen[tick]) {
      continue;
    }
    ++shared;
    if (a.hash[tick] != b.hash[tick]) {
      std::fprintf(stderr, "FAIL: %s differ at tick %d: %016llx vs %016llx\n",
                   what, tick, static_cast<unsigned long long>(a.hash[tick]),
                   static_cast<unsigned long long>(b.hash[tick]));
      ++g_failures;
    }
  }
  return shared;
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

  const TickHashes recorded = run(Mode::Record, 3);
  const TickHashes replayOne = run(Mode::Replay, 1);
  const TickHashes replayThree = run(Mode::Replay, 3);
  const TickHashes live = run(Mode::Live, 1);

  CHECK(compare(replayOne, recorded, "replay at 1 step/frame vs recording") ==
            kTicks / 3,
        "the one-step replay meets the recording at every tick it observed");
  CHECK(compare(replayThree, recorded,
                "replay at 3 steps/frame vs recording") == kTicks / 3,
        "the three-step replay meets it too");
  CHECK(compare(replayOne, replayThree, "the two replays") == kTicks / 3,
        "and the replays meet each other");
  int observed = 0;
  for (int tick = 1; tick <= kTicks; ++tick) {
    observed += replayOne.seen[tick] ? 1 : 0;
  }
  CHECK(observed == kTicks, "the one-step replay was hashed at every tick");
  // Without the log the tap lands in one step at one step a frame: pressed
  // there but never held, so neither the key nor the action reads down and
  // the marker for tick 3 differs.
  CHECK(live.seen[6] && recorded.seen[6] && (live.hash[6] != recorded.hash[6]),
        "the same input run live at one step a frame diverges, so matching "
        "the recording is the log's doing");

  engine::shutdown();
  std::remove(kScriptPath);
  std::remove(kLogPath);

  if (g_failures != 0) {
    std::fprintf(stderr, "input_replay_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("input_replay_test: all checks passed\n");
  return 0;
}
