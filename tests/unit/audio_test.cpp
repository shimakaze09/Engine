// Verifies audio test behavior for the Engine test suite.

#include "engine/audio/audio.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "../test_harness.h"
#include "engine/core/cvar.h"
#include "engine/core/vfs.h"

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

/// EXPECTATION (audit M-29): set_master_volume behaves exactly like
/// set_bus_volume(Master) — the stored value bus_volume returns reflects
/// it, negatives clamp to 0, and non-finite input is ignored. Init may
/// fail in CI (no audio device) — checks run only when it succeeds.
static void test_master_volume_stored() {
  using namespace engine::audio;
  if (!initialize_audio()) {
    g_tests.check(true, "master volume stored (skipped, no device)");
    return;
  }

  set_master_volume(0.4F);
  TEST_ASSERT(bus_volume(AudioBus::Master) == 0.4F);

  set_master_volume(-2.0F);
  TEST_ASSERT(bus_volume(AudioBus::Master) == 0.0F);

  set_master_volume(0.6F);
  set_master_volume(std::numeric_limits<float>::quiet_NaN());
  TEST_ASSERT(bus_volume(AudioBus::Master) == 0.6F);

  set_bus_volume(AudioBus::Sfx, 0.5F);
  set_bus_volume(AudioBus::Sfx, std::numeric_limits<float>::infinity());
  TEST_ASSERT(bus_volume(AudioBus::Sfx) == 0.5F);

  shutdown_audio();
  g_tests.check(true, "master volume stored");
}

/// EXPECTATION (audit M-29): non-finite listener transforms are rejected
/// without touching miniaudio, invalid play params fail fast, and a
/// literal stop_all with active pool/music state is safe. Device-gated
/// like the other live-engine tests.
static void test_invalid_inputs_rejected() {
  using namespace engine::audio;
  if (!initialize_audio()) {
    g_tests.check(true, "invalid inputs rejected (skipped, no device)");
    return;
  }

  const float nan = std::numeric_limits<float>::quiet_NaN();
  set_listener(engine::math::Vec3(nan, 0.0F, 0.0F),
               engine::math::Vec3(0.0F, 0.0F, -1.0F),
               engine::math::Vec3(0.0F, 1.0F, 0.0F));

  PlayParams badPitch{};
  badPitch.pitch = 0.0F;
  TEST_ASSERT(!play_sound(SoundHandle{12345U}, badPitch));

  PlayParams badVolume{};
  badVolume.volume = nan;
  TEST_ASSERT(!play_sound_oneshot(SoundHandle{12345U}, badVolume));

  TEST_ASSERT(!play_music("assets/sounds/ambient.wav", nan, false));

  stop_all();
  shutdown_audio();
  g_tests.check(true, "invalid inputs rejected");
}

/// Appends a little-endian integer of `bytes` width to a byte vector.
static void put_le(std::vector<std::uint8_t> &out, std::uint32_t value,
                   std::size_t bytes) {
  for (std::size_t i = 0U; i < bytes; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
  }
}

/// Writes a 16-bit PCM mono WAV of `frames` silent frames whose data chunk
/// header claims `claimedDataBytes`; the claim is what a decoder derives
/// its frame count from, so it can exceed the bytes actually present.
static bool write_wav(const std::filesystem::path &path, std::uint32_t frames,
                      std::uint32_t claimedDataBytes) {
  constexpr std::uint32_t kSampleRate = 22050U;
  constexpr std::uint32_t kChannels = 1U;
  constexpr std::uint32_t kBlockAlign = kChannels * 2U;
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
  put_le(bytes, 36U + claimedDataBytes, 4U);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put_le(bytes, 16U, 4U);
  put_le(bytes, 1U, 2U); // PCM
  put_le(bytes, kChannels, 2U);
  put_le(bytes, kSampleRate, 4U);
  put_le(bytes, kSampleRate * kBlockAlign, 4U);
  put_le(bytes, kBlockAlign, 2U);
  put_le(bytes, 16U, 2U);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put_le(bytes, claimedDataBytes, 4U);
  bytes.resize(bytes.size() + static_cast<std::size_t>(frames) * kBlockAlign,
               0U);
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool ok =
      std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
  return (std::fclose(file) == 0) && ok;
}

