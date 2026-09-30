// Verifies audio test behavior for the Engine test suite.

#include "engine/audio/audio.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "../test_harness.h"
#include "audio_diagnostics.h"
#include "engine/core/cvar.h"
#include "engine/core/vfs.h"
#include "sound_handle.h"

static engine::tests::TestContext g_tests;

/// The generation counter must visit every nonzero value of its field and
/// never mint zero, the invalid encoding: a counter that reset to zero
/// would hand out handles that decode as invalid, and one that overflowed
/// would corrupt the slot bits. Driven on the pure function because the
/// eight-million-load alternative is not a unit test.
static void test_sound_generation_wraps_skipping_zero() noexcept {
  using engine::audio::kSoundGenerationMask;
  using engine::audio::next_sound_generation;
  static_assert(next_sound_generation(0U) == 1U);
  static_assert(next_sound_generation(kSoundGenerationMask - 1U) ==
                kSoundGenerationMask);
  static_assert(next_sound_generation(kSoundGenerationMask) == 1U);

  std::uint32_t generation = 1U;
  std::uint32_t steps = 0U;
  bool sawInvalid = false;
  bool sawOverflow = false;
  do {
    generation = next_sound_generation(generation);
    ++steps;
    sawInvalid = sawInvalid || (generation == 0U);
    sawOverflow = sawOverflow || (generation > kSoundGenerationMask);
  } while ((generation != 1U) && (steps <= kSoundGenerationMask));
  g_tests.check(!sawInvalid, "generation counter never mints zero");
  g_tests.check(!sawOverflow, "generation counter stays inside its field");
  g_tests.check(steps == kSoundGenerationMask,
                "generation period visits every nonzero value exactly once");
}

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
    g_tests.skip("bus volume roundtrip (no audio device)");
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
    g_tests.skip("master volume stored (no audio device)");
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
    g_tests.skip("invalid inputs rejected (no audio device)");
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

