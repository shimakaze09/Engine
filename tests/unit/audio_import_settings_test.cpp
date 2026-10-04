// Verifies a sound's import settings reach its decode: a stereo 44.1 kHz
// tone written to a scratch directory loads through load_sound
// - as authored with no settings: two channels at its own rate;
// - folded to one channel under Force To Mono, heard equally in both ears;
// - resampled to the sample rate its ".meta" names, with the length the
//   new rate gives (rates the tone's 0.1 s divides into whole frames, so
//   the count is exact);
// - with both at once;
// and a sidecar whose settings do not read leaves the sound at its own
// format. A sound with no block of its own decodes with its folder's.

#include "engine/audio/audio.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "audio_diagnostics.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/vfs.h"

namespace {

engine::tests::TestContext g_tests;

constexpr const char *kDirectory = "audio_import_settings_test_files";
constexpr const char *kSound = "importfx/tone.wav";
constexpr std::uint32_t kRate = 44100U;
constexpr std::uint32_t kFrames = 4410U;

void put_u16(std::ofstream &file, std::uint32_t value) {
  const char bytes[2] = {static_cast<char>(value & 0xFFU),
                         static_cast<char>((value >> 8U) & 0xFFU)};
  file.write(bytes, 2);
}

void put_u32(std::ofstream &file, std::uint32_t value) {
  put_u16(file, value & 0xFFFFU);
  put_u16(file, value >> 16U);
}

/// Writes 0.1 s of 16-bit stereo at 44.1 kHz: a 440 Hz sine of amplitude
/// 0.5 on the left and silence on the right, so a fold to mono is told
/// apart from the source by its right channel.
bool write_tone(const std::string &path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  const std::uint32_t dataBytes = kFrames * 4U;
  file.write("RIFF", 4);
  put_u32(file, 36U + dataBytes);
  file.write("WAVEfmt ", 8);
  put_u32(file, 16U);
  put_u16(file, 1U);
  put_u16(file, 2U);
  put_u32(file, kRate);
  put_u32(file, kRate * 4U);
  put_u16(file, 4U);
  put_u16(file, 16U);
  file.write("data", 4);
  put_u32(file, dataBytes);
  for (std::uint32_t i = 0U; i < kFrames; ++i) {
    const double phase = 2.0 * 3.14159265358979323846 * 440.0 *
                         static_cast<double>(i) / static_cast<double>(kRate);
    const auto left =
        static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(phase)));
    put_u16(file, static_cast<std::uint16_t>(left));
    put_u16(file, 0U);
  }
  file.close();
  return !file.fail();
}

/// Gives the tone a sidecar holding `settings`; none at all when null.
bool author(const std::string &osPath,
            const engine::content::AudioImportSettings *settings) noexcept {
  std::error_code ec{};
  std::filesystem::remove(osPath + ".meta", ec);
  if (settings == nullptr) {
    return true;
  }
  engine::content::AssetSidecar sidecar{};
  if (!engine::content::parse_asset_guid(
          "44444444-5555-4666-8777-888888888888", &sidecar.guid)) {
    return false;
  }
  sidecar.hasAudioImport = true;
  sidecar.audioImport = *settings;
  return engine::content::write_asset_sidecar(osPath.c_str(), sidecar);
}

struct Decoded final {
  bool loaded = false;
  std::uint32_t channels = 0U;
  std::uint32_t sampleRate = 0U;
  std::uint64_t frames = 0U;
  float leftPeak = -1.0F;
  float rightPeak = -1.0F;
};

/// Loads the tone, reads the format it decoded to and plays it once into
/// the null-device mix, then unloads it so the next load decodes afresh.
Decoded load_and_play() noexcept {
  using namespace engine::audio;
  Decoded out{};
  const SoundHandle sound = load_sound(kSound);
  out.loaded = (sound != kInvalidSound) &&
               audio_sound_format(sound, &out.channels, &out.sampleRate,
                                  &out.frames);
  if (out.loaded && play_sound_oneshot(sound, PlayParams{}, AudioBus::Sfx)) {
    float peak = 0.0F;
    static_cast<void>(audio_mix_null_device(4800U, &peak, &out.leftPeak,
                                            &out.rightPeak));
    float rest = 0.0F;
    while (audio_mix_null_device(4800U, &rest) && (rest > 0.0F)) {
    }
  }
  unload_sound(sound);
  update_audio();
  return out;
}

