// Implements audio behavior for the Engine audio system.

#include "engine/audio/audio.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_diagnostics.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "sound_handle.h"

#if defined(ENGINE_PLATFORM_WEB)
#include <emscripten.h>
#endif

// miniaudio's declarations; its implementation is compiled once, in
// miniaudio_impl.cpp, with the decoders the engine loads.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-out-of-range-compare"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma clang diagnostic ignored "-Wdeprecated-pragma"
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-result"
#endif

#include "miniaudio.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "engine/core/diagnostic.h"
#include "engine/core/thread_affinity.h"

namespace engine::audio {

namespace {

/// A loaded sound: its PCM decoded once at load, and the sound playing it
/// through a reference that allocates nothing. Every one-shot reads the
/// same PCM through a reference of its own, so playback never decodes.
struct SoundEntry final {
  bool active = false;
  std::uint32_t generation = 1U;
  void *pcm = nullptr;
  ma_uint64 pcmFrames = 0U;
  ma_format format = ma_format_unknown;
  ma_uint32 channels = 0U;
  ma_uint32 sampleRate = 0U;
  ma_audio_buffer_ref source{};
  ma_sound sound{};
};

constexpr std::size_t kMaxOneShotInstances = 32U;

/// A mixer voice made once, when audio starts, and re-pointed at a sound's
/// decoded PCM on each playback, so playing allocates nothing: a sound
/// made per playback would build a mixer node, a heap allocation per
/// gameplay sound event. A voice mixes a fixed channel count, fixed when
/// it is made, so each slot holds a mono and a stereo voice.
struct OneShotVoice final {
  bool ready = false;
  ma_audio_buffer_ref source{};
  ma_sound sound{};
};

/// One fire-and-forget playback slot. sourceSlot lets unload_sound stop
/// a playback whose PCM is going away.
struct OneShotInstance final {
  bool active = false;
  std::size_t sourceSlot = 0U;
  OneShotVoice *playing = nullptr;
  OneShotVoice mono{};
  OneShotVoice stereo{};
};

struct AudioState final {
  bool initialized = false;
  bool nullDevice = false;
  bool busesReady = false;
  ma_engine engine{};
  ma_sound_group musicGroup{};
  ma_sound_group sfxGroup{};
  float busVolumes[3] = {1.0F, 1.0F, 1.0F};
  SoundEntry sounds[kMaxSounds] = {};
  OneShotInstance oneShots[kMaxOneShotInstances] = {};
  bool musicActive = false;
  ma_sound music{};
};

AudioState g_audio{};

/// Decoders opened since the process started; playback must open none.
std::size_t g_decoderOpens = 0U;
/// Heap allocations the mixer made, counted by its allocation callbacks.
/// Atomic because miniaudio may allocate from its device thread.
std::atomic<std::size_t> g_mixerAllocations{0U};

void *counted_malloc(std::size_t size, void * /*userData*/) {
  g_mixerAllocations.fetch_add(1U, std::memory_order_relaxed);
  return std::malloc(size);
}

void *counted_realloc(void *block, std::size_t size, void * /*userData*/) {
  g_mixerAllocations.fetch_add(1U, std::memory_order_relaxed);
  return std::realloc(block, size);
}

void counted_free(void *block, void * /*userData*/) { std::free(block); }
/// The one-shot pool's exhaustion warning, once per episode of loaded
/// sounds: reset when they are all unloaded or audio shuts down.
bool g_oneShotPoolWarned = false;

// Input budgets, cvar-configurable and enforced before the bytes they
// bound exist in memory: a sound file is read whole and decoded once at
// load, parsing untrusted bytes, so the bytes read are capped by the
// bounded VFS read on the handle it consumes, and the PCM a header claims
// is capped from the decoder's reported length before the first frame
// decodes. Streamed music never sits in memory whole, so it carries only
// a file cap, and that cap is advisory: it is checked from metadata before
// the stream opens, and a file that grows after the check streams on
// (decision 0021).
constexpr int kDefaultMaxSoundFileBytes = 32 * 1024 * 1024;
constexpr int kDefaultMaxMusicFileBytes = 512 * 1024 * 1024;
constexpr int kDefaultMaxDecodedPcmBytes = 256 * 1024 * 1024;
constexpr const char *kMaxSoundFileBytesCvar = "audio.max_sound_file_bytes";
constexpr const char *kMaxMusicFileBytesCvar = "audio.max_music_file_bytes";
constexpr const char *kMaxDecodedPcmBytesCvar = "audio.max_decoded_pcm_bytes";

/// Registers the budget cvars; a later initialize_audio finds them already
/// present, which registration reports as false and is not a failure.
void register_budget_cvars() noexcept {
  static_cast<void>(core::cvar_register_int(
      kMaxSoundFileBytesCvar, kDefaultMaxSoundFileBytes,
      "Largest file load_sound accepts (bytes); a larger file is refused "
      "before any of it is read"));
  static_cast<void>(core::cvar_register_int(
      kMaxMusicFileBytesCvar, kDefaultMaxMusicFileBytes,
      "Largest file play_music opens (bytes), checked from its size before "
      "the stream opens; advisory, since a file that grows while it streams "
      "is not cut off"));
  static_cast<void>(core::cvar_register_int(
      kMaxDecodedPcmBytesCvar, kDefaultMaxDecodedPcmBytes,
      "Largest decoded PCM a loaded sound's header may claim (bytes); a "
      "larger sound is refused before decoding"));
}

/// Logs one audio diagnostic in the `<path>: <reason>` shape the editor
/// console parses for its navigation actions.
void log_path_error(const char *virtualPath, const char *reason) noexcept {
  core::log_path_diagnostic(core::LogLevel::Error, core::LogChannel::Audio,
                            virtualPath, reason);
}

/// The byte budget a cvar currently holds; a value below zero bounds
/// nothing meaningful and is treated as zero.
std::uint64_t budget_bytes(const char *cvarName, int fallback) noexcept {
  const int budget = core::cvar_get_int(cvarName, fallback);
  return (budget > 0) ? static_cast<std::uint64_t>(budget) : 0U;
}

/// Refuses a file larger than the named budget before any of it is read;
/// the size comes from file metadata, so an oversized input costs no
/// allocation. A missing or unreadable file is refused here as well. The
/// streamed-music path uses this: its bytes are read by miniaudio's own
/// file stream, never allocated whole, so metadata is the only bound
/// available before the stream opens.
bool file_within_budget(const char *virtualPath, const char *cvarName,
                        int fallback) noexcept {
  std::uint64_t fileBytes = 0U;
  if (!core::vfs_file_size(virtualPath, &fileBytes)) {
    log_path_error(virtualPath, "sound file not found or unreadable");
    return false;
  }
  const std::uint64_t limit = budget_bytes(cvarName, fallback);
  if (fileBytes > limit) {
    char reason[192] = {};
    std::snprintf(reason, sizeof(reason),
                  "file of %llu bytes exceeds %s (%llu), an advisory cap "
                  "checked before streaming",
                  static_cast<unsigned long long>(fileBytes), cvarName,
                  static_cast<unsigned long long>(limit));
    log_path_error(virtualPath, reason);
    return false;
  }
  return true;
}

/// Refuses a decoder whose header claims more PCM than the budget before
/// the first frame decodes: frames times the decoder's output frame size.
/// A source that cannot report a length passes, since nothing in its
/// header bounds it; the file cap is what limits such inputs.
bool decoded_pcm_within_budget(ma_decoder &decoder,
                               const char *virtualPath) noexcept {
  ma_uint64 frames = 0U;
  if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS) {
    return true;
  }
  const std::uint64_t bytesPerFrame = static_cast<std::uint64_t>(
      ma_get_bytes_per_frame(decoder.outputFormat, decoder.outputChannels));
  const std::uint64_t limit =
      budget_bytes(kMaxDecodedPcmBytesCvar, kDefaultMaxDecodedPcmBytes);
  // The comparison divides rather than multiplies so a header claiming
  // any frame count stays overflow-free.
  if ((bytesPerFrame == 0U) ||
      (static_cast<std::uint64_t>(frames) > (limit / bytesPerFrame))) {
    char reason[224] = {};
    std::snprintf(reason, sizeof(reason),
                  "header claims %llu PCM frames of %llu bytes, exceeding %s "
                  "(%llu)",
                  static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(bytesPerFrame),
                  kMaxDecodedPcmBytesCvar,
                  static_cast<unsigned long long>(limit));
    log_path_error(virtualPath, reason);
    return false;
  }
  return true;
}

/// Silent PCM a voice reads while no playback holds it, so it never
/// points at a sound's freed PCM.
constexpr float kSilentFrame[2] = {0.0F, 0.0F};

/// Ends a one-shot's playback and returns its slot to the pool. The voice
/// is detached from the mix before its source is pointed back at
/// silence: detaching waits out a mix in progress, so no mix reads the
/// sound's PCM after this returns and the PCM can be freed.
void reset_one_shot(OneShotInstance &instance) noexcept {
  if (!instance.active) {
    return;
  }
  OneShotVoice &voice = *instance.playing;
  ma_sound_stop(&voice.sound);
  ma_node_detach_output_bus(&voice.sound, 0U);
  const ma_uint32 channels = (&voice == &instance.stereo) ? 2U : 1U;
  ma_audio_buffer_ref_uninit(&voice.source);
  static_cast<void>(ma_audio_buffer_ref_init(ma_format_f32, channels,
                                             kSilentFrame, 1U, &voice.source));
  instance.active = false;
  instance.playing = nullptr;
}

/// Makes one pooled voice of `channels` channels over silence, detached
/// from the mix until a playback routes it; false leaves it unusable.
bool make_voice(OneShotVoice &voice, ma_uint32 channels) noexcept {
  if (ma_audio_buffer_ref_init(ma_format_f32, channels, kSilentFrame, 1U,
                               &voice.source) != MA_SUCCESS) {
    return false;
  }
  voice.source.sampleRate = ma_engine_get_sample_rate(&g_audio.engine);
  if (ma_sound_init_from_data_source(&g_audio.engine, &voice.source, 0U,
                                     nullptr, &voice.sound) != MA_SUCCESS) {
    ma_audio_buffer_ref_uninit(&voice.source);
    return false;
  }
  ma_node_detach_output_bus(&voice.sound, 0U);
  voice.ready = true;
  return true;
}

/// Releases a pooled voice at shutdown.
void release_voice(OneShotVoice &voice) noexcept {
  if (!voice.ready) {
    return;
  }
  ma_sound_uninit(&voice.sound);
  ma_audio_buffer_ref_uninit(&voice.source);
  voice = OneShotVoice{};
}

/// True when every component is a finite float; positions and listener
/// vectors cross the public API boundary and a NaN would silently poison
/// the spatializer.
bool finite_vec(const math::Vec3 &v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Validates playback params: volume finite and non-negative, pitch finite
/// and positive (miniaudio requires pitch > 0). Invalid params reject the
/// playback call.
bool valid_play_params(const PlayParams &params) noexcept {
  if (!std::isfinite(params.volume) || (params.volume < 0.0F) ||
      !std::isfinite(params.pitch) || (params.pitch <= 0.0F) ||
      !std::isfinite(params.minDistance) || (params.minDistance <= 0.0F) ||
      !std::isfinite(params.rolloff) || (params.rolloff < 0.0F)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "rejected non-finite/out-of-range play params");
    return false;
  }
  return true;
}

/// Rejects enum values outside the declared buses before any array
/// indexing — AudioBus arrives across the public API boundary and a
/// caller-forged value would otherwise write past busVolumes.
bool bus_valid(AudioBus bus) noexcept {
  return static_cast<std::uint8_t>(bus) <=
         static_cast<std::uint8_t>(AudioBus::Sfx);
}

/// Group routing for a bus; nullptr = the engine endpoint (Master).
ma_sound_group *bus_group(AudioBus bus) noexcept;

/// The mixer node a bus's sounds feed: its group, or the engine endpoint.
ma_node *bus_node(AudioBus bus) noexcept {
  ma_sound_group *group = bus_group(bus);
  return (group != nullptr) ? static_cast<ma_node *>(group)
                            : ma_engine_get_endpoint(&g_audio.engine);
}

ma_sound_group *bus_group(AudioBus bus) noexcept {
  if (!g_audio.busesReady) {
    return nullptr;
  }
  switch (bus) {
  case AudioBus::Music:
    return &g_audio.musicGroup;
  case AudioBus::Sfx:
    return &g_audio.sfxGroup;
  case AudioBus::Master:
    break;
  }
  return nullptr;
}

/// Points a fresh read cursor at the entry's decoded PCM. It allocates
/// nothing: the reference only records where the frames are.
bool init_pcm_source(const SoundEntry &entry,
                     ma_audio_buffer_ref *source) noexcept {
  if (ma_audio_buffer_ref_init(entry.format, entry.channels, entry.pcm,
                               entry.pcmFrames, source) != MA_SUCCESS) {
    return false;
  }
  source->sampleRate = entry.sampleRate;
  return true;
}

/// Releases a loaded entry's sound and PCM (not its slot generation).
void release_sound_pcm(SoundEntry &entry) noexcept {
  ma_sound_uninit(&entry.sound);
  ma_audio_buffer_ref_uninit(&entry.source);
  std::free(entry.pcm);
  entry.pcm = nullptr;
}

/// Starts a pooled one-shot from the entry's decoded PCM; positional
/// playback spatializes at `position`.
bool start_one_shot(SoundEntry *entry, std::size_t sourceSlot,
                    const PlayParams &params, AudioBus bus, bool positional,
                    const math::Vec3 &position) noexcept {
  if ((entry == nullptr) || (entry->pcm == nullptr)) {
    return false;
  }
  if (!valid_play_params(params) || (positional && !finite_vec(position))) {
    return false;
  }

  std::size_t slot = kMaxOneShotInstances;
  for (std::size_t i = 0U; i < kMaxOneShotInstances; ++i) {
    if (!g_audio.oneShots[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == kMaxOneShotInstances) {
    if (!g_oneShotPoolWarned) {
      g_oneShotPoolWarned = true;
      core::log_message(core::LogLevel::Warning, "audio",
                        "one-shot pool exhausted; sound dropped");
    }
    return false;
  }

  // Loading brings every sound to one or two channels; a voice mixes no
  // other count, so anything else is refused rather than misread.
  if ((entry->channels != 1U) && (entry->channels != 2U)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "one-shots play mono or stereo sounds only");
    return false;
  }
  OneShotInstance &instance = g_audio.oneShots[slot];
  OneShotVoice &voice =
      (entry->channels == 1U) ? instance.mono : instance.stereo;
  if (!voice.ready) {
    core::log_message(core::LogLevel::Error, "audio",
                      "no one-shot voice for this channel count");
    return false;
  }
  // Re-pointing allocates nothing: the source only records where the
  // frames are, and the voice takes the sound's sample rate by rescaling
  // its resampler, which the mix does on its next block once the recorded
  // pitch no longer matches.
  ma_audio_buffer_ref_uninit(&voice.source);
  if (!init_pcm_source(*entry, &voice.source)) {
    static_cast<void>(ma_audio_buffer_ref_init(
        ma_format_f32, entry->channels, kSilentFrame, 1U, &voice.source));
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to start a one-shot instance");
    return false;
  }
  voice.sound.engineNode.sampleRate = entry->sampleRate;
  voice.sound.engineNode.oldPitch = -1.0F;
  instance.active = true;
  instance.sourceSlot = sourceSlot;
  instance.playing = &voice;

  ma_sound_set_spatialization_enabled(&voice.sound,
                                      positional ? MA_TRUE : MA_FALSE);
  ma_node_attach_output_bus(&voice.sound, 0U, bus_node(bus), 0U);
  ma_sound_set_volume(&voice.sound, params.volume);
  ma_sound_set_pitch(&voice.sound, params.pitch);
  ma_sound_set_looping(&voice.sound, MA_FALSE);
  if (positional) {
    ma_sound_set_position(&voice.sound, position.x, position.y, position.z);
    // Explicit attenuation: the mixer's own defaults hold full volume
    // only within one metre, which silences everything a third-person
    // camera hears.
    ma_sound_set_attenuation_model(&voice.sound, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(&voice.sound, params.minDistance);
    ma_sound_set_rolloff(&voice.sound, params.rolloff);
  }
  if (ma_sound_start(&voice.sound) != MA_SUCCESS) {
    reset_one_shot(instance);
    return false;
  }
  return true;
}

/// Builds an externally visible handle for a live sound slot.
SoundHandle make_sound_handle(std::size_t slot) noexcept {
  if (slot >= kMaxSounds) {
    return kInvalidSound;
  }

  const SoundEntry &entry = g_audio.sounds[slot];
  const std::uint32_t slotToken = static_cast<std::uint32_t>(slot + 1U);
  return SoundHandle{(entry.generation << kSoundSlotBits) | slotToken};
}

/// Decodes and validates a sound handle against the current slot generation.
SoundEntry *lookup_sound_entry(SoundHandle handle) noexcept {
  if ((handle == kInvalidSound) || !g_audio.initialized) {
    return nullptr;
  }

  const std::uint32_t slotToken = handle.id & kSoundSlotMask;
  const std::uint32_t generation = handle.id >> kSoundSlotBits;
  if ((slotToken == 0U) || (slotToken > kMaxSounds) || (generation == 0U)) {
    return nullptr;
  }

  SoundEntry &entry = g_audio.sounds[slotToken - 1U];
  if (!entry.active || (entry.generation != generation)) {
    return nullptr;
  }

  return &entry;
}

/// Clears a slot's resources and advances its generation.
void reset_sound_entry(SoundEntry &entry) noexcept {
  const std::uint32_t generation = next_sound_generation(entry.generation);
  entry = SoundEntry{};
  entry.generation = generation;
}

} // namespace

std::size_t audio_decoder_opens() noexcept { return g_decoderOpens; }

std::size_t audio_mixer_allocations() noexcept {
  return g_mixerAllocations.load(std::memory_order_relaxed);
}

bool audio_mix_null_device(std::uint32_t frames, float *peak, float *leftPeak,
                           float *rightPeak) noexcept {
  if (!g_audio.initialized || !g_audio.nullDevice) {
    return false;
  }
  // Mixed in fixed chunks into fixed storage, so mixing allocates nothing.
  constexpr std::uint32_t kChunkFrames = 256U;
  static float chunk[kChunkFrames * 8U] = {};
  const ma_uint32 channels = ma_engine_get_channels(&g_audio.engine);
  if ((channels == 0U) || (channels > 8U)) {
    return false;
  }
  float loudest = 0.0F;
  float left = 0.0F;
  float right = 0.0F;
  std::uint32_t remaining = frames;
  while (remaining > 0U) {
    const std::uint32_t count =
        (remaining < kChunkFrames) ? remaining : kChunkFrames;
    if (ma_engine_read_pcm_frames(&g_audio.engine, chunk, count, nullptr) !=
        MA_SUCCESS) {
      return false;
    }
    for (std::uint32_t i = 0U; i < (count * channels); ++i) {
      loudest = std::fmax(loudest, std::fabs(chunk[i]));
      if ((i % channels) == 0U) {
        left = std::fmax(left, std::fabs(chunk[i]));
      } else if ((i % channels) == 1U) {
        right = std::fmax(right, std::fabs(chunk[i]));
      }
    }
    remaining -= count;
  }
  if (peak != nullptr) {
    *peak = loudest;
  }
  if (leftPeak != nullptr) {
    *leftPeak = left;
  }
  if (rightPeak != nullptr) {
    *rightPeak = right;
  }
  return true;
}

bool initialize_audio() noexcept { return initialize_audio(AudioConfig{}); }

bool audio_uses_null_device() noexcept {
  return g_audio.initialized && g_audio.nullDevice;
}

bool audio_is_initialized() noexcept { return g_audio.initialized; }

#if defined(ENGINE_PLATFORM_WEB)
namespace {

/// miniaudio's WebAudio start calls AudioContext.resume() and drops the
/// promise. It stays pending until the page's first user gesture, and a
/// shutdown in between closes the context under it, which rejects it as an
/// unhandled "Cannot resume a closed AudioContext". Observing every resume
/// on this device's context settles that case quietly; a resume that fails
/// while the context is still open is still reported.
void observe_webaudio_resume(const ma_device &device) noexcept {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
  EM_ASM(
      {
        var context = miniaudio.get_device_by_index($0).webaudio;
        var resume = context.resume.bind(context);
        context.resume = function() {
          var pending = resume();
          pending.catch(function(error) {
            if (context.state !== 'closed') {
              console.error('audio: AudioContext resume failed', error);
            }
          });
          return pending;
        };
      },
      device.webaudio.deviceIndex);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

} // namespace
#endif

/// Initializes the owning system for audio.
bool initialize_audio(const AudioConfig &audioConfig) noexcept {
  if (g_audio.initialized) {
    return true;
  }
  register_budget_cvars();

  ma_engine_config config = ma_engine_config_init();
  config.noDevice = audioConfig.nullDevice ? MA_TRUE : MA_FALSE;
  config.allocationCallbacks.onMalloc = &counted_malloc;
  config.allocationCallbacks.onRealloc = &counted_realloc;
  config.allocationCallbacks.onFree = &counted_free;
  if (audioConfig.nullDevice) {
    // Without a device the engine has no format to take over, so the mix
    // format is fixed here.
    config.channels = 2U;
    config.sampleRate = 48000U;
  }
#if defined(ENGINE_PLATFORM_WEB)
  // Started by hand below, once its context's resume is observed.
  config.noAutoStart = MA_TRUE;
#endif

  const ma_result result = ma_engine_init(&config, &g_audio.engine);
  if (result != MA_SUCCESS) {
    core::log_message(
        core::LogLevel::Error, "audio", "failed to initialize audio engine");
    return false;
  }
#if defined(ENGINE_PLATFORM_WEB)
  if (!audioConfig.nullDevice) {
    observe_webaudio_resume(*ma_engine_get_device(&g_audio.engine));
    if (ma_engine_start(&g_audio.engine) != MA_SUCCESS) {
      core::log_message(core::LogLevel::Error, "audio",
                        "failed to start audio engine");
      ma_engine_uninit(&g_audio.engine);
      return false;
    }
  }
#endif

  // Both groups or neither: a partial pair would leak the first group,
  // because shutdown releases them only when busesReady is set.
  const bool musicGroupReady =
      ma_sound_group_init(&g_audio.engine, 0U, nullptr, &g_audio.musicGroup) ==
      MA_SUCCESS;
  const bool sfxGroupReady =
      musicGroupReady &&
      (ma_sound_group_init(&g_audio.engine, 0U, nullptr, &g_audio.sfxGroup) ==
       MA_SUCCESS);
  if (musicGroupReady && !sfxGroupReady) {
    ma_sound_group_uninit(&g_audio.musicGroup);
  }
  g_audio.busesReady = musicGroupReady && sfxGroupReady;
  if (!g_audio.busesReady) {
    core::log_message(core::LogLevel::Warning, "audio",
                      "bus groups unavailable; routing through the engine");
  }
  g_audio.busVolumes[0] = 1.0F;
  g_audio.busVolumes[1] = 1.0F;
  g_audio.busVolumes[2] = 1.0F;

  // Every one-shot voice is made now, so playback never allocates.
  std::size_t voicesMissing = 0U;
  for (auto &instance : g_audio.oneShots) {
    voicesMissing += make_voice(instance.mono, 1U) ? 0U : 1U;
    voicesMissing += make_voice(instance.stereo, 2U) ? 0U : 1U;
  }
  if (voicesMissing > 0U) {
    core::log_message(core::LogLevel::Warning, "audio",
                      "some one-shot voices could not be made; fewer "
                      "one-shots can play at once");
  }

  g_audio.initialized = true;
  g_audio.nullDevice = audioConfig.nullDevice;
  core::log_message(core::LogLevel::Info, "audio",
                    audioConfig.nullDevice ? "audio initialized (no device)"
                                           : "audio initialized");
  return true;
}

/// Shuts down the owning system for audio.
/// Releases run-scoped sound content while the engine/buses stay live.
void unload_all_sounds() noexcept {
  if (!g_audio.initialized) {
    return;
  }

  for (auto &instance : g_audio.oneShots) {
    reset_one_shot(instance);
  }
  stop_music();

  for (auto &entry : g_audio.sounds) {
    if (!entry.active) {
      continue;
    }
    release_sound_pcm(entry);
    reset_sound_entry(entry);
  }
  g_oneShotPoolWarned = false;
}

void shutdown_audio() noexcept {
  if (!g_audio.initialized) {
    return;
  }

  for (auto &instance : g_audio.oneShots) {
    reset_one_shot(instance);
    release_voice(instance.mono);
    release_voice(instance.stereo);
  }
  stop_music();
  if (g_audio.busesReady) {
    ma_sound_group_uninit(&g_audio.musicGroup);
    ma_sound_group_uninit(&g_audio.sfxGroup);
    g_audio.busesReady = false;
  }

  for (auto &entry : g_audio.sounds) {
    if (!entry.active) {
      continue;
    }
    release_sound_pcm(entry);
    reset_sound_entry(entry);
  }
  g_oneShotPoolWarned = false;

  ma_engine_uninit(&g_audio.engine);
  g_audio.engine = ma_engine{};
  g_audio.initialized = false;

  core::log_message(core::LogLevel::Info, "audio", "audio shut down");
}

/// Per-frame audio hook: recycles finished one-shot instances so the
/// pool never leaks slots (device-driven playback needs no other pump).
void update_audio() noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if (!g_audio.initialized) {
    return;
  }
  for (auto &instance : g_audio.oneShots) {
    if (instance.active &&
        (ma_sound_is_playing(&instance.playing->sound) == MA_FALSE)) {
      reset_one_shot(instance);
    }
  }
}

/// Loads the requested resource for sound.
SoundHandle load_sound(const char *virtualPath) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if ((virtualPath == nullptr) || !g_audio.initialized) {
    return kInvalidSound;
  }

  std::size_t slot = kMaxSounds;
  for (std::size_t i = 0U; i < kMaxSounds; ++i) {
    if (!g_audio.sounds[i].active) {
      slot = i;
      break;
    }
  }

  if (slot == kMaxSounds) {
    core::log_message(core::LogLevel::Error, "audio", "sound registry full");
    return kInvalidSound;
  }

  // The file cap is enforced by the read itself, on the size of the handle
  // it consumes, so a file replaced or grown after a metadata check can
  // never allocate past the budget.
  void *fileData = nullptr;
  std::size_t fileSize = 0U;
  const std::uint64_t fileLimit =
      budget_bytes(kMaxSoundFileBytesCvar, kDefaultMaxSoundFileBytes);
  switch (core::vfs_read_binary_bounded(virtualPath, fileLimit, &fileData,
                                        &fileSize)
              .kind) {
  case core::FailureKind::Ok:
    break;
  case core::FailureKind::CapacityExhausted: {
    char reason[192] = {};
    std::snprintf(reason, sizeof(reason),
                  "file of %llu bytes exceeds %s (%llu)",
                  static_cast<unsigned long long>(fileSize),
                  kMaxSoundFileBytesCvar,
                  static_cast<unsigned long long>(fileLimit));
    log_path_error(virtualPath, reason);
    return kInvalidSound;
  }
  case core::FailureKind::NotFound:
    log_path_error(virtualPath, "sound file not found or unreadable");
    return kInvalidSound;
  case core::FailureKind::IoFailed:
  default:
    log_path_error(virtualPath, "failed to read sound file via VFS");
    return kInvalidSound;
  }

  SoundEntry &entry = g_audio.sounds[slot];
  ma_decoder decoder{};
  ma_decoder_config decoderConfig = ma_decoder_config_init_default();
  ++g_decoderOpens;
  ma_result res =
      ma_decoder_init_memory(fileData, fileSize, &decoderConfig, &decoder);
  if ((res == MA_SUCCESS) && (decoder.outputChannels > 2U)) {
    // One-shot voices mix one or two channels, so a sound with more is
    // decoded down to stereo, once, here.
    ma_decoder_uninit(&decoder);
    decoderConfig.channels = 2U;
    ++g_decoderOpens;
    res = ma_decoder_init_memory(fileData, fileSize, &decoderConfig, &decoder);
  }
  if (res != MA_SUCCESS) {
    core::vfs_free(fileData);
    log_path_error(virtualPath, "failed to decode sound file");
    return kInvalidSound;
  }

  // The header is parsed but no frame is decoded yet: a claimed length
  // beyond the PCM budget is refused here, before any frame decodes.
  if (!decoded_pcm_within_budget(decoder, virtualPath)) {
    ma_decoder_uninit(&decoder);
    core::vfs_free(fileData);
    return kInvalidSound;
  }

  // Decoded once, here: every playback of this sound, one-shots included,
  // reads these frames and never parses or allocates a decoder again.
  ma_uint64 frames = 0U;
  const ma_uint32 bytesPerFrame =
      ma_get_bytes_per_frame(decoder.outputFormat, decoder.outputChannels);
  void *pcm = nullptr;
  if ((ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS) &&
      (frames > 0U) && (bytesPerFrame > 0U)) {
    pcm = std::malloc(static_cast<std::size_t>(frames) * bytesPerFrame);
  }
  ma_uint64 framesRead = 0U;
  const bool decoded =
      (pcm != nullptr) &&
      (ma_decoder_read_pcm_frames(&decoder, pcm, frames, &framesRead) ==
       MA_SUCCESS) &&
      (framesRead > 0U);
  entry.format = decoder.outputFormat;
  entry.channels = decoder.outputChannels;
  entry.sampleRate = decoder.outputSampleRate;
  ma_decoder_uninit(&decoder);
  core::vfs_free(fileData);
  if (!decoded) {
    std::free(pcm);
    log_path_error(virtualPath, "failed to decode sound file");
    return kInvalidSound;
  }
  entry.pcm = pcm;
  entry.pcmFrames = framesRead;

  if (!init_pcm_source(entry, &entry.source)) {
    std::free(entry.pcm);
    entry.pcm = nullptr;
    log_path_error(virtualPath, "failed to create sound");
    return kInvalidSound;
  }
  res = ma_sound_init_from_data_source(&g_audio.engine, &entry.source, 0U,
                                       nullptr, &entry.sound);
  if (res != MA_SUCCESS) {
    ma_audio_buffer_ref_uninit(&entry.source);
    std::free(entry.pcm);
    entry.pcm = nullptr;
    core::log_message(core::LogLevel::Error, "audio", "failed to create sound");
    return kInvalidSound;
  }

  entry.active = true;
  return make_sound_handle(slot);
}

void unload_sound(SoundHandle handle) noexcept {
  SoundEntry *entry = lookup_sound_entry(handle);
  if (entry == nullptr) {
    return;
  }

  const std::size_t sourceSlot =
      static_cast<std::size_t>(entry - &g_audio.sounds[0]);
  for (auto &instance : g_audio.oneShots) {
    if (instance.active && (instance.sourceSlot == sourceSlot)) {
      reset_one_shot(instance);
    }
  }

  release_sound_pcm(*entry);
  reset_sound_entry(*entry);
}

bool play_sound(SoundHandle handle, const PlayParams &params) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  SoundEntry *entry = lookup_sound_entry(handle);
  if ((entry == nullptr) || !valid_play_params(params)) {
    return false;
  }