/// Writes a 16-bit PCM WAV of `frames` frames, every sample `value`
/// (silent by default), mono at 22050 Hz unless told otherwise, whose data
/// chunk header claims `claimedDataBytes`; the claim is what a decoder
/// derives its frame count from, so it can exceed the bytes present.
static bool write_wav(const std::filesystem::path &path, std::uint32_t frames,
                      std::uint32_t claimedDataBytes, std::int16_t value = 0,
                      std::uint32_t channels = 1U,
                      std::uint32_t sampleRate = 22050U) {
  const std::uint32_t blockAlign = channels * 2U;
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
  put_le(bytes, 36U + claimedDataBytes, 4U);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put_le(bytes, 16U, 4U);
  put_le(bytes, 1U, 2U); // PCM
  put_le(bytes, channels, 2U);
  put_le(bytes, sampleRate, 4U);
  put_le(bytes, sampleRate * blockAlign, 4U);
  put_le(bytes, blockAlign, 2U);
  put_le(bytes, 16U, 2U);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put_le(bytes, claimedDataBytes, 4U);
  for (std::uint32_t i = 0U; i < (frames * channels); ++i) {
    put_le(bytes, static_cast<std::uint16_t>(value), 2U);
  }
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
    g_tests.skip("decode budgets (no audio device)");
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

/// Live-handle registry boundaries: an unloaded handle is stale (it no
/// longer plays, and the reloaded slot mints a different handle that does
/// not revive it), a loaded path is shared (300 loads take one slot and
/// decode once, and only the last unload frees it), the registry admits
/// exactly kMaxSounds distinct paths and refuses the next, and
/// unload_all_sounds invalidates every handle. Reported skipped, not
/// passed, when no audio device initializes.
static void test_registry_boundaries() {
  using namespace engine::audio;
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path scratch =
      fs::current_path(ec) / "engine_audio_registry_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  TEST_ASSERT(write_wav(scratch / "tone.wav", 220U, 440U));
  // One more distinct path than the registry holds.
  for (std::size_t i = 0U; i <= engine::audio::kMaxSounds; ++i) {
    char name[32] = {};
    std::snprintf(name, sizeof(name), "tone%03zu.wav", i);
    fs::copy_file(scratch / "tone.wav", scratch / name, ec);
    TEST_ASSERT(!ec);
  }
  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audioreg", scratch.string().c_str()));
  if (!initialize_audio()) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.skip("registry boundaries (no audio device)");
    return;
  }

  const SoundHandle first = load_sound("audioreg/tone.wav");
  g_tests.check(first != kInvalidSound, "fixture loads");
  unload_sound(first);
  g_tests.check(!play_sound(first, {}), "unloaded handle no longer plays");
  const SoundHandle reloaded = load_sound("audioreg/tone.wav");
  g_tests.check(reloaded != kInvalidSound, "slot reloads after unload");
  g_tests.check(reloaded.id != first.id, "reloaded slot mints a new handle");
  g_tests.check(!play_sound(first, {}),
                "stale handle stays rejected after its slot is reused");

  // One path loaded by 300 scripts is one sound (#893).
  const std::size_t decodesBefore = audio_decoder_opens();
  bool allShared = true;
  for (int i = 0; i < 299; ++i) {
    allShared = allShared && (load_sound("audioreg/tone.wav") == reloaded);
  }
  g_tests.check(allShared, "a loaded path returns its own handle again");
  g_tests.check(audio_decoder_opens() == decodesBefore,
                "a loaded path is not decoded again");
  for (int i = 0; i < 299; ++i) {
    unload_sound(reloaded);
  }
  g_tests.check(play_sound(reloaded, {}),
                "the sound lives until its last reference is returned");
  unload_sound(reloaded);
  g_tests.check(!play_sound(reloaded, {}),
                "the last unload frees the shared sound");

  std::size_t live = 0U;
  SoundHandle last = kInvalidSound;
  while (live < engine::audio::kMaxSounds) {
    char path[48] = {};
    std::snprintf(path, sizeof(path), "audioreg/tone%03zu.wav", live);
    last = load_sound(path);
    if (last == kInvalidSound) {
      break;
    }
    ++live;
  }
  g_tests.check(live == engine::audio::kMaxSounds,
                "registry holds exactly kMaxSounds distinct sounds");
  char pastPath[48] = {};
  std::snprintf(pastPath, sizeof(pastPath), "audioreg/tone%03zu.wav",
                engine::audio::kMaxSounds);
  g_tests.check(load_sound(pastPath) == kInvalidSound,
                "a distinct sound past the registry capacity is refused");
  g_tests.check(load_sound("audioreg/tone000.wav") != kInvalidSound,
                "a full registry still shares a loaded path");
  unload_all_sounds();
  g_tests.check(!play_sound(last, {}),
                "unload_all_sounds invalidates live handles");
  g_tests.check(load_sound("audioreg/tone.wav") != kInvalidSound,
                "registry accepts loads again after unload_all_sounds");

  shutdown_audio();
  engine::core::shutdown_vfs();
  fs::remove_all(scratch, ec);
}

