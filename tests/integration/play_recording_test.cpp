// A play recording replays through the production pipeline and reports
// whether the replay reproduced it, checkpoint by checkpoint (#767 F2).
//
// The run is engine_integration_input_replay's: a script's on_fixed_tick
// spawns a marker per step placed by what the step read, so every step's
// input is in World::state_hash. A recording at three steps a frame is
// replayed at one, at three and at two (the last through the time scale),
// each with live key noise the replay must ignore; each report must
// compare every tick the two runs share and find no difference. A replay
// whose world starts from another random seed must diverge at tick 0, in
// the random section and no earlier one.
//
// The manifest is refused whole when it is truncated, another revision,
// malformed, out of order, miscounted or followed by bytes, or when its
// input log is missing or not the log it sealed; a refusal starts no
// replay. A recording never replaces an existing one, and one whose
// manifest cannot be written reports the failure and leaves what was at
// its path unchanged. A recording longer than the checkpoint storage keeps
// tick 0 and the last tick on a doubled stride, and its replay compares
// all of them.

#include "../asset_root.h"
#include "engine/core/cvar.h"
#include "engine/core/input.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/play_recording.h"
#include "engine/runtime/world.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

namespace rt = engine::runtime;

constexpr const char *kScriptPath = "play_recording_test.lua";
constexpr const char *kDemoPath = "play_recording_test.demo";
constexpr const char *kInputPath = "play_recording_test.input";
constexpr const char *kScratchDemo = "play_recording_scratch.demo";
constexpr const char *kScratchInput = "play_recording_scratch.input";
constexpr int kTicks = 18;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

bool write_text(const char *path, const std::string &text) noexcept {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

std::string read_text(const char *path) noexcept {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

/// One marker entity per step, at (what the step read, steps held so far,
/// 0): the read is down + 2 pressed + 4 action pressed.
bool write_script() noexcept {
  return write_text(
      kScriptPath,
      "local M = {}\n"
      "local held = 0\n"
      "function M.on_begin_play(self)\n"
      "  engine.register_action('play_recording_jump', engine.KEY_SPACE)\n"
      "end\n"
      "local function flag(value, weight)\n"
      "  if value then return weight end\n"
      "  return 0\n"
      "end\n"
      "function M.on_fixed_tick(self, dt)\n"
      "  local read = flag(engine.is_key_down(engine.KEY_SPACE), 1) +\n"
      "      flag(engine.is_key_pressed(engine.KEY_SPACE), 2) +\n"
      "      flag(engine.is_action_pressed('play_recording_jump'), 4)\n"
      "  held = held + flag(engine.is_key_down(engine.KEY_SPACE), 1)\n"
      "  local marker = engine.spawn_entity()\n"
      "  if marker ~= nil then\n"
      "    engine.set_position(marker, read, held, 0)\n"
      "  end\n"
      "end\n"
      "return M\n");
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

enum class Mode { Record, Replay };

/// One session of kTicks steps at `stepsPerFrame` (reached through the
/// time scale when it is not 1). `reseed` gives the world another random
/// seed before the first frame. Returns the replay's report.
rt::PlayReplayReport run(Mode mode, int stepsPerFrame, float timeScale = 1.0F,
                         bool reseed = false) noexcept {
  rt::PlayReplayReport report{};
  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    CHECK(false, "pipeline initialize");
    pipeline.teardown();
    return report;
  }
  CHECK(engine::core::cvar_set_float("sim.time_scale", timeScale),
        "set the time scale");
  CHECK(pipeline.set_frame_delta_override((stepsPerFrame / 60.0) /
                                          static_cast<double>(timeScale)),
        "delta override");
  if (reseed) {
    pipeline.world()->seed_random(0xC0FFEEU);
  }
  if (mode == Mode::Record) {
    CHECK(rt::begin_play_recording(kDemoPath), "begin the recording");
    CHECK(rt::play_recording_active() && engine::core::input_recording_active(),
          "the recording and its input log are open");
  } else {
    CHECK(rt::begin_play_replay(kDemoPath), "begin the replay");
    CHECK(rt::play_replay_active() && engine::core::input_replay_active(),
          "the replay and its input log are open");
  }
  for (int tick = 0; tick < kTicks; tick += stepsPerFrame) {
    if (mode == Mode::Replay) {
      CHECK(push_key((tick % 2) == 0, kNow), "push live noise");
    } else {
      for (int t = tick; t < tick + stepsPerFrame; ++t) {
        push_input_before_tick(t);
      }
    }
    CHECK(pipeline.execute_frame(), "frame");
  }
  report = rt::play_replay_report();
  CHECK(rt::end_play_log(), "the session's log ends cleanly");
  CHECK(!rt::play_recording_active() && !rt::play_replay_active(),
        "nothing stays open");
  CHECK(engine::core::cvar_set_float("sim.time_scale", 1.0F),
        "restore the time scale");
  pipeline.teardown();
  CHECK(push_key(false, kNow), "release the key");
  return report;
}

void check_record_and_replay() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kDemoPath, ec);
  std::filesystem::remove(kInputPath, ec);

  static_cast<void>(run(Mode::Record, 3));
  const std::string manifest = read_text(kDemoPath);
  CHECK(manifest.rfind("engine-play-recording 1\ninput ", 0) == 0,
        "the manifest opens with its revision and the input log's seal");
  CHECK(manifest.find("\ncheckpoints 7\n0 ") != std::string::npos,
        "it holds a checkpoint at tick 0 and at each of the six frames' ends");
  CHECK((manifest.size() > 4U) &&
            (manifest.compare(manifest.size() - 5U, 5U, "\nend\n") == 0),
        "and closes with its end line");

  const rt::PlayReplayReport one = run(Mode::Replay, 1);
  CHECK(one.finished && (one.recorded == 7U) && (one.compared == 7U) &&
            (one.mismatched == 0U) &&
            (one.firstDivergentSection == rt::PlayHashSection::None),
        "a replay at one step a frame meets every recorded checkpoint");
  const rt::PlayReplayReport three = run(Mode::Replay, 3);
  CHECK(three.finished && (three.compared == 7U) && (three.mismatched == 0U),
        "a replay at the recording's three steps a frame meets them all");
  const rt::PlayReplayReport fastForward = run(Mode::Replay, 2, 2.0F);
  CHECK(fastForward.finished && (fastForward.compared == 4U) &&
            (fastForward.mismatched == 0U),
        "a replay at time scale 2 meets the ticks it shares (0, 6, 12, 18)");

  const rt::PlayReplayReport reseeded = run(Mode::Replay, 3, 1.0F, true);
  CHECK(reseeded.finished && (reseeded.compared == 7U) &&
            (reseeded.mismatched == 7U) &&
            (reseeded.firstDivergentTick == 0U) &&
            (reseeded.firstDivergentSection == rt::PlayHashSection::Random),
        "a replay from another seed diverges at tick 0, first in random");
}