  ma_sound_set_volume(&entry->sound, params.volume);
  ma_sound_set_pitch(&entry->sound, params.pitch);
  ma_sound_set_looping(&entry->sound, params.loop ? MA_TRUE : MA_FALSE);

  ma_sound_seek_to_pcm_frame(&entry->sound, 0);

  const ma_result res = ma_sound_start(&entry->sound);
  return res == MA_SUCCESS;
}

void stop_sound(SoundHandle handle) noexcept {
  SoundEntry *entry = lookup_sound_entry(handle);
  if (entry != nullptr) {
    ma_sound_stop(&entry->sound);
  }
}

/// Stops everything that can be audible: direct playback of every loaded
/// sound, every pooled one-shot instance, and the streamed music track.
void stop_all() noexcept {
  if (!g_audio.initialized) {
    return;
  }

  for (auto &entry : g_audio.sounds) {
    if (entry.active) {
      ma_sound_stop(&entry.sound);
    }
  }
  for (auto &instance : g_audio.oneShots) {
    reset_one_shot(instance);
  }
  stop_music();
}

/// Master volume routes through the Master bus so the stored value,
/// clamping, and validation stay consistent with set_bus_volume.
void set_master_volume(float volume) noexcept {
  set_bus_volume(AudioBus::Master, volume);
}

