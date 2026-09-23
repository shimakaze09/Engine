// Pins the timer cadence contract after the move from per-frame to
// per-fixed-step (docs/decisions/0019).
//
// The old cadence advanced timers once per rendered frame with that
// frame's whole delta, so when a timer came due depended on how long the
// frame took: the same script fired callbacks at different simulation
// times on a 144 Hz and a 30 Hz machine, and a frame that caught up
// several steps collapsed them into one advance. The new cadence advances
// once per fixed step and dispatches once per frame.
//
// Both halves are asserted here because only the pair is the contract:
// coming due is simulation time (so a frame rate cannot move it), and
// dispatch is once per frame (so a catch-up frame cannot multiply a
// callback). A test of either alone would pass on an implementation that
// got the other wrong.

#include "engine/runtime/timer_manager.h"

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

using engine::runtime::TimerId;
using engine::runtime::TimerManager;

int g_fireCount;
TimerId g_lastFired;

void count_fire(TimerId id, void *) noexcept {
  ++g_fireCount;
  g_lastFired = id;
}

/// The fixed step the engine simulates at, as a float, matching what the
/// pipeline hands `advance`.
constexpr float kStep = 1.0F / 60.0F;

/// Simulates one frame: `steps` fixed advances, then one dispatch.
std::size_t run_frame(TimerManager &timers, int steps) noexcept {
  for (int i = 0; i < steps; ++i) {
    static_cast<void>(timers.advance(kStep));
  }
  return timers.dispatch();
}

/// EXPECTATION: a timer comes due after its delay in simulated time, and
/// the number of frames that time was spread over does not matter. This
/// is the half the old cadence got wrong.
void check_due_time_is_simulation_time() noexcept {
  // Ten steps of simulated time, delivered as ten one-step frames.
  {
    g_fireCount = 0;
    TimerManager timers{};
    static_cast<void>(timers.set_timeout(kStep * 10.0F, &count_fire, nullptr));
    int firedOnFrame = 0;
    for (int frame = 1; frame <= 12; ++frame) {
      if ((run_frame(timers, 1) > 0U) && (firedOnFrame == 0)) {
        firedOnFrame = frame;
      }
    }
    check(g_fireCount == 1, "a one-shot fires once at one step per frame");
    check(firedOnFrame == 10,
          "a ten-step timer fires on the tenth step, one step per frame");
  }

  // The same ten steps of simulated time, delivered as two five-step
  // frames — a machine running at a third of the rate, or one recovering
  // from a hitch. Under the old cadence the whole frame's delta arrived
  // in one advance, so the timer came due on the frame boundary; the
  // count is what must not change.
  {
    g_fireCount = 0;
    TimerManager timers{};
    static_cast<void>(timers.set_timeout(kStep * 10.0F, &count_fire, nullptr));
    const std::size_t firstFrame = run_frame(timers, 5);
    check(firstFrame == 0U,
          "five steps do not reach a ten-step timer");
    const std::size_t secondFrame = run_frame(timers, 5);
    check(secondFrame == 1U,
          "the tenth step reaches it, whichever frame carries that step");
    check(g_fireCount == 1, "and it still fires exactly once");
  }

  // Elapsed time is the sum of the steps, not of the frames: the two
  // schedules above must leave the same clock behind.
  {
    TimerManager oneAtATime{};
    for (int i = 0; i < 10; ++i) {
      static_cast<void>(run_frame(oneAtATime, 1));
    }
    TimerManager fiveAtATime{};
    static_cast<void>(run_frame(fiveAtATime, 5));
    static_cast<void>(run_frame(fiveAtATime, 5));
    check(oneAtATime.elapsed_seconds() == fiveAtATime.elapsed_seconds(),
          "ten steps are ten steps however many frames carried them");
  }
}

