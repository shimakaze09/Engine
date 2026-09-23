// The input log (#679) at its boundaries: a recorded run replays what each
// step read, a reset's previous snapshot included; an empty log, a single
// step, the drain ring at and one past its capacity, and several rings'
// worth of steps; and refusal of an unknown version, a truncated or
// damaged file and a misaligned replay, each of which changes nothing.
// Timestamps sit at the window's ends, as in input_steps_test, so which step
// an event lands in never depends on how fast the test runs.

#include "engine/core/hash.h"
#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/platform_event.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

using namespace engine::core;

constexpr KeyScancode kKey = kKey_Space;
constexpr const char *kAction = "input_log_test_jump";
constexpr std::uint64_t kFirstStep = 1U;
constexpr std::uint64_t kLastStep = std::numeric_limits<std::uint64_t>::max();
constexpr const char *kLogPath = "input_log_test.bin";
constexpr const char *kOtherPath = "input_log_test_other.bin";
constexpr const char *kDamagedPath = "input_log_test_damaged.bin";

// The layout's fixed offsets: the version after the 8-byte magic, and a
// footer of magic, count and checksum.
constexpr std::size_t kVersionOffset = 8U;
constexpr std::size_t kFooterBytes = 24U;
constexpr std::size_t kEmptyLogBytes = 12U + kFooterBytes;
constexpr std::size_t kStepFlagsOffset = 12U + 8U;

std::uint64_t g_tick = 0U;

void pump(std::initializer_list<PlatformEvent> events) {
  begin_input_frame();
  for (const PlatformEvent &event : events) {
    input_process_event(event);
  }
  end_input_frame();
}

PlatformEvent key(bool down, std::uint64_t timestampNs) {
  PlatformEvent event{};
  event.kind = down ? PlatformEventKind::KeyDown : PlatformEventKind::KeyUp;
  event.scancode = kKey;
  event.timestampNs = timestampNs;
  return event;
}

PlatformEvent mouse_to(int x, std::uint64_t timestampNs) {
  PlatformEvent event{};
  event.kind = PlatformEventKind::MouseMove;
  event.x = static_cast<float>(x);
  event.deltaX = 1.0F;
  event.timestampNs = timestampNs;
  return event;
}

/// Everything a step answers that the cases below vary.
struct StepView final {
  bool down = false;
  bool pressed = false;
  bool released = false;
  bool action = false;
  int mouseX = 0;
  int mouseDeltaX = 0;

  bool operator==(const StepView &) const noexcept = default;
};

/// Runs one frame of `count` steps from the running tick and appends what
/// each read.
void run_steps(std::uint32_t count, std::vector<StepView> &out) {
  begin_input_steps(count, g_tick);
  while (advance_input_step()) {
    StepView view{};
    view.down = is_key_down(kKey);
    view.pressed = is_key_pressed(kKey);
    view.released = is_key_released(kKey);
    view.action = is_action_pressed(kAction);
    const MouseState mouse = mouse_state();
    view.mouseX = mouse.x;
    view.mouseDeltaX = mouse.deltaX;
    out.push_back(view);
    ++g_tick;
  }
  end_input_steps();
}

std::vector<std::uint8_t> read_bytes(const char *path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return {};
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  return in ? bytes : std::vector<std::uint8_t>{};
}