void set_bus_volume(AudioBus bus, float volume) noexcept {
  if (!g_audio.initialized || !bus_valid(bus)) {
    return;
  }
  if (!std::isfinite(volume)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "rejected non-finite bus volume");
    return;
  }
  const float clamped = (volume > 0.0F) ? volume : 0.0F;
  g_audio.busVolumes[static_cast<std::size_t>(bus)] = clamped;
  switch (bus) {
  case AudioBus::Master:
    ma_engine_set_volume(&g_audio.engine, clamped);
    break;
  case AudioBus::Music:
    if (g_audio.busesReady) {
      ma_sound_group_set_volume(&g_audio.musicGroup, clamped);
    }
    break;
  case AudioBus::Sfx:
    if (g_audio.busesReady) {
      ma_sound_group_set_volume(&g_audio.sfxGroup, clamped);
    }
    break;
  }
}

float bus_volume(AudioBus bus) noexcept {
  if (!g_audio.initialized || !bus_valid(bus)) {
    return 1.0F;
  }
  return g_audio.busVolumes[static_cast<std::size_t>(bus)];
}

void set_listener(const math::Vec3 &position, const math::Vec3 &forward,
                  const math::Vec3 &up) noexcept {
  if (!g_audio.initialized) {
    return;
  }
  if (!finite_vec(position) || !finite_vec(forward) || !finite_vec(up)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "rejected non-finite listener transform");
    return;
  }
  ma_engine_listener_set_position(&g_audio.engine, 0U, position.x, position.y,
                                  position.z);
  ma_engine_listener_set_direction(&g_audio.engine, 0U, forward.x, forward.y,
                                   forward.z);
  ma_engine_listener_set_world_up(&g_audio.engine, 0U, up.x, up.y, up.z);
}