/// EXPECTATION (#600): a positional sound keeps its full volume out to
/// the distance a third-person camera sits from the character it follows,
/// falls off by the inverse model past that, and a sound too far to be
/// heard is refused rather than taking one of the fixed instance slots.
void test_positional_attenuation() noexcept {
  using namespace engine::audio;
  PlayParams params{};
  g_tests.check(params.minDistance == kDefaultMinAudibleDistance,
                "the default minimum distance is the kit camera's distance");
  g_tests.check(params.rolloff == kDefaultRolloff,
                "the default rolloff is the inverse law");

  // A footstep at the followed character's feet, heard from the camera
  // 7.4 m away: full gain, where the mixer's own 1 m default would have
  // left it at about 0.135.
  g_tests.check(distance_gain(7.4F, params) == 1.0F,
                "a sound at the followed entity plays at full gain");
  g_tests.check(distance_gain(0.0F, params) == 1.0F,
                "a sound at the listener plays at full gain");
  // Past the minimum distance the inverse model applies exactly:
  // 8 / (8 + 1 * (24 - 8)) = 1/3.
  g_tests.check(distance_gain(24.0F, params) == (1.0F / 3.0F),
                "three times the minimum distance is a third of the gain");
  PlayParams tight{};
  tight.minDistance = 1.0F;
  g_tests.check(distance_gain(2.0F, tight) == 0.5F,
                "a sound authored to fall off from a metre does");
  PlayParams flat{};
  flat.rolloff = 0.0F;
  g_tests.check(distance_gain(1000.0F, flat) == 1.0F,
                "zero rolloff never attenuates");
  PlayParams broken{};
  broken.minDistance = 0.0F;
  g_tests.check(distance_gain(10.0F, broken) == 1.0F,
                "an invalid minimum distance reports no attenuation");

  // Out-of-range attenuation params are refused like the other play
  // params, before any instance exists.
  g_tests.check(!play_sound_at(kInvalidSound, engine::math::Vec3(0, 0, 0),
                               broken),
                "an invalid minimum distance is refused");

  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path scratch = fs::current_path(ec) / "engine_audio_gain_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  TEST_ASSERT(write_wav(scratch / "tone.wav", 220U, 440U));
  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audiogain", scratch.string().c_str()));
  const auto finish = [&scratch]() noexcept {
    unload_all_sounds();
    shutdown_audio();
    engine::core::shutdown_vfs();
    std::error_code removeError{};
    fs::remove_all(scratch, removeError);
  };
  if (!initialize_audio()) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.skip("inaudible one-shots are culled (no audio device)");
    return;
  }
  set_listener(engine::math::Vec3(0.0F, 0.0F, 0.0F),
               engine::math::Vec3(0.0F, 0.0F, -1.0F),
               engine::math::Vec3(0.0F, 1.0F, 0.0F));
  const SoundHandle handle = load_sound("audiogain/tone.wav");
  if (handle == kInvalidSound) {
    finish();
    g_tests.check(false, "the gain fixture loads");
    return;
  }
  PlayParams audible{};
  g_tests.check(play_sound_at(handle, engine::math::Vec3(0.0F, 0.0F, -5.0F),
                              audible),
                "a sound within earshot starts");
  // 8 / (8 + (100000 - 8)) is far below the audible floor.
  g_tests.check(!play_sound_at(handle,
                               engine::math::Vec3(0.0F, 0.0F, -100000.0F),
                               audible),
                "a sound too far to hear never takes an instance slot");
  PlayParams silent{};
  silent.volume = 0.0F;
  g_tests.check(!play_sound_at(handle, engine::math::Vec3(0.0F, 0.0F, -1.0F),
                               silent),
                "a silent sound never takes an instance slot");
  finish();
}

/// EXPECTATION (#573): a sound is decoded once, at load; every playback
/// after that, positional or not, reads the decoded PCM and opens no
/// decoder. Before, each one-shot opened its own decoder over the file,
/// a header parse and an allocation per gameplay sound event.
static void test_playback_opens_no_decoder() {
  using namespace engine::audio;
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path scratch = fs::current_path(ec) / "engine_audio_decode_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  TEST_ASSERT(write_wav(scratch / "tone.wav", 220U, 440U));
  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audiodecode", scratch.string().c_str()));
  AudioConfig config{};
  config.nullDevice = true;
  if (!initialize_audio(config)) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.check(false, "audio initializes on the null device");
    return;
  }
  set_listener(engine::math::Vec3(0.0F, 0.0F, 0.0F),
               engine::math::Vec3(0.0F, 0.0F, -1.0F),
               engine::math::Vec3(0.0F, 1.0F, 0.0F));

  const std::size_t beforeLoad = engine::audio::audio_decoder_opens();
  const SoundHandle handle = load_sound("audiodecode/tone.wav");
  g_tests.check(handle != kInvalidSound, "the fixture loads");
  g_tests.check(engine::audio::audio_decoder_opens() == beforeLoad + 1U,
                "loading a sound opens one decoder");

  const std::size_t afterLoad = engine::audio::audio_decoder_opens();
  PlayParams params{};
  int started = 0;
  for (int i = 0; i < 8; ++i) {
    started +=
        play_sound_at(handle, engine::math::Vec3(0.0F, 0.0F, -2.0F), params)
            ? 1
            : 0;
    started += play_sound_oneshot(handle, params) ? 1 : 0;
  }
  started += play_sound(handle, params) ? 1 : 0;
  g_tests.check(started == 17, "every playback starts");
  g_tests.check(engine::audio::audio_decoder_opens() == afterLoad,
                "no playback opens a decoder");

  unload_all_sounds();
  shutdown_audio();
  engine::core::shutdown_vfs();
  fs::remove_all(scratch, ec);
}

