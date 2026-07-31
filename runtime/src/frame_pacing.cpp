// Implements frame pacing helpers: vsync interval normalization, the
// pure frame-cap wait computation, and the hybrid sleep-then-spin wait
// the pipeline runs at the end of each frame.

#include "frame_pacing.h"

#include <chrono>
#include <thread>

namespace engine::runtime {

int normalize_vsync_interval(int requested) noexcept {
  if (requested < 0) {
    return -1;
  }
  return (requested == 0) ? 0 : 1;
}

double frame_cap_wait_seconds(double elapsedSeconds, int maxFps) noexcept {
  if (maxFps <= 0) {
    return 0.0;
  }
  const double targetSeconds = 1.0 / static_cast<double>(maxFps);
  const double wait = targetSeconds - elapsedSeconds;
  return (wait > 0.0) ? wait : 0.0;
}

void wait_for_frame_cap(double waitSeconds) noexcept {
  if (waitSeconds <= 0.0) {
    return;
  }

  using Clock = std::chrono::steady_clock;
  const Clock::time_point deadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(waitSeconds));

  // OS sleep granularity is coarse (~1.5 ms on Windows), so sleep most of
  // the wait and spin the remainder.
  constexpr std::chrono::milliseconds kSpinReserve{2};
  const Clock::time_point sleepUntil = deadline - kSpinReserve;
  if (sleepUntil > Clock::now()) {
    std::this_thread::sleep_until(sleepUntil);
  }
  while (Clock::now() < deadline) {
    std::this_thread::yield();
  }
}

} // namespace engine::runtime