float distance_gain(float distance, const PlayParams &params) noexcept {
  if (!std::isfinite(distance) || (distance <= params.minDistance)) {
    return 1.0F;
  }
  if (!std::isfinite(params.minDistance) || (params.minDistance <= 0.0F) ||
      !std::isfinite(params.rolloff) || (params.rolloff < 0.0F)) {
    return 1.0F;
  }
  return params.minDistance /
         (params.minDistance +
          (params.rolloff * (distance - params.minDistance)));
}

bool play_sound_at(SoundHandle handle, const math::Vec3 &position,
                   const PlayParams &params, AudioBus bus) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  SoundEntry *entry = lookup_sound_entry(handle);
  if (entry == nullptr) {
    return false;
  }
  // A sound too far to be heard never takes one of the fixed instance
  // slots: the pool is better spent on one that can be.
  if (g_audio.initialized && valid_play_params(params)) {
    const ma_vec3f listener =
        ma_engine_listener_get_position(&g_audio.engine, 0U);
    const math::Vec3 toListener =
        math::sub(position, math::Vec3(listener.x, listener.y, listener.z));
    const float distance = math::length(toListener);
    if ((params.volume * distance_gain(distance, params)) < kInaudibleGain) {
      return false;
    }
  }
  const std::size_t sourceSlot =
      static_cast<std::size_t>(entry - &g_audio.sounds[0]);
  return start_one_shot(entry, sourceSlot, params, bus, true, position);
}