/// EXPECTATION (#573 row 3): playing a sound allocates nothing. The
/// one-shot pool's voices are made once, when audio starts; a playback
/// points a free voice at the sound's decoded PCM, whatever its sample
/// rate and channel count, and a finished one frees its voice for the
/// next. Before, every one-shot built its own mixer node, a heap
/// allocation per gameplay sound event. Hundreds of plays, more than the
/// pool holds at once, each heard in the mix, allocate nothing.
static void test_playback_allocates_nothing() {
  using namespace engine::audio;
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path scratch = fs::current_path(ec) / "engine_audio_pool_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  // Short, loud and constant, so a playback is heard in the mix and ends
  // within one mixed block: mono at 22050 Hz, stereo at 44100 Hz, and six
  // channels, which loading brings down to stereo.
  TEST_ASSERT(write_wav(scratch / "mono.wav", 220U, 440U, 12000));
  TEST_ASSERT(
      write_wav(scratch / "stereo.wav", 441U, 441U * 4U, 12000, 2U, 44100U));
  TEST_ASSERT(
      write_wav(scratch / "surround.wav", 441U, 441U * 12U, 12000, 6U, 44100U));
  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audiopool", scratch.string().c_str()));
  AudioConfig config{};
  config.nullDevice = true;
  if (!initialize_audio(config)) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.check(false, "audio initializes on the null device");
    return;
  }
  set_listener(engine::math::Vec3(0.0F, 0.0F, 0.0F),
               engine::math::Vec3(0.0F, 0.0F, -1.0F),
               engine::math::Vec3(0.0F, 1.0F, 0.0F));
  const SoundHandle sounds[] = {load_sound("audiopool/mono.wav"),
                                load_sound("audiopool/stereo.wav"),
                                load_sound("audiopool/surround.wav")};
  g_tests.check((sounds[0] != kInvalidSound) && (sounds[1] != kInvalidSound) &&
                    (sounds[2] != kInvalidSound),
                "mono, stereo and six-channel sounds load");

  float peak = 0.0F;
  g_tests.check(audio_mix_null_device(4800U, &peak) && (peak == 0.0F),
                "the mix is silent before anything plays");

  const std::size_t before = audio_mixer_allocations();
  const PlayParams params{};
  int started = 0;
  int heard = 0;
  constexpr int kRounds = 100;
  for (int round = 0; round < kRounds; ++round) {
    const SoundHandle sound = sounds[round % 3];
    // Two at once, positional and not, so voices of both kinds are in
    // use and recycled.
    started +=
        play_sound_at(sound, engine::math::Vec3(0.0F, 0.0F, -2.0F), params) ? 1
                                                                            : 0;
    started += play_sound_oneshot(sound, params, AudioBus::Sfx) ? 1 : 0;
    // 100 ms at 48 kHz outlasts every fixture, so both voices finish.
    peak = 0.0F;
    heard += (audio_mix_null_device(4800U, &peak) && (peak > 0.05F)) ? 1 : 0;
    update_audio();
  }
  g_tests.check(started == (2 * kRounds),
                "every playback starts: finished voices return to the pool");
  g_tests.check(heard == kRounds, "every round is heard in the mix");
  g_tests.check(audio_mixer_allocations() == before,
                "no playback allocates in the mixer");
  g_tests.check(audio_mix_null_device(4800U, &peak) && (peak == 0.0F),
                "the mix is silent once every playback has finished");

  unload_all_sounds();
  shutdown_audio();
  engine::core::shutdown_vfs();
  fs::remove_all(scratch, ec);
}

