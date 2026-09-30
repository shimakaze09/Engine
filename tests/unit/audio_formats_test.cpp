// Every extension the asset catalog files as a Sound is one the audio
// module decodes, and each one plays: a 0.2 s 440 Hz tone in each format
// (tests/data/audio) loads through load_sound and streams through
// play_music, and both are heard in the null-device mix. The catalog's
// Sound row and the audio module's kLoadableSoundExtensions are the same
// set, so a format cannot be catalogued without a decoder.

#include "engine/audio/audio.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "../test_harness.h"
#include "audio_diagnostics.h"
#include "engine/content/asset_type_table.h"
#include "engine/core/vfs.h"

namespace {

engine::tests::TestContext g_tests;

bool contains(const char *const *list, std::size_t count,
              const char *suffix) noexcept {
  for (std::size_t i = 0U; i < count; ++i) {
    if (std::strcmp(list[i], suffix) == 0) {
      return true;
    }
  }
  return false;
}

void check_catalog_matches_decoders() noexcept {
  using engine::audio::kLoadableSoundExtensions;
  const engine::content::AssetTypeDescriptor &sound =
      engine::content::asset_type_descriptor(
          engine::content::AssetTypeTag::Audio);
  const std::size_t loadable = std::size(kLoadableSoundExtensions);
  for (std::size_t i = 0U; i < sound.sourceSuffixCount; ++i) {
    char message[96] = {};
    std::snprintf(message, sizeof(message),
                  "the catalogued Sound suffix %s is decodable",
                  sound.sourceSuffixes[i]);
    g_tests.check(
        contains(kLoadableSoundExtensions, loadable, sound.sourceSuffixes[i]),
        message);
  }
  for (const char *suffix : kLoadableSoundExtensions) {
    char message[96] = {};
    std::snprintf(message, sizeof(message),
                  "the decodable suffix %s is catalogued as a Sound", suffix);
    g_tests.check(
        contains(sound.sourceSuffixes, sound.sourceSuffixCount, suffix),
        message);
  }
}

/// The loudest sample of the next 100 ms of the 48 kHz mix, or -1 when the
/// null device could not mix.
float mixed_peak() noexcept {
  float peak = 0.0F;
  return engine::audio::audio_mix_null_device(4800U, &peak) ? peak : -1.0F;
}

/// The fixture is a sine of amplitude 0.5. A decode that fails or yields
/// silence mixes to 0. Measured on Linux, the four formats peak within
/// 0.007 of 0.5 (the lossy codecs' coding error and the resampler's
/// ripple); every platform runs the same decoders, so a tenth of the
/// amplitude either way is the tone at its authored level and nothing else.
bool heard_at_level(float peak) noexcept {
  return (peak > 0.45F) && (peak < 0.55F);
}

void check_each_format_plays() noexcept {
  using namespace engine::audio;
  for (const char *suffix : kLoadableSoundExtensions) {
    char path[64] = {};
    std::snprintf(path, sizeof(path), "audiofmt/tone%s", suffix);
    char message[128] = {};

    const SoundHandle sound = load_sound(path);
    std::snprintf(message, sizeof(message), "%s loads", path);
    g_tests.check(sound != kInvalidSound, message);
    float peak = -1.0F;
    if ((sound != kInvalidSound) &&
        play_sound_oneshot(sound, PlayParams{}, AudioBus::Sfx)) {
      peak = mixed_peak();
    }
    std::snprintf(message, sizeof(message),
                  "%s plays as a sound at its level (peak %.3f)", path,
                  static_cast<double>(peak));
    g_tests.check(heard_at_level(peak), message);
    while (mixed_peak() > 0.0F) {
    }
    unload_sound(sound);

    peak = -1.0F;
    if (play_music(path, 1.0F, false)) {
      peak = mixed_peak();
    }
    std::snprintf(message, sizeof(message),
                  "%s streams as music at its level (peak %.3f)", path,
                  static_cast<double>(peak));
    g_tests.check(heard_at_level(peak), message);
    stop_music();
    update_audio();
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  check_catalog_matches_decoders();

  if (!engine::core::initialize_vfs() ||
      !engine::core::mount("audiofmt", ENGINE_AUDIO_FIXTURE_DIR)) {
    g_tests.fail("mount the audio fixtures");
    return g_tests.finish("audio formats");
  }
  engine::audio::AudioConfig config{};
  config.nullDevice = true;
  if (!engine::audio::initialize_audio(config)) {
    g_tests.fail("initialize audio on the null device");
  } else {
    check_each_format_plays();
    engine::audio::shutdown_audio();
  }
  engine::core::shutdown_vfs();
  return g_tests.finish("audio formats");
}
