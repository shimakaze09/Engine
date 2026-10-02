// Pins every platform's capability row (#312 item 7): which platforms own
// the main loop, have worker threads, need an audio unlock, may block the
// main thread and take touch as the primary pointer. Also that the running
// platform reports its own row, and that with no window the native window
// is kind None and the caps say so.

#include "engine/core/platform.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

using engine::core::NativeWindowKind;
using engine::core::PlatformCaps;
using engine::core::PlatformId;

struct Row final {
  PlatformId id;
  bool ownsMainLoop;
  bool hasThreads;
  bool needsAudioUnlock;
  bool mainThreadMayBlock;
  bool touchPrimary;
};

constexpr Row kRows[] = {
    {PlatformId::Windows, false, true, false, true, false},
    {PlatformId::Linux, false, true, false, true, false},
    {PlatformId::MacOS, false, true, false, true, false},
    {PlatformId::Web, true, true, true, false, false},
    {PlatformId::IOS, true, true, false, false, true},
    {PlatformId::Android, false, true, false, true, true},
};

bool matches(const PlatformCaps &caps, const Row &row) noexcept {
  return (caps.id == row.id) && !caps.hasWindow &&
         (caps.ownsMainLoop == row.ownsMainLoop) &&
         (caps.hasThreads == row.hasThreads) &&
         (caps.needsAudioUnlock == row.needsAudioUnlock) &&
         (caps.mainThreadMayBlock == row.mainThreadMayBlock) &&
         (caps.touchPrimary == row.touchPrimary);
}

} // namespace

/// Runs this executable or test program.
int main() {
  for (const Row &row : kRows) {
    CHECK(matches(engine::core::platform_caps_for(row.id), row),
          "each platform's caps row is the pinned one");
  }

  // The running platform reports its own row; before initialization there
  // is no window, so the native window is None and hasWindow is false.
  const PlatformCaps running = engine::core::platform_caps();
  bool knownRow = false;
  for (const Row &row : kRows) {
    knownRow = knownRow || matches(running, row);
  }
  CHECK(knownRow, "the running platform reports its pinned row");
  CHECK(engine::core::platform_native_window().kind == NativeWindowKind::None,
        "no native window before the platform initializes");

  if (g_failures != 0) {
    std::fprintf(stderr, "platform caps tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("platform caps tests passed\n");
  return 0;
}