/// Plays `sound` as a one-shot and returns how many frames of the 48 kHz
/// mix carry it, mixed in 16-frame blocks.
static int audible_frames(engine::audio::SoundHandle sound) noexcept {
  using namespace engine::audio;
  if (!play_sound_oneshot(sound, PlayParams{}, AudioBus::Sfx)) {
    return -1;
  }
  int frames = 0;
  for (int block = 0; block < 400; ++block) {
    float peak = 0.0F;
    if (!audio_mix_null_device(16U, &peak)) {
      return -1;
    }
    frames += (peak > 0.05F) ? 16 : 0;
  }
  update_audio();
  return frames;
}

/// EXPECTATION (#573 row 3): a pooled voice plays each sound at that
/// sound's own sample rate, even straight after one of another rate on
/// the same voice. 882 frames at 44.1 kHz last 960 frames of the 48 kHz
/// mix; 220 frames at 22.05 kHz last 479. Measured in 16-frame blocks, so
/// a count is within one block plus the resampler's few frames of
/// latency of the ideal: 32 frames either way. Playing the second at the
/// first's rate would last 239 frames; at the mix's own rate, 220. The
/// voice also follows its bus's volume, and one played without a
/// position after a positional playback is not left where that one was.
static void test_voice_takes_each_sounds_rate() {
  using namespace engine::audio;
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path scratch = fs::current_path(ec) / "engine_audio_rate_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  TEST_ASSERT(
      write_wav(scratch / "fast.wav", 882U, 882U * 2U, 12000, 1U, 44100U));
  TEST_ASSERT(write_wav(scratch / "slow.wav", 220U, 440U, 12000));
  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audiorate", scratch.string().c_str()));
  AudioConfig config{};
  config.nullDevice = true;
  if (!initialize_audio(config)) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.check(false, "audio initializes on the null device");
    return;
  }
  const SoundHandle fast = load_sound("audiorate/fast.wav");
  const SoundHandle slow = load_sound("audiorate/slow.wav");
  const auto near = [](int measured, int ideal) noexcept {
    return (measured >= (ideal - 32)) && (measured <= (ideal + 32));
  };
  const int fastFrames = audible_frames(fast);
  const int slowFrames = audible_frames(slow);
  char what[128] = {};
  std::snprintf(what, sizeof(what),
                "a 44.1 kHz sound lasts its length in the mix (%d frames)",
                fastFrames);
  g_tests.check(near(fastFrames, 960), what);
  std::snprintf(what, sizeof(what),
                "then a 22.05 kHz sound on the same voice lasts its own "
                "(%d frames)",
                slowFrames);
  g_tests.check(near(slowFrames, 479), what);

  // A voice is routed to its bus on each playback: muting the bus
  // silences it, and restoring it brings the sound back.
  set_bus_volume(AudioBus::Sfx, 0.0F);
  g_tests.check(audible_frames(slow) == 0,
                "a one-shot on the muted Sfx bus is silent");
  set_bus_volume(AudioBus::Sfx, 1.0F);
  // A voice last played in the world, off to the listener's left, pans
  // hard left; played next without a position, the same voice is not
  // placed at all, and a mono sound plays the same in both channels.
  float left = 0.0F;
  float right = 0.0F;
  g_tests.check(play_sound_at(slow, engine::math::Vec3(-5.0F, 0.0F, 0.0F),
                              PlayParams{}) &&
                    audio_mix_null_device(4800U, nullptr, &left, &right) &&
                    (left > (2.0F * right)),
                "a positional one-shot to the left pans left");
  update_audio();
  g_tests.check(play_sound_oneshot(slow, PlayParams{}, AudioBus::Sfx) &&
                    audio_mix_null_device(4800U, nullptr, &left, &right) &&
                    (left > 0.05F) && (left == right),
                "the same voice played without a position is not panned");
  update_audio();

  unload_all_sounds();
  shutdown_audio();
  engine::core::shutdown_vfs();
  fs::remove_all(scratch, ec);
}