/// EXPECTATION: a frame dispatches a timer at most once, however many
/// steps it caught up. This is the half the old cadence got right and the
/// new one must keep: a re-entrant or multiplied gameplay callback is
/// worse than a late one.
void check_dispatch_is_once_per_frame() noexcept {
  // A repeating timer whose interval is shorter than one step, in a frame
  // that catches up eight steps. Coming due eight times must still be
  // one callback.
  g_fireCount = 0;
  TimerManager timers{};
  static_cast<void>(timers.set_interval(kStep * 0.25F, &count_fire, nullptr));
  const std::size_t fired = run_frame(timers, 8);
  check(fired == 1U, "eight catch-up steps dispatch one callback");
  check(g_fireCount == 1, "and the callback ran once");

  // It is still armed, and the next frame dispatches it again: deduping
  // within a frame must not swallow the timer.
  const std::size_t next = run_frame(timers, 1);
  check(next == 1U, "the repeating timer dispatches again next frame");
  check(g_fireCount == 2, "so the callback has now run twice");
}

/// EXPECTATION: a timer cancelled after coming due and before dispatch
/// does not fire. Cancelling has to mean it will not run, which only
/// holds if dispatch re-checks the slot rather than trusting the mark.
void check_cancel_between_due_and_dispatch() noexcept {
  g_fireCount = 0;
  TimerManager timers{};
  const TimerId id = timers.set_timeout(kStep, &count_fire, nullptr);
  check(id != engine::runtime::kInvalidTimerId, "the timer was created");
  const std::size_t marked = timers.advance(kStep);
  check(marked == 1U, "one step brings the timer due");
  timers.cancel(id);
  const std::size_t fired = timers.dispatch();
  check(fired == 0U, "a timer cancelled while due does not dispatch");
  check(g_fireCount == 0, "and its callback never ran");
  check(timers.active_count() == 0U, "the cancelled slot is free");
}

/// EXPECTATION: the combined `tick` still behaves as one advance plus one
/// dispatch, because callers outside the fixed step use it and their
/// meaning must not have changed under them.
void check_tick_still_advances_and_dispatches() noexcept {
  g_fireCount = 0;
  TimerManager timers{};
  static_cast<void>(timers.set_timeout(kStep * 2.0F, &count_fire, nullptr));
  check(timers.tick(kStep) == 0U, "one step is not yet due");
  check(timers.tick(kStep) == 1U, "the second step fires it");
  check(g_fireCount == 1, "the callback ran once through tick");
}

/// EXPECTATION: a callback that cancels its own repeating timer ends it,
/// and one that re-arms a one-shot keeps it. Both run during dispatch
/// now, so the slot bookkeeping after the callback has to still hold.
TimerManager *g_selfCancelTarget;
TimerId g_selfCancelId;

void cancel_self(TimerId id, void *) noexcept {
  ++g_fireCount;
  if (g_selfCancelTarget != nullptr) {
    g_selfCancelTarget->cancel(id);
  }
}

void rearm_self(TimerId, void *) noexcept {
  ++g_fireCount;
  if (g_selfCancelTarget != nullptr) {
    g_selfCancelId = g_selfCancelTarget->set_timeout(kStep, &rearm_self,
                                                     nullptr);
  }
}

void check_callback_can_change_its_own_timer() noexcept {
  {
    g_fireCount = 0;
    TimerManager timers{};
    g_selfCancelTarget = &timers;
    static_cast<void>(timers.set_interval(kStep, &cancel_self, nullptr));
    static_cast<void>(run_frame(timers, 1));
    check(g_fireCount == 1, "the repeating callback ran once");
    check(timers.active_count() == 0U,
          "a callback cancelling its own repeat ends it");
    static_cast<void>(run_frame(timers, 4));
    check(g_fireCount == 1, "and it does not come back");
    g_selfCancelTarget = nullptr;
  }
  {
    g_fireCount = 0;
    TimerManager timers{};
    g_selfCancelTarget = &timers;
    g_selfCancelId = timers.set_timeout(kStep, &rearm_self, nullptr);
    static_cast<void>(run_frame(timers, 1));
    check(g_fireCount == 1, "the one-shot ran");
    check(timers.active_count() == 1U,
          "a callback re-arming from inside dispatch leaves one armed");
    // The re-armed timer must not fire again in the same frame: it was
    // set during dispatch, so nothing has marked it.
    check(timers.dispatch() == 0U,
          "a timer armed during dispatch waits for the next frame");
    static_cast<void>(run_frame(timers, 1));
    check(g_fireCount == 2, "and fires on that next frame");
    g_selfCancelTarget = nullptr;
  }
}

/// One frame schedule: how many fixed steps each frame carried, all
/// summing to 60 — one second of simulation delivered at four different
/// frame rates.
struct Schedule final {
  const char *name;
  int stepsPerFrame;
  int frames;
};