void check_settings_reach_the_decode(const std::string &osPath) noexcept {
  using engine::content::AudioImportSettings;

  g_tests.check(author(osPath, nullptr), "the tone has no sidecar");
  const Decoded source = load_and_play();
  g_tests.check(source.loaded && (source.channels == 2U) &&
                    (source.sampleRate == kRate) && (source.frames == kFrames),
                "with no settings the tone decodes as written");
  // The mix is at 48 kHz, so a peak is the sine's amplitude to within the
  // resampler's ripple; the silent channel stays silent.
  g_tests.check((source.leftPeak > 0.45F) && (source.rightPeak < 0.001F),
                "the stereo source is heard on the left only");

  AudioImportSettings mono{};
  mono.forceMono = true;
  g_tests.check(author(osPath, &mono), "the tone is set to Force To Mono");
  const Decoded folded = load_and_play();
  g_tests.check(folded.loaded && (folded.channels == 1U) &&
                    (folded.sampleRate == kRate) && (folded.frames == kFrames),
                "Force To Mono decodes one channel at the file's rate");
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "the mono decode is heard equally in both ears (left %.4f, "
                "right %.4f)",
                static_cast<double>(folded.leftPeak),
                static_cast<double>(folded.rightPeak));
  g_tests.check((folded.leftPeak > 0.1F) &&
                    (std::fabs(folded.leftPeak - folded.rightPeak) < 1.0e-6F),
                message);

  AudioImportSettings half{};
  half.sampleRate = 22050U;
  g_tests.check(author(osPath, &half), "the tone is set to 22050 Hz");
  const Decoded resampled = load_and_play();
  g_tests.check(resampled.loaded && (resampled.channels == 2U) &&
                    (resampled.sampleRate == 22050U) &&
                    (resampled.frames == kFrames / 2U),
                "a sample rate setting resamples the decode, halving its "
                "frames");

  AudioImportSettings both{};
  both.sampleRate = 16000U;
  both.forceMono = true;
  g_tests.check(author(osPath, &both), "the tone is set to both");
  const Decoded smallest = load_and_play();
  g_tests.check(smallest.loaded && (smallest.channels == 1U) &&
                    (smallest.sampleRate == 16000U) &&
                    (smallest.frames == (kFrames * 16000U) / kRate),
                "both settings apply together");

  // A rate outside 8000..192000 Hz is not a sidecar this build reads.
  std::ofstream meta(osPath + ".meta", std::ios::binary | std::ios::trunc);
  meta << "{\n  \"schemaVersion\": 1,\n  \"guid\": "
          "\"44444444-5555-4666-8777-888888888888\",\n  "
          "\"importSettings\": {\"sampleRate\": 100}\n}\n";
  meta.close();
  g_tests.check(!meta.fail(), "the tone's sidecar asks for 100 Hz");
  const Decoded refused = load_and_play();
  g_tests.check(refused.loaded && (refused.channels == 2U) &&
                    (refused.sampleRate == kRate) &&
                    (refused.frames == kFrames),
                "a sidecar whose settings do not read leaves the format as "
                "written");
}

/// A sound with no block of its own decodes with its folder's.
void check_folder_settings_reach_the_decode() noexcept {
  using namespace engine::audio;
  std::error_code ec{};
  const std::string folder = std::string(kDirectory) + "/sfx";
  std::filesystem::create_directories(folder, ec);
  engine::content::AssetSidecar sidecar{};
  sidecar.folder = true;
  sidecar.hasAudioImport = true;
  sidecar.audioImport.forceMono = true;
  sidecar.audioImport.sampleRate = 22050U;
  g_tests.check(
      write_tone(folder + "/step.wav") &&
          engine::content::parse_asset_guid(
              "44444444-5555-4666-8777-888888888889", &sidecar.guid) &&
          engine::content::write_asset_sidecar(folder.c_str(), sidecar),
      "sfx/step.wav and a folder sidecar asking for mono 22050 Hz");
  const SoundHandle sound = load_sound("importfx/sfx/step.wav");
  std::uint32_t channels = 0U;
  std::uint32_t sampleRate = 0U;
  std::uint64_t frames = 0U;
  g_tests.check(
      (sound != kInvalidSound) &&
          audio_sound_format(sound, &channels, &sampleRate, &frames) &&
          (channels == 1U) && (sampleRate == 22050U) &&
          (frames == kFrames / 2U),
      "the folder's settings decode the sound below it");
  unload_sound(sound);
  update_audio();
  std::filesystem::remove(folder + ".meta", ec);
}

} // namespace

/// Runs the audio import settings suite.
int main() {
  std::error_code ec{};
  std::filesystem::remove_all(kDirectory, ec);
  std::filesystem::create_directories(kDirectory, ec);
  const std::string osPath = std::string(kDirectory) + "/tone.wav";
  if (!write_tone(osPath) || !engine::core::initialize_vfs() ||
      !engine::core::mount("importfx", kDirectory)) {
    g_tests.fail("write and mount the tone");
    return g_tests.finish("audio import settings");
  }
  engine::audio::AudioConfig config{};
  config.nullDevice = true;
  if (!engine::audio::initialize_audio(config)) {
    g_tests.fail("initialize audio on the null device");
  } else {
    check_settings_reach_the_decode(osPath);
    check_folder_settings_reach_the_decode();
    engine::audio::shutdown_audio();
  }
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(kDirectory, ec);
  return g_tests.finish("audio import settings");
}
