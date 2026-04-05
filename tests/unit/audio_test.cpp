#include "engine/audio/audio.h"

#include <cstdio>

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                  \
      ++g_failed;                                                              \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    std::printf("[ RUN  ] %s\n", #fn);                                         \
    fn();                                                                      \
    if (g_failed == before_##fn) {                                             \
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
  ++g_passed;
}

static void test_load_without_init() {
  using namespace engine::audio;
  // System not initialized — should return invalid.
  const SoundHandle h = load_sound("nonexistent.wav");
  TEST_ASSERT(h == kInvalidSound);
  ++g_passed;
}

static void test_unload_invalid() {
  using namespace engine::audio;
  // Should not crash.
  unload_sound(kInvalidSound);
  unload_sound(SoundHandle{999U});
  ++g_passed;
}

static void test_play_invalid() {
  using namespace engine::audio;
  PlayParams params{};
  TEST_ASSERT(!play_sound(kInvalidSound, params));
  TEST_ASSERT(!play_sound(SoundHandle{999U}, params));
  ++g_passed;
}

static void test_stop_without_init() {
  using namespace engine::audio;
  // Should not crash.
  stop_sound(kInvalidSound);
  stop_all();
  ++g_passed;
}

static void test_set_master_volume_without_init() {
  using namespace engine::audio;
  // Should not crash.
  set_master_volume(0.5F);
  ++g_passed;
}

static void test_update_without_init() {
  using namespace engine::audio;
  // Should not crash.
  update_audio();
  ++g_passed;
}

int main() {
  int before_test_double_init_and_shutdown = g_failed;
  RUN_TEST(test_double_init_and_shutdown);
  int before_test_load_without_init = g_failed;
  RUN_TEST(test_load_without_init);
  int before_test_unload_invalid = g_failed;
  RUN_TEST(test_unload_invalid);
  int before_test_play_invalid = g_failed;
  RUN_TEST(test_play_invalid);
  int before_test_stop_without_init = g_failed;
  RUN_TEST(test_stop_without_init);
  int before_test_set_master_volume_without_init = g_failed;
  RUN_TEST(test_set_master_volume_without_init);
  int before_test_update_without_init = g_failed;
  RUN_TEST(test_update_without_init);

  std::printf("\nAudio tests: %d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
