// Verifies audio test behavior for the Engine test suite.

#include "engine/audio/audio.h"

#include <cstdio>

#include "../test_harness.h"

static engine::tests::TestContext g_tests;

#define TEST_ASSERT(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_tests.check(false, #cond);                                             \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    std::printf("[ RUN  ] %s\n", #fn);                                         \
    const int failuresBefore = g_tests.failed();                               \
    fn();                                                                      \
    if (g_tests.failed() == failuresBefore) {                                  \
      std::printf("[  OK  ] %s\n", #fn);                                       \
    }                                                                          \
  } while (false)

// --------------------------------------------------------------------------
// Tests — audio device may or may not be available in CI.  These test the
// registry bookkeeping and graceful failure paths only.
// --------------------------------------------------------------------------

static void test_double_init_and_shutdown() {
  using namespace engine::audio;
  // init may fail in CI (no audio device) — either result is acceptable.
  const bool first = initialize_audio();
  if (first) {
    // Double init should succeed.
    TEST_ASSERT(initialize_audio());
    shutdown_audio();
  }
  // Double shutdown is safe.
  shutdown_audio();
  g_tests.check(true, "double init and shutdown");
}

/// Handles test load without init.
static void test_load_without_init() {
  using namespace engine::audio;
  // System not initialized — should return invalid.
  const SoundHandle h = load_sound("nonexistent.wav");
  TEST_ASSERT(h == kInvalidSound);
  g_tests.check(true, "load without init");
}

/// Handles test unload invalid.
static void test_unload_invalid() {
  using namespace engine::audio;
  // Should not crash.
  unload_sound(kInvalidSound);
  unload_sound(SoundHandle{999U});
  g_tests.check(true, "unload invalid");
}

/// Handles test play invalid.
static void test_play_invalid() {
  using namespace engine::audio;
  PlayParams params{};
  TEST_ASSERT(!play_sound(kInvalidSound, params));
  TEST_ASSERT(!play_sound(SoundHandle{999U}, params));
  g_tests.check(true, "play invalid");
}

/// Handles test stop without init.
static void test_stop_without_init() {
  using namespace engine::audio;
  // Should not crash.
  stop_sound(kInvalidSound);
  stop_all();
  g_tests.check(true, "stop without init");
}

/// Handles test set master volume without init.
static void test_set_master_volume_without_init() {
  using namespace engine::audio;
  // Should not crash.
  set_master_volume(0.5F);
  g_tests.check(true, "set master volume without init");
}

/// Handles test update without init.
static void test_update_without_init() {
  using namespace engine::audio;
  // Should not crash.
  update_audio();
  g_tests.check(true, "update without init");
}

/// Runs this executable or test program.
int main() {
  RUN_TEST(test_double_init_and_shutdown);
  RUN_TEST(test_load_without_init);
  RUN_TEST(test_unload_invalid);
  RUN_TEST(test_play_invalid);
  RUN_TEST(test_stop_without_init);
  RUN_TEST(test_set_master_volume_without_init);
  RUN_TEST(test_update_without_init);

  return g_tests.finish("Audio tests");
}
