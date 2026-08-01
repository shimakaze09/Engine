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

static void test_load_without_init() {
  using namespace engine::audio;
  // System not initialized — should return invalid.
  const SoundHandle h = load_sound("nonexistent.wav");
  TEST_ASSERT(h == kInvalidSound);
  g_tests.check(true, "load without init");
}

static void test_unload_invalid() {
  using namespace engine::audio;
  // Should not crash.
  unload_sound(kInvalidSound);
  unload_sound(SoundHandle{999U});
  g_tests.check(true, "unload invalid");
}

static void test_play_invalid() {
  using namespace engine::audio;
  PlayParams params{};
  TEST_ASSERT(!play_sound(kInvalidSound, params));
  TEST_ASSERT(!play_sound(SoundHandle{999U}, params));
  g_tests.check(true, "play invalid");
}

static void test_stop_without_init() {
  using namespace engine::audio;
  // Should not crash.
  stop_sound(kInvalidSound);
  stop_all();
  g_tests.check(true, "stop without init");
}

static void test_set_master_volume_without_init() {
  using namespace engine::audio;
  // Should not crash.
  set_master_volume(0.5F);
  g_tests.check(true, "set master volume without init");
}

static void test_update_without_init() {
  using namespace engine::audio;
  // Should not crash.
  update_audio();
  g_tests.check(true, "update without init");
}

/// EXPECTATION: every new bus/3D/music API is a safe no-op before init —
/// bus_volume falls back to 1, one-shots and music report false.
static void test_extended_api_without_init() {
  using namespace engine::audio;
  set_bus_volume(AudioBus::Music, 0.5F);
  TEST_ASSERT(bus_volume(AudioBus::Music) == 1.0F);
  set_listener(engine::math::Vec3(0.0F, 0.0F, 0.0F),
               engine::math::Vec3(0.0F, 0.0F, -1.0F),
               engine::math::Vec3(0.0F, 1.0F, 0.0F));
  PlayParams params{};
  TEST_ASSERT(!play_sound_at(kInvalidSound,
                             engine::math::Vec3(0.0F, 0.0F, 0.0F), params,
                             AudioBus::Sfx));
  TEST_ASSERT(!play_sound_oneshot(kInvalidSound, params, AudioBus::Sfx));
  TEST_ASSERT(!play_music("assets/sounds/ambient.wav", 1.0F, true));
  stop_music();
  g_tests.check(true, "extended API without init");
}

/// EXPECTATION: with a live engine, bus volumes round-trip exactly
/// (negative input clamps to 0) and stale-handle one-shots still fail.
/// Init may fail in CI (no audio device) — the checks run only when it
/// succeeds.
static void test_bus_volume_roundtrip() {
  using namespace engine::audio;
  if (!initialize_audio()) {
    g_tests.check(true, "bus volume roundtrip (skipped, no device)");
    return;
  }

  set_bus_volume(AudioBus::Music, 0.25F);
  TEST_ASSERT(bus_volume(AudioBus::Music) == 0.25F);
  set_bus_volume(AudioBus::Sfx, 2.0F);
  TEST_ASSERT(bus_volume(AudioBus::Sfx) == 2.0F);
  set_bus_volume(AudioBus::Sfx, -1.0F);
  TEST_ASSERT(bus_volume(AudioBus::Sfx) == 0.0F);
  set_bus_volume(AudioBus::Master, 0.75F);
  TEST_ASSERT(bus_volume(AudioBus::Master) == 0.75F);

  PlayParams params{};
  TEST_ASSERT(!play_sound_at(SoundHandle{12345U},
                             engine::math::Vec3(1.0F, 2.0F, 3.0F), params,
                             AudioBus::Sfx));
  TEST_ASSERT(!play_music("assets/does_not_exist.wav", 1.0F, false));

  set_listener(engine::math::Vec3(1.0F, 2.0F, 3.0F),
               engine::math::Vec3(0.0F, 0.0F, -1.0F),
               engine::math::Vec3(0.0F, 1.0F, 0.0F));
  update_audio();

  shutdown_audio();
  g_tests.check(true, "bus volume roundtrip");
}

/// EXPECTATION (audit H-22): a forged out-of-range AudioBus value is
/// rejected at the public API boundary — set_bus_volume must not write
/// past the three-bus volume array and bus_volume must return the 1.0
/// fallback — both before and after initialization.
static void test_out_of_range_bus_rejected() {
  using namespace engine::audio;
  const auto forgedBus = static_cast<AudioBus>(7);

  set_bus_volume(forgedBus, 123.0F);
  TEST_ASSERT(bus_volume(forgedBus) == 1.0F);

  if (initialize_audio()) {
    set_bus_volume(AudioBus::Sfx, 0.5F);
    set_bus_volume(forgedBus, 123.0F);
    TEST_ASSERT(bus_volume(forgedBus) == 1.0F);
    TEST_ASSERT(bus_volume(AudioBus::Sfx) == 0.5F);
    shutdown_audio();
  }
  g_tests.check(true, "out-of-range bus rejected");
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
  RUN_TEST(test_extended_api_without_init);
  RUN_TEST(test_bus_volume_roundtrip);
  RUN_TEST(test_out_of_range_bus_rejected);

  return g_tests.finish("Audio tests");
}