constexpr Schedule kSchedules[4] = {{"60 frames x 1 step", 1, 60},
                                    {"30 frames x 2 steps", 2, 30},
                                    {"15 frames x 4 steps", 4, 15},
                                    {"12 frames x 5 steps", 5, 12}};

/// Runs a one-second timer through a schedule and returns how many fixed
/// steps of simulation had elapsed when it came *due* — which is the
/// thing a frame rate must not move. Dispatch is deliberately not what
/// is measured: it happens once per frame by design, so a schedule with
/// four steps per frame will always run the callback on a multiple of
/// four whatever the cadence, and measuring that would compare the frame
/// rate against itself.
///
/// `frameSum` selects the old cadence, whose only way to observe
/// due-ness was one advance per frame carrying the frame's whole delta.
/// Its answer is therefore reported at the frame boundary, because that
/// is genuinely all the old cadence knew.
int due_step_under(const Schedule &schedule, bool frameSum) noexcept {
  TimerManager timers{};
  static_cast<void>(timers.set_timeout(1.0F, &count_fire, nullptr));
  int stepsRun = 0;
  for (int frame = 0; frame < schedule.frames + 2; ++frame) {
    if (frameSum) {
      const std::size_t marked =
          timers.advance(kStep * static_cast<float>(schedule.stepsPerFrame));
      stepsRun += schedule.stepsPerFrame;
      if (marked > 0U) {
        return stepsRun;
      }
      continue;
    }
    for (int step = 0; step < schedule.stepsPerFrame; ++step) {
      const std::size_t marked = timers.advance(kStep);
      ++stepsRun;
      if (marked > 0U) {
        return stepsRun;
      }
    }
  }
  return 0;
}

/// EXPECTATION, and the reason the cadence moved: under the old
/// per-frame advance a timer's firing step depended on the frame
/// pattern, because `elapsed` accumulated one frame-sized float per
/// frame and those sums do not agree across schedules. Under the
/// per-step advance every schedule accumulates the same 60 additions of
/// the fixed delta and agrees exactly.
///
/// The numbers are exact, not approximate: the project compiles with
/// -ffp-contract=off, so these are the same additions in the same order
/// on every platform.
void check_frame_rate_cannot_move_the_firing_step() noexcept {
  int perStep[4] = {};
  int frameSum[4] = {};
  for (std::size_t i = 0U; i < 4U; ++i) {
    perStep[i] = due_step_under(kSchedules[i], false);
    frameSum[i] = due_step_under(kSchedules[i], true);
    std::printf("timer_cadence_test: %-20s per-step step %d, frame-sum "
                "step %d\n",
                kSchedules[i].name, perStep[i], frameSum[i]);
  }

  bool perStepAgrees = true;
  for (std::size_t i = 1U; i < 4U; ++i) {
    perStepAgrees = perStepAgrees && (perStep[i] == perStep[0]);
  }
  check(perStepAgrees,
        "per-step advance fires a one-second timer on the same step at "
        "every frame rate");
  check(perStep[0] > 0, "the one-second timer fired at all");

  // The old cadence, named so the regression is legible: at least two of
  // these schedules disagreed, which is the frame-rate dependence this
  // change removes. If this ever stops holding, the float accumulation
  // changed and the claim above needs re-deriving rather than trusting.
  bool frameSumDisagrees = false;
  for (std::size_t i = 1U; i < 4U; ++i) {
    frameSumDisagrees = frameSumDisagrees || (frameSum[i] != frameSum[0]);
  }
  check(frameSumDisagrees,
        "the old per-frame cadence did depend on the frame rate, which is "
        "what this test exists to have moved away from");
}

} // namespace

/// Runs this executable or test program.
int main() {
  check_frame_rate_cannot_move_the_firing_step();
  check_due_time_is_simulation_time();
  check_dispatch_is_once_per_frame();
  check_cancel_between_due_and_dispatch();
  check_tick_still_advances_and_dispatches();
  check_callback_can_change_its_own_timer();

  if (g_failures != 0) {
    std::fprintf(stderr, "timer_cadence_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("timer_cadence_test: timers come due on simulation time and "
              "dispatch once per frame\n");
  return 0;
}