bool play_sound_oneshot(SoundHandle handle, const PlayParams &params,
                        AudioBus bus) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  SoundEntry *entry = lookup_sound_entry(handle);
  if (entry == nullptr) {
    return false;
  }
  const std::size_t sourceSlot =
      static_cast<std::size_t>(entry - &g_audio.sounds[0]);
  return start_one_shot(entry, sourceSlot, params, bus, false,
                        math::Vec3(0.0F, 0.0F, 0.0F));
}

bool play_music(const char *virtualPath, float volume, bool loop) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if ((virtualPath == nullptr) || !g_audio.initialized) {
    return false;
  }
  if (!std::isfinite(volume) || (volume < 0.0F)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "rejected non-finite/negative music volume");
    return false;
  }

  if (!file_within_budget(virtualPath, kMaxMusicFileBytesCvar,
                          kDefaultMaxMusicFileBytes)) {
    return false;
  }

  char osPath[1024] = {};
  if (!core::vfs_resolve_os_path(virtualPath, osPath, sizeof(osPath))) {
    log_path_error(virtualPath, "music path did not resolve");
    return false;
  }

  stop_music();
  if (ma_sound_init_from_file(&g_audio.engine, osPath,
                              MA_SOUND_FLAG_STREAM, bus_group(AudioBus::Music),
                              nullptr, &g_audio.music) != MA_SUCCESS) {
    log_path_error(virtualPath, "failed to open music stream");
    return false;
  }
  g_audio.musicActive = true;
  ma_sound_set_volume(&g_audio.music, volume);
  ma_sound_set_looping(&g_audio.music, loop ? MA_TRUE : MA_FALSE);
  if (ma_sound_start(&g_audio.music) != MA_SUCCESS) {
    stop_music();
    return false;
  }
  return true;
}

void stop_music() noexcept {
  if (!g_audio.musicActive) {
    return;
  }
  ma_sound_stop(&g_audio.music);
  ma_sound_uninit(&g_audio.music);
  g_audio.music = ma_sound{};
  g_audio.musicActive = false;
}

} // namespace engine::audio