/// Begins a replay of a manifest holding `text`, beside the good input
/// log; it must be refused with nothing opened.
void check_refused_manifest(const std::string &text,
                            const char *what) noexcept {
  std::error_code ec{};
  std::filesystem::copy_file(kInputPath, kScratchInput,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  CHECK(!ec && write_text(kScratchDemo, text), "stage the scratch recording");
  CHECK(!rt::begin_play_replay(kScratchDemo), what);
  CHECK(!rt::play_replay_active() && !engine::core::input_replay_active(),
        "a refused replay opens nothing");
}

std::string replaced(std::string text, const std::string &from,
                     const std::string &to) noexcept {
  const std::size_t at = text.find(from);
  if (at != std::string::npos) {
    text.replace(at, from.size(), to);
  }
  return text;
}

void check_refusals() noexcept {
  const std::string good = read_text(kDemoPath);
  if (good.empty()) {
    CHECK(false, "the recording exists to refuse copies of");
    return;
  }
  check_refused_manifest(good.substr(0U, good.size() - 4U),
                         "a manifest without its end line is refused");
  check_refused_manifest(
      replaced(good, "engine-play-recording 1", "engine-play-recording 2"),
      "another revision is refused");
  check_refused_manifest(replaced(good, "checkpoints 7", "checkpoints 8"),
                         "a count the lines disagree with is refused");
  check_refused_manifest(good + "x", "bytes after the end line are refused");
  const std::size_t firstHash = good.find("\n0 ") + 3U;
  std::string upper = good;
  upper[firstHash] = 'G';
  check_refused_manifest(upper,
                         "a hash that is not 16 lowercase hex is refused");
  const std::size_t lastLine = good.rfind("\n18 ");
  std::string disordered = good;
  disordered.replace(lastLine + 1U, 2U, "15");
  check_refused_manifest(disordered, "ticks out of order are refused");
  const std::size_t sealAt = good.find("\ninput ");
  const std::size_t checksumAt = good.find(' ', sealAt + 7U) + 1U;
  std::string wrongSeal = good;
  wrongSeal[checksumAt] = (good[checksumAt] == '0') ? '1' : '0';
  check_refused_manifest(wrongSeal,
                         "an input log other than the one sealed is refused");

  std::error_code ec{};
  CHECK(write_text(kScratchDemo, good) &&
            std::filesystem::remove(kScratchInput, ec),
        "stage a manifest without its log");
  CHECK(!rt::begin_play_replay(kScratchDemo) && !rt::play_replay_active(),
        "a manifest whose log is missing is refused");
  CHECK(!rt::begin_play_replay("play_recording_absent.demo"),
        "a path with no recording is refused");

  CHECK(!rt::begin_play_recording(kDemoPath) && (read_text(kDemoPath) == good),
        "a recording is never begun over an existing one");
  CHECK(!rt::begin_play_recording("play_recording_test.txt") &&
            !engine::core::input_recording_active(),
        "a recording path that is not .demo is refused");
  CHECK(rt::begin_play_replay(kDemoPath) &&
            !rt::begin_play_recording("play_recording_other.demo") &&
            !rt::begin_play_replay(kDemoPath) && rt::end_play_log(),
        "nothing begins while a replay is open");
  std::filesystem::remove(kScratchDemo, ec);
}

void check_failed_manifest_write() noexcept {
  std::error_code ec{};
  std::filesystem::remove_all(kScratchDemo, ec);
  std::filesystem::remove(kScratchInput, ec);
  CHECK(rt::begin_play_recording(kScratchDemo), "begin a scratch recording");
  // A directory where the manifest goes: the staged replace cannot land.
  CHECK(std::filesystem::create_directory(kScratchDemo, ec),
        "block the manifest's path");
  CHECK(!rt::end_play_log(), "the unsaved recording is reported");
  CHECK(std::filesystem::is_directory(kScratchDemo, ec) &&
            !rt::play_recording_active(),
        "what was at the manifest's path is unchanged and nothing stays open");
  std::filesystem::remove_all(kScratchDemo, ec);
  std::filesystem::remove(kScratchInput, ec);
}

void check_long_recording() noexcept {
  std::unique_ptr<rt::World> world(new (std::nothrow) rt::World());
  if (world == nullptr) {
    CHECK(false, "allocate a world");
    return;
  }
  constexpr std::uint64_t kLastTick = 20000U;
  std::error_code ec{};
  std::filesystem::remove(kScratchDemo, ec);
  std::filesystem::remove(kScratchInput, ec);
  CHECK(rt::begin_play_recording(kScratchDemo), "begin a long recording");
  for (std::uint64_t tick = 0U; tick <= kLastTick; ++tick) {
    rt::observe_play_checkpoint(tick, *world);
    rt::observe_play_checkpoint(tick, *world);
  }
  CHECK(rt::end_play_log(), "the long recording is saved");
  const std::string manifest = read_text(kScratchDemo);
  // Ticks 0-16383 fill it; the doubled stride keeps the 8192 even ones,
  // then records 16384-20000 two apart: 8192 + 1809.
  CHECK(manifest.find("\ncheckpoints 10001\n0 ") != std::string::npos,
        "a full recording thins to a doubled stride, tick 0 kept");
  CHECK(manifest.find("\n20000 ") != std::string::npos &&
            manifest.find("\n19999 ") == std::string::npos,
        "and records on that stride to its last tick");
  CHECK(rt::begin_play_replay(kScratchDemo), "replay the long recording");
  for (std::uint64_t tick = 0U; tick <= kLastTick; ++tick) {
    rt::observe_play_checkpoint(tick, *world);
  }
  const rt::PlayReplayReport report = rt::play_replay_report();
  CHECK(report.finished && (report.recorded == 10001U) &&
            (report.compared == 10001U) && (report.mismatched == 0U),
        "its replay compares every checkpoint it kept");
  CHECK(rt::end_play_log(), "the replay ends");
  std::filesystem::remove(kScratchDemo, ec);
  std::filesystem::remove(kScratchInput, ec);
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root()) {
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

  check_record_and_replay();
  check_refusals();
  check_failed_manifest_write();
  check_long_recording();

  engine::shutdown();
  std::remove(kScriptPath);
  std::remove(kDemoPath);
  std::remove(kInputPath);

  if (g_failures != 0) {
    std::fprintf(stderr, "play_recording_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("play_recording_test: all checks passed\n");
  return 0;
}
