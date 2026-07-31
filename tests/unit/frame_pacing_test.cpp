// Verifies the pure frame pacing helpers: vsync interval normalization
// to the supported set and exact frame-cap wait computation (uncapped,
// under budget, exactly on budget, and over budget).

#include "frame_pacing.h"

#include <cstdio>

namespace {

using engine::runtime::frame_cap_wait_seconds;
using engine::runtime::normalize_vsync_interval;

/// EXPECTATION: any negative request maps to adaptive (-1), zero stays
/// off, and every positive value clamps to plain vsync (1).
int check_vsync_normalization() {
  if (normalize_vsync_interval(-1) != -1) {
    return 1;
  }
  if (normalize_vsync_interval(-7) != -1) {
    return 2;
  }
  if (normalize_vsync_interval(0) != 0) {
    return 3;
  }
  if (normalize_vsync_interval(1) != 1) {
    return 4;
  }
  if (normalize_vsync_interval(5) != 1) {
    return 5;
  }
  return 0;
}

/// EXPECTATION: maxFps <= 0 waits nothing; a frame under budget waits
/// exactly the remainder (target 1/maxFps); a frame at or past its budget
/// waits nothing.
int check_frame_cap_wait() {
  if (frame_cap_wait_seconds(0.005, 0) != 0.0) {
    return 10;
  }
  if (frame_cap_wait_seconds(0.005, -30) != 0.0) {
    return 11;
  }

  const double target = 1.0 / 100.0;
  if (frame_cap_wait_seconds(0.0, 100) != target) {
    return 12;
  }
  if (frame_cap_wait_seconds(0.004, 100) != (target - 0.004)) {
    return 13;
  }
  if (frame_cap_wait_seconds(target, 100) != 0.0) {
    return 14;
  }
  if (frame_cap_wait_seconds(0.5, 100) != 0.0) {
    return 15;
  }

  if (frame_cap_wait_seconds(1.0 / 60.0, 60) != 0.0) {
    return 16;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_vsync_normalization();
  if (result != 0) {
    std::printf("vsync normalization failed: %d\n", result);
    return result;
  }
  result = check_frame_cap_wait();
  if (result != 0) {
    std::printf("frame cap wait failed: %d\n", result);
    return result;
  }
  return 0;
}