/// play_sound plays a pooled voice on the SFX bus (#805): the bus's
/// volume applies to it, a second play layers a second voice instead of
/// restarting the first, a looping play sounds past the sound's end until
/// stop_sound, and stop_sound silences every voice of the sound.
static void test_play_sound_uses_sfx_voices() {
  using namespace engine::audio;
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path scratch = fs::current_path(ec) / "engine_audio_sfx_test";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  TEST_ASSERT(!ec);
  // A constant level, so two voices started together sum exactly.
  TEST_ASSERT(write_wav(scratch / "level.wav", 4410U, 4410U * 2U, 8000));
  TEST_ASSERT(engine::core::initialize_vfs());
  TEST_ASSERT(engine::core::mount("audiosfx", scratch.string().c_str()));
  AudioConfig config{};
  config.nullDevice = true;
  if (!initialize_audio(config)) {
    engine::core::shutdown_vfs();
    fs::remove_all(scratch, ec);
    g_tests.check(false, "audio initializes on the null device");
    return;
  }
  const SoundHandle level = load_sound("audiosfx/level.wav");
  g_tests.check(level != kInvalidSound, "the fixture loads");
  const auto peak_of_next_mix = []() noexcept {
    float peak = -1.0F;
    return audio_mix_null_device(480U, &peak) ? peak : -1.0F;
  };

  set_bus_volume(AudioBus::Sfx, 0.0F);
  g_tests.check(play_sound(level, PlayParams{}) && (peak_of_next_mix() == 0.0F),
                "play_sound on the muted SFX bus is silent");
  stop_sound(level);
  set_bus_volume(AudioBus::Sfx, 1.0F);

  g_tests.check(play_sound(level, PlayParams{}), "a first play starts");
  const float one = peak_of_next_mix();
  stop_sound(level);
  update_audio();
  g_tests.check(play_sound(level, PlayParams{}) &&
                    play_sound(level, PlayParams{}),
                "a second play of the same sound starts");
  const float two = peak_of_next_mix();
  char what[128] = {};
  std::snprintf(what, sizeof(what),
                "two plays layer two voices (peak %.3f against %.3f)",
                static_cast<double>(two), static_cast<double>(one));
  // The same frames on two voices started in the same block sum; float
  // mixing leaves at most a rounding step between 2 x one and two.
  g_tests.check((one > 0.1F) && (std::fabs(two - (2.0F * one)) < 1.0e-4F),
                what);
  stop_sound(level);
  update_audio();
  g_tests.check(peak_of_next_mix() == 0.0F,
                "stop_sound silences every voice of the sound");

  PlayParams looping{};
  looping.loop = true;
  g_tests.check(play_sound(level, looping), "a looping play starts");
  // 0.2 s of sound, mixed for half a second: still sounding.
  for (int block = 0; block < 50; ++block) {
    static_cast<void>(peak_of_next_mix());
    update_audio();
  }
  g_tests.check(peak_of_next_mix() > 0.1F,
                "a looping play sounds past the sound's end");
  stop_sound(level);
  update_audio();
  g_tests.check(peak_of_next_mix() == 0.0F, "stop_sound ends a loop");

  unload_all_sounds();
  shutdown_audio();
  engine::core::shutdown_vfs();
  fs::remove_all(scratch, ec);
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
  RUN_TEST(test_positional_attenuation);
  RUN_TEST(test_decode_budgets);
  RUN_TEST(test_registry_boundaries);
  RUN_TEST(test_playback_opens_no_decoder);
  RUN_TEST(test_playback_allocates_nothing);
  RUN_TEST(test_voice_takes_each_sounds_rate);
  RUN_TEST(test_play_sound_uses_sfx_voices);

  test_sound_generation_wraps_skipping_zero();

  return g_tests.finish("Audio tests");
}
