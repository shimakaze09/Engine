// Implements audio behavior for the Engine audio system.

#include "engine/audio/audio.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"

// Silence warnings from miniaudio in third-party code.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-out-of-range-compare"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-result"
#endif

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#include "miniaudio.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace engine::audio {

namespace {

constexpr std::size_t kMaxSounds = 256U;
constexpr unsigned kSoundSlotBits = 9U;
constexpr std::uint32_t kSoundSlotMask = (1U << kSoundSlotBits) - 1U;
// A handle packs (generation << kSoundSlotBits) | slotToken into 32 bits, so
// the generation counter must wrap within the bits that survive the encode or
// old slots would eventually mint handles that never validate.
constexpr std::uint32_t kSoundGenerationBits = 32U - kSoundSlotBits;
constexpr std::uint32_t kSoundGenerationMask =
    (1U << kSoundGenerationBits) - 1U;

static_assert(kMaxSounds < (1U << kSoundSlotBits),
              "slot tokens (slot + 1) must fit the handle slot bits");

struct SoundEntry final {
  bool active = false;
  std::uint32_t generation = 1U;
  ma_decoder decoder{};
  ma_sound sound{};
  void *fileData = nullptr;
  std::size_t fileSize = 0U;
};

constexpr std::size_t kMaxOneShotInstances = 32U;

/// One fire-and-forget playback: its own decoder over the source sound's
/// retained file buffer plus the playing ma_sound; sourceSlot lets
/// unload_sound kill instances whose buffer is going away.
struct OneShotInstance final {
  bool active = false;
  std::size_t sourceSlot = 0U;
  ma_decoder decoder{};
  ma_sound sound{};
};

struct AudioState final {
  bool initialized = false;
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

/// Releases one one-shot instance's playback resources.
void reset_one_shot(OneShotInstance &instance) noexcept {
  if (!instance.active) {
    return;
  }
  ma_sound_uninit(&instance.sound);
  ma_decoder_uninit(&instance.decoder);
  instance = OneShotInstance{};
}

/// True when every component is a finite float; positions and listener
/// vectors cross the public API boundary and a NaN would silently poison
/// the spatializer (audit M-29).
bool finite_vec(const math::Vec3 &v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Validates playback params: volume finite and non-negative, pitch finite
/// and positive (miniaudio requires pitch > 0). Invalid params reject the
/// playback call (audit M-29).
bool valid_play_params(const PlayParams &params) noexcept {
  if (!std::isfinite(params.volume) || (params.volume < 0.0F) ||
      !std::isfinite(params.pitch) || (params.pitch <= 0.0F)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "rejected non-finite/out-of-range play params");
    return false;
  }
  return true;
}

/// Rejects enum values outside the declared buses before any array
/// indexing — AudioBus arrives across the public API boundary and a
/// caller-forged value would otherwise write past busVolumes (audit
/// H-22).
bool bus_valid(AudioBus bus) noexcept {
  return static_cast<std::uint8_t>(bus) <=
         static_cast<std::uint8_t>(AudioBus::Sfx);
}

/// Group routing for a bus; nullptr = the engine endpoint (Master).
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

/// Starts a pooled one-shot from the entry's retained buffer; positional
/// playback spatializes at `position`.
bool start_one_shot(SoundEntry *entry, std::size_t sourceSlot,
                    const PlayParams &params, AudioBus bus, bool positional,
                    const math::Vec3 &position) noexcept {
  if ((entry == nullptr) || (entry->fileData == nullptr)) {
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
    static bool warned = false;
    if (!warned) {
      warned = true;
      core::log_message(core::LogLevel::Warning, "audio",
                        "one-shot pool exhausted; sound dropped");
    }
    return false;
  }

  OneShotInstance &instance = g_audio.oneShots[slot];
  ma_decoder_config decoderConfig = ma_decoder_config_init_default();
  if (ma_decoder_init_memory(entry->fileData, entry->fileSize, &decoderConfig,
                             &instance.decoder) != MA_SUCCESS) {
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to decode one-shot instance");
    return false;
  }

  const ma_uint32 flags =
      positional ? 0U : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);
  if (ma_sound_init_from_data_source(
          &g_audio.engine, &instance.decoder, flags,
          bus_group(bus), &instance.sound) != MA_SUCCESS) {
    ma_decoder_uninit(&instance.decoder);
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to create one-shot instance");
    return false;
  }

  instance.active = true;
  instance.sourceSlot = sourceSlot;
  ma_sound_set_volume(&instance.sound, params.volume);
  ma_sound_set_pitch(&instance.sound, params.pitch);
  ma_sound_set_looping(&instance.sound, MA_FALSE);
  if (positional) {
    ma_sound_set_position(&instance.sound, position.x, position.y,
                          position.z);
  }
  if (ma_sound_start(&instance.sound) != MA_SUCCESS) {
    reset_one_shot(instance);
    return false;
  }
  return true;
}

/// Advances a generation counter within the handle-encodable width,
/// skipping zero.
std::uint32_t next_sound_generation(std::uint32_t generation) noexcept {
  generation = (generation + 1U) & kSoundGenerationMask;
  if (generation == 0U) {
    generation = 1U;
  }
  return generation;
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
  entry = SoundEntry{false, next_sound_generation(entry.generation), {}, {},
                     nullptr};
}

} // namespace