/// EXPECTATION (regression test for #425): load_sound and play_music
/// bound their inputs before the bytes they bound exist in memory. A file
/// over `audio.max_sound_file_bytes` is refused before it is read, a
/// header claiming more decoded PCM than `audio.max_decoded_pcm_bytes` is
/// refused before decoding (exact at the boundary), a file over
/// `audio.max_music_file_bytes` never opens a stream, and the defaults
/// still admit an ordinary fixture. Device-gated like the other
/// live-engine checks; the fixtures live in a scratch VFS mount.
static void test_decode_budgets() {
  using namespace engine::audio;
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path scratch = fs::current_path(ec) / "engine_audio_budget_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  // 2205 frames of s16 mono: 4410 decoded bytes, a 4454-byte file.
  constexpr std::uint32_t kFrames = 2205U;
  constexpr std::uint64_t kDecodedBytes = kFrames * 2U;
  constexpr std::uint64_t kFileBytes = 44U + kDecodedBytes;
  TEST_ASSERT(write_wav(scratch / "normal.wav", kFrames,
                        static_cast<std::uint32_t>(kDecodedBytes)));
  // 100 real frames under a header claiming ~2 GiB of samples.
  TEST_ASSERT(write_wav(scratch / "huge_header.wav", 100U, 0x7FFFFFF0U));

  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audiotest", scratch.string().c_str()));
  std::uint64_t measured = 0U;
  TEST_ASSERT(engine::core::vfs_file_size("audiotest/normal.wav", &measured));
  TEST_ASSERT(measured == kFileBytes);

  if (!initialize_audio()) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.check(true, "decode budgets (skipped, no device)");
    return;
  }

  // The budgets exist once audio is initialized; their defaults admit the
  // ordinary fixture.
  const int soundCap = engine::core::cvar_get_int("audio.max_sound_file_bytes");
  const int pcmCap = engine::core::cvar_get_int("audio.max_decoded_pcm_bytes");
  const int musicCap = engine::core::cvar_get_int("audio.max_music_file_bytes");
  g_tests.check(soundCap > 0 && pcmCap > 0 && musicCap > 0,
                "budget cvars registered with positive defaults");
  SoundHandle handle = load_sound("audiotest/normal.wav");
  g_tests.check(handle != kInvalidSound, "default budgets admit the fixture");
  unload_sound(handle);

  // File cap: one byte under the file's size refuses it before any read.
  g_tests.check(engine::core::cvar_set_int("audio.max_sound_file_bytes",
                                           static_cast<int>(kFileBytes) - 1),
                "sound file cap is settable");
  handle = load_sound("audiotest/normal.wav");
  g_tests.check(handle == kInvalidSound, "file over the sound cap refused");
  g_tests.check(engine::core::cvar_set_int("audio.max_sound_file_bytes",
                                           static_cast<int>(kFileBytes)),
                "sound file cap restored to the exact size");
  handle = load_sound("audiotest/normal.wav");
  g_tests.check(handle != kInvalidSound, "file exactly at the sound cap loads");
  unload_sound(handle);
  engine::core::cvar_set_int("audio.max_sound_file_bytes", soundCap);

  // PCM cap: exact at the boundary of the header's claimed decoded bytes.
  g_tests.check(engine::core::cvar_set_int("audio.max_decoded_pcm_bytes",
                                           static_cast<int>(kDecodedBytes) - 1),
                "decoded PCM cap is settable");
  handle = load_sound("audiotest/normal.wav");
  g_tests.check(handle == kInvalidSound, "header over the PCM cap refused");
  engine::core::cvar_set_int("audio.max_decoded_pcm_bytes",
                             static_cast<int>(kDecodedBytes));
  handle = load_sound("audiotest/normal.wav");
  g_tests.check(handle != kInvalidSound, "header exactly at the PCM cap loads");
  unload_sound(handle);
  engine::core::cvar_set_int("audio.max_decoded_pcm_bytes", pcmCap);

  // A small file whose header claims gigabytes of samples is refused by
  // the default PCM budget, before decoding.
  handle = load_sound("audiotest/huge_header.wav");
  g_tests.check(handle == kInvalidSound,
                "small file with a gigabyte-claiming header refused");

  // Music: the file cap applies before the stream opens.
  g_tests.check(engine::core::cvar_set_int("audio.max_music_file_bytes",
                                           static_cast<int>(kFileBytes) - 1),
                "music file cap is settable");
  g_tests.check(!play_music("audiotest/normal.wav", 1.0F, false),
                "music over the file cap refused");
  engine::core::cvar_set_int("audio.max_music_file_bytes", musicCap);
  g_tests.check(play_music("audiotest/normal.wav", 1.0F, false),
                "music within the default cap streams");
  stop_music();

  shutdown_audio();
  engine::core::shutdown_vfs();
  fs::remove_all(scratch, ec);
  g_tests.check(true, "decode budgets");
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
  RUN_TEST(test_master_volume_stored);
  RUN_TEST(test_invalid_inputs_rejected);
  RUN_TEST(test_decode_budgets);

  return g_tests.finish("Audio tests");
}
