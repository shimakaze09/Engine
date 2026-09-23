// The fixed-step input snapshots (#653) at their boundaries: events that
// fall in no step yet, a reset, a key held across steps, a release the
// pump saw without an event, more events than the record holds, and the
// return to live state. Timestamps are chosen at the window's ends (1 is
// before any window starts, the maximum after any ends), so which step an
// event lands in never depends on how fast the test runs.

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/platform_event.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

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
constexpr std::uint64_t kFirstStep = 1U;
constexpr std::uint64_t kLastStep = std::numeric_limits<std::uint64_t>::max();

/// One pump with the given key events, the way the pipeline runs it.
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

struct StepView final {
  bool down = false;
  bool pressed = false;
  bool released = false;
};

/// Runs `stepCount` steps and records what each one read for the key.
template <std::size_t N> void run_steps(StepView (&views)[N]) {
  begin_input_steps(static_cast<std::uint32_t>(N), 0U);
  for (StepView &view : views) {
    CHECK(advance_input_step(), "a readied step advances");
    view.down = is_key_down(kKey);
    view.pressed = is_key_pressed(kKey);
    view.released = is_key_released(kKey);
  }
  CHECK(!advance_input_step(), "no step past the readied ones");
  end_input_steps();
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(initialize_logging());
  static_cast<void>(initialize_input());
  pump({});
  reset_input_steps();

  // A tap split across steps: down in the first step, up in the last.
  {
    pump({key(true, kFirstStep), key(false, kLastStep)});
    StepView steps[3];
    run_steps(steps);
    CHECK(steps[0].pressed && steps[0].down, "the press lands in step 0");
    CHECK(!steps[1].pressed && steps[1].down,
          "a key held across steps is pressed once and down after");
    CHECK(steps[2].released && !steps[2].down, "the release lands in step 2");
    CHECK(!steps[0].released && !steps[1].released,
          "no release before the key came up");
  }

  // A tap inside one step: pressed and released there, never down.
  {
    pump({key(true, kLastStep), key(false, kLastStep)});
    StepView steps[2];
    run_steps(steps);
    CHECK(!steps[0].pressed, "nothing in the first step");
    CHECK(steps[1].pressed && steps[1].released && !steps[1].down,
          "a tap inside one step is pressed and released in it alone");
  }

  // A frame with no steps: its events wait for the next steps.
  {
    pump({key(true, kFirstStep)});
    begin_input_steps(0U, 0U);
    CHECK(!advance_input_step(), "zero steps ready nothing");
    end_input_steps();
    pump({key(false, kLastStep)});
    StepView steps[2];
    run_steps(steps);
    CHECK(steps[0].pressed && steps[0].down,
          "a press from a frame that ran no step reaches the next steps");
    CHECK(steps[1].released && !steps[1].down,
          "and the later release after it");
  }

  // A reset drops what was recorded: stopped-play input is not replayed.
  {
    pump({key(true, kFirstStep)});
    reset_input_steps();
    StepView steps[1];
    pump({});
    run_steps(steps);
    CHECK(!steps[0].pressed && steps[0].down,
          "after a reset the held key is down but not pressed again");
    pump({key(false, kFirstStep)});
    run_steps(steps);
    CHECK(steps[0].released, "its release still arrives");
  }

  // A release the pump saw without a key event -- focus loss -- still
  // ends the last step, with its edge.
  {
    pump({key(true, kFirstStep)});
    StepView steps[1];
    run_steps(steps);
    PlatformEvent focus{};
    focus.kind = PlatformEventKind::WindowFocusLost;
    pump({focus});
    StepView after[2];
    run_steps(after);
    CHECK(after[0].down, "no key event: the first step still reads held");
    CHECK(after[1].released && !after[1].down,
          "the last step ends in the live state, released");
  }

  // More events than the record holds: the last step still ends right.
  {
    begin_input_frame();
    for (int i = 0; i < 600; ++i) {
      input_process_event(key((i % 2) == 0, kFirstStep));
    }
    input_process_event(key(true, kLastStep)); // past the record
    end_input_frame();
    StepView steps[2];
    run_steps(steps);
    CHECK(steps[1].down,
          "a press past the record capacity still ends the last step down");
    pump({key(false, kFirstStep)});
    StepView after[1];
    run_steps(after);
    CHECK(after[0].released && !after[0].down, "and releases after");
  }

  // Outside a step, queries answer from live state again.
  {
    pump({key(true, kFirstStep), key(false, kFirstStep)});
    CHECK(is_key_pressed(kKey), "live state keeps the frame's own edge");
    StepView steps[1];
    run_steps(steps);
    CHECK(is_key_pressed(kKey) && !is_key_down(kKey),
          "after the steps, live state answers again");
  }

  shutdown_input();
  shutdown_logging();
  if (g_failures != 0) {
    std::fprintf(stderr, "input_steps_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("input_steps_test passed");
  return 0;
}