/// Initializes the owning system for audio.
bool initialize_audio() noexcept {
  if (g_audio.initialized) {
    return true;
  }

  ma_engine_config config = ma_engine_config_init();
  config.noDevice = MA_FALSE;

  const ma_result result = ma_engine_init(&config, &g_audio.engine);
  if (result != MA_SUCCESS) {
    core::log_message(
        core::LogLevel::Error, "audio", "failed to initialize audio engine");
    return false;
  }

  // Both groups or neither: a partial pair would leak the first group,
  // because shutdown releases them only when busesReady is set (audit
  // H-22).
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

  g_audio.initialized = true;
  core::log_message(core::LogLevel::Info, "audio", "audio initialized");
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
    ma_sound_uninit(&entry.sound);
    ma_decoder_uninit(&entry.decoder);
    if (entry.fileData != nullptr) {
      core::vfs_free(entry.fileData);
      entry.fileData = nullptr;
    }
    reset_sound_entry(entry);
  }
}

void shutdown_audio() noexcept {
  if (!g_audio.initialized) {
    return;
  }

  for (auto &instance : g_audio.oneShots) {
    reset_one_shot(instance);
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
    ma_sound_uninit(&entry.sound);
    ma_decoder_uninit(&entry.decoder);
    if (entry.fileData != nullptr) {
      core::vfs_free(entry.fileData);
      entry.fileData = nullptr;
    }
    reset_sound_entry(entry);
  }

  ma_engine_uninit(&g_audio.engine);
  g_audio.engine = ma_engine{};
  g_audio.initialized = false;

  core::log_message(core::LogLevel::Info, "audio", "audio shut down");
}

/// Per-frame audio hook: recycles finished one-shot instances so the
/// pool never leaks slots (device-driven playback needs no other pump).
void update_audio() noexcept {
  if (!g_audio.initialized) {
    return;
  }
  for (auto &instance : g_audio.oneShots) {
    if (instance.active &&
        (ma_sound_is_playing(&instance.sound) == MA_FALSE)) {
      reset_one_shot(instance);
    }
  }
}

/// Loads the requested resource for sound.
SoundHandle load_sound(const char *virtualPath) noexcept {
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

  void *fileData = nullptr;
  std::size_t fileSize = 0U;
  if (!core::vfs_read_binary(virtualPath, &fileData, &fileSize)) {
    core::log_message(
        core::LogLevel::Error, "audio", "failed to read sound file via VFS");
    return kInvalidSound;
  }

  SoundEntry &entry = g_audio.sounds[slot];
  entry.fileData = fileData;
  entry.fileSize = fileSize;

  ma_decoder_config decoderConfig = ma_decoder_config_init_default();
  ma_result res = ma_decoder_init_memory(
      fileData, fileSize, &decoderConfig, &entry.decoder);
  if (res != MA_SUCCESS) {
    core::vfs_free(fileData);
    entry.fileData = nullptr;
    core::log_message(
        core::LogLevel::Error, "audio", "failed to decode sound file");
    return kInvalidSound;
  }

  res = ma_sound_init_from_data_source(
      &g_audio.engine, &entry.decoder, 0U, nullptr, &entry.sound);
  if (res != MA_SUCCESS) {
    ma_decoder_uninit(&entry.decoder);
    core::vfs_free(fileData);
    entry.fileData = nullptr;
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

  ma_sound_uninit(&entry->sound);
  ma_decoder_uninit(&entry->decoder);
  if (entry->fileData != nullptr) {
    core::vfs_free(entry->fileData);
    entry->fileData = nullptr;
  }
  reset_sound_entry(*entry);
}

bool play_sound(SoundHandle handle, const PlayParams &params) noexcept {
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
/// sound, every pooled one-shot instance, and the streamed music track
/// (audit M-29 — the name is now literal).
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
/// clamping, and validation stay consistent with set_bus_volume
/// (audit M-29).
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

bool play_sound_at(SoundHandle handle, const math::Vec3 &position,
                   const PlayParams &params, AudioBus bus) noexcept {
  SoundEntry *entry = lookup_sound_entry(handle);
  if (entry == nullptr) {
    return false;
  }
  const std::size_t sourceSlot =
      static_cast<std::size_t>(entry - &g_audio.sounds[0]);
  return start_one_shot(entry, sourceSlot, params, bus, true, position);
}

bool play_sound_oneshot(SoundHandle handle, const PlayParams &params,
                        AudioBus bus) noexcept {
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
  if ((virtualPath == nullptr) || !g_audio.initialized) {
    return false;
  }
  if (!std::isfinite(volume) || (volume < 0.0F)) {
    core::log_message(core::LogLevel::Error, "audio",
                      "rejected non-finite/negative music volume");
    return false;
  }

  char osPath[1024] = {};
  if (!core::vfs_resolve_os_path(virtualPath, osPath, sizeof(osPath))) {
    core::log_message(core::LogLevel::Error, "audio",
                      "music path did not resolve");
    return false;
  }

  stop_music();
  if (ma_sound_init_from_file(&g_audio.engine, osPath,
                              MA_SOUND_FLAG_STREAM, bus_group(AudioBus::Music),
                              nullptr, &g_audio.music) != MA_SUCCESS) {
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to open music stream");
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