bool write_bytes(const char *path, const std::vector<std::uint8_t> &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

/// Re-seals a hand-edited log so only the edit, not the checksum, is what
/// the reader refuses.
void reseal(std::vector<std::uint8_t> &bytes) {
  const std::size_t checksumAt = bytes.size() - 8U;
  std::uint64_t hash = kFnv1a64Offset;
  for (std::size_t i = 0U; i < checksumAt; ++i) {
    hash = fnv1a_64_append(hash, bytes[i]);
  }
  for (std::size_t i = 0U; i < 8U; ++i) {
    bytes[checksumAt + i] = static_cast<std::uint8_t>(hash >> (8U * i));
  }
}

/// Records `steps` one-step frames, the mouse at x = tick in each, so every
/// step's snapshot differs from the one before.
bool record_distinct_steps(const char *path, std::size_t steps) {
  g_tick = 0U;
  reset_input_steps();
  if (!begin_input_recording(path)) {
    return false;
  }
  std::vector<StepView> ignored;
  ignored.reserve(1U);
  for (std::size_t i = 0U; i < steps; ++i) {
    pump({mouse_to(static_cast<int>(i), kFirstStep)});
    run_steps(1U, ignored);
    ignored.clear();
  }
  return end_input_recording();
}

/// Replays the log at `path` from tick 0 for `steps` steps with the live
/// mouse held elsewhere; returns how many steps read the log's mouse x.
std::size_t replay_distinct_steps(const char *path, std::size_t steps) {
  g_tick = 0U;
  pump({mouse_to(-1000, kFirstStep)});
  reset_input_steps();
  if (!begin_input_replay(path)) {
    return 0U;
  }
  std::vector<StepView> views;
  views.reserve(steps);
  for (std::size_t i = 0U; i < steps; ++i) {
    run_steps(1U, views);
  }
  std::size_t matched = 0U;
  for (std::size_t i = 0U; i < views.size(); ++i) {
    if (views[i].mouseX == static_cast<int>(i)) {
      ++matched;
    }
  }
  return matched;
}

/// The recorded session: taps split across a frame's steps, a press held
/// through a pause (a reset, so the next step's previous snapshot is the
/// live state and not the step before), and mouse motion.
std::vector<StepView> record_session(const char *path) {
  std::vector<StepView> views;
  g_tick = 0U;
  pump({});
  reset_input_steps();
  CHECK(begin_input_recording(path), "a recording begins");
  CHECK(input_recording_active(), "and is active");
  CHECK(!begin_input_recording(kOtherPath),
        "a second recording is refused while one is open");

  pump(
      {key(true, kFirstStep), mouse_to(10, kFirstStep), key(false, kLastStep)});
  run_steps(3U, views);
  pump({});
  run_steps(2U, views);
  // Paused: the press arrives while nothing steps.
  pump({key(true, kFirstStep)});
  reset_input_steps();
  pump({mouse_to(20, kLastStep)});
  run_steps(2U, views);
  pump({key(false, kFirstStep)});
  run_steps(1U, views);
  CHECK(end_input_recording(), "the recording commits");
  CHECK(!input_recording_active(), "and is no longer active");
  return views;
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(initialize_logging());
  static_cast<void>(initialize_input());
  CHECK(register_action(kAction, kKey), "register the test action");

  // A recorded session replays step for step, at another frame schedule,
  // whatever the live devices do meanwhile.
  {
    const std::vector<StepView> recorded = record_session(kLogPath);
    CHECK(recorded.size() == 8U, "the session ran eight steps");
    CHECK(recorded[0].pressed && recorded[0].action && recorded[0].down,
          "the tap's press lands in the first step");
    CHECK(recorded[2].released, "and its release in the frame's last");
    CHECK(recorded[5].down && !recorded[5].pressed && !recorded[5].action,
          "a press held through a pause is not pressed again after it");

    // Live input the replay must not see: the key held, the mouse elsewhere.
    pump({key(true, kFirstStep), mouse_to(-5, kFirstStep)});
    reset_input_steps();
    g_tick = 0U;
    CHECK(begin_input_replay(kLogPath), "the log loads");
    CHECK(input_replay_active(), "and replays");
    // Re-recording what the replay reads writes the same bytes.
    CHECK(begin_input_recording(kOtherPath), "record the replay");
    std::vector<StepView> replayed;
    for (int i = 0; i < 8; ++i) {
      pump({key(i % 2 == 0, kFirstStep)});
      run_steps(1U, replayed);
    }
    CHECK(end_input_recording(), "the re-recording commits");
    CHECK(!input_replay_active(), "the replay ends after the log's last step");
    CHECK(replayed == recorded,
          "every replayed step reads what the recorded step read");
    CHECK(read_bytes(kOtherPath) == read_bytes(kLogPath),
          "recording the replay writes a byte-identical log");

    std::vector<StepView> after;
    pump({});
    run_steps(1U, after);
    CHECK(after.size() == 1U && after[0].mouseX == -5,
          "the step after the log reads the live devices again");
    pump({key(false, kFirstStep)});
    reset_input_steps();
  }

  // An empty log: valid, and there is nothing to replay.
  {
    CHECK(record_distinct_steps(kLogPath, 0U), "an empty recording commits");
    CHECK(read_bytes(kLogPath).size() == kEmptyLogBytes,
          "an empty log is the header and footer alone");
    CHECK(begin_input_replay(kLogPath), "an empty log loads");
    CHECK(!input_replay_active(), "and leaves nothing to replay");
  }

  // One step.
  {
    CHECK(record_distinct_steps(kLogPath, 1U), "a one-step recording");
    CHECK(replay_distinct_steps(kLogPath, 2U) == 1U,
          "one step replays and the next reads live");
    CHECK(!input_replay_active(), "the replay ended after its one step");
  }

  // The drain ring: at its capacity, and one past it, which drains the ring
  // mid-recording.
  for (const std::size_t steps :
       {kInputLogRingCapacity, kInputLogRingCapacity + 1U}) {
    CHECK(record_distinct_steps(kLogPath, steps),
          "a recording around the ring's capacity");
    CHECK(replay_distinct_steps(kLogPath, steps) == steps,
          "every step around the ring's capacity replays");
    CHECK(!input_replay_active(), "and the replay ends at its last");
  }

  // Many rings' worth: the ring bounds only what is held between drains.
  {
    constexpr std::size_t kSteps = (4U * kInputLogRingCapacity) + 3U;
    CHECK(record_distinct_steps(kLogPath, kSteps),
          "a recording of several rings' worth");
    CHECK(replay_distinct_steps(kLogPath, kSteps) == kSteps,
          "every step of it replays");
  }

  // Refusals leave a running replay as it was.
  {
    CHECK(record_distinct_steps(kLogPath, 4U), "a valid log");
    const std::vector<std::uint8_t> valid = read_bytes(kLogPath);
    g_tick = 0U;
    CHECK(begin_input_replay(kLogPath), "start a replay to keep");
    std::vector<StepView> views;
    run_steps(1U, views);

    std::vector<std::uint8_t> version = valid;
    version[kVersionOffset] = 2U;
    reseal(version);
    CHECK(write_bytes(kDamagedPath, version) &&
              !begin_input_replay(kDamagedPath),
          "the next version is refused");
    version[kVersionOffset] = 0U;
    reseal(version);
    CHECK(write_bytes(kDamagedPath, version) &&
              !begin_input_replay(kDamagedPath),
          "an older version is refused");

    // Every cut through the header, the first step's start and the footer,
    // and a stride through the rest, which one file write per cut makes
    // slow to cover byte by byte.
    bool everyCutRefused = true;
    for (std::size_t size = 0U; size < valid.size();
         size += ((size < 48U) || (size + 48U >= valid.size())) ? 1U : 7U) {
      const std::vector<std::uint8_t> cut(
          valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(size));
      everyCutRefused = everyCutRefused && write_bytes(kDamagedPath, cut) &&
                        !begin_input_replay(kDamagedPath);
    }
    CHECK(everyCutRefused, "every truncation tried is refused");

    std::vector<std::uint8_t> flipped = valid;
    flipped[valid.size() / 2U] ^= 0x01U;
    CHECK(write_bytes(kDamagedPath, flipped) &&
              !begin_input_replay(kDamagedPath),
          "a damaged byte is refused by the checksum");

    std::vector<std::uint8_t> flagged = valid;
    flagged[kStepFlagsOffset] |= 0x80U;
    reseal(flagged);
    CHECK(write_bytes(kDamagedPath, flagged) &&
              !begin_input_replay(kDamagedPath),
          "an unknown step flag is refused");

    CHECK(!begin_input_replay("input_log_test_missing.bin"),
          "a missing log is refused");

    CHECK(input_replay_active(), "the replay survived every refusal");
    for (int i = 0; i < 3; ++i) {
      run_steps(1U, views);
    }
    CHECK(views.size() == 4U && views[3].mouseX == 3,
          "and still gives its own steps in order");
    CHECK(!input_replay_active(), "to its end");
  }

  // The steps after a replay read the live devices as if it had never run:
  // a key the log ends holding, and the live keyboard never held, is up
  // there with no release edge.
  {
    g_tick = 0U;
    reset_input_steps();
    CHECK(begin_input_recording(kLogPath), "record a held key");
    std::vector<StepView> views;
    pump({key(true, kFirstStep)});
    run_steps(1U, views);
    CHECK(end_input_recording(), "commit it");
    pump({key(false, kFirstStep)});
    reset_input_steps();
    g_tick = 0U;
    CHECK(begin_input_replay(kLogPath), "replay it");
    views.clear();
    run_steps(2U, views);
    CHECK(views.size() == 2U && views[0].down && views[0].pressed,
          "the replayed step holds the key");
    CHECK(views.size() == 2U && !views[1].down && !views[1].released,
          "the step after reads the live devices, with no release");
  }

  // A replay whose next step is not the one running stops, and that step
  // reads the live devices.
  {
    CHECK(record_distinct_steps(kLogPath, 2U), "a log from tick 0");
    pump({mouse_to(-7, kFirstStep)});
    reset_input_steps();
    CHECK(begin_input_replay(kLogPath), "load it");
    g_tick = 5U;
    std::vector<StepView> views;
    run_steps(1U, views);
    CHECK(!input_replay_active(), "a misaligned step ends the replay");
    CHECK(views.size() == 1U && views[0].mouseX == -7,
          "and reads the live devices");
  }

  // A recording that fails leaves the destination as it was.
  {
    const std::vector<std::uint8_t> previous = {'o', 'l', 'd'};
    CHECK(write_bytes(kLogPath, previous), "an earlier file at the path");
    g_tick = 0U;
    CHECK(begin_input_recording(kLogPath), "record over it");
    std::vector<StepView> views;
    run_steps(1U, views);
    g_tick = 9U; // the steps skip ticks 1 to 8
    run_steps(1U, views);
    CHECK(!input_recording_active(), "a skipped tick fails the recording");
    CHECK(!end_input_recording(), "a failed recording does not commit");
    CHECK(read_bytes(kLogPath) == previous,
          "and the earlier file is untouched");
    CHECK(!end_input_recording(), "ending with none open is refused");
  }

  end_input_replay();
  shutdown_input();
  shutdown_logging();
  std::remove(kLogPath);
  std::remove(kOtherPath);
  std::remove(kDamagedPath);
  if (g_failures != 0) {
    std::fprintf(stderr, "input_log_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("input_log_test passed");
  return 0;
}
