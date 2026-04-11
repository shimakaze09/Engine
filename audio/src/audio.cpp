#include "engine/audio/audio.h"

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

struct SoundEntry final {
  bool active = false;
  ma_decoder decoder{};
  ma_sound sound{};
  void *fileData = nullptr;
};

struct AudioState final {
  bool initialized = false;
  ma_engine engine{};
  SoundEntry sounds[kMaxSounds] = {};
};

AudioState g_audio{};

} // namespace

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

  g_audio.initialized = true;
  core::log_message(core::LogLevel::Info, "audio", "audio initialized");
  return true;
}

void shutdown_audio() noexcept {
  if (!g_audio.initialized) {
    return;
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
    entry.active = false;
  }

  ma_engine_uninit(&g_audio.engine);
  g_audio = AudioState{};

  core::log_message(core::LogLevel::Info, "audio", "audio shut down");
}

void update_audio() noexcept {
  // miniaudio's default device-driven mode handles playback on its own
  // thread.  Nothing to pump here, but the function is kept as a hook for
  // future per-frame work (spatial position updates, etc.).
}

SoundHandle load_sound(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || !g_audio.initialized) {
    return kInvalidSound;
  }

  // Find a free slot.
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

  // Read the file through VFS.
  void *fileData = nullptr;
  std::size_t fileSize = 0U;
  if (!core::vfs_read_binary(virtualPath, &fileData, &fileSize)) {
    core::log_message(
        core::LogLevel::Error, "audio", "failed to read sound file via VFS");
    return kInvalidSound;
  }

  SoundEntry &entry = g_audio.sounds[slot];
  entry.fileData = fileData;

  // Decode from memory.
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

  // Create sound from data source.
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
  return SoundHandle{static_cast<std::uint32_t>(slot + 1U)};
}

void unload_sound(SoundHandle handle) noexcept {
  if ((handle.id == 0U) || (handle.id > kMaxSounds) || !g_audio.initialized) {
    return;
  }

  SoundEntry &entry = g_audio.sounds[handle.id - 1U];
  if (!entry.active) {
    return;
  }

  ma_sound_uninit(&entry.sound);
  ma_decoder_uninit(&entry.decoder);
  if (entry.fileData != nullptr) {
    core::vfs_free(entry.fileData);
    entry.fileData = nullptr;
  }
  entry.active = false;
}

bool play_sound(SoundHandle handle, const PlayParams &params) noexcept {
  if ((handle.id == 0U) || (handle.id > kMaxSounds) || !g_audio.initialized) {
    return false;
  }

  SoundEntry &entry = g_audio.sounds[handle.id - 1U];
  if (!entry.active) {
    return false;
  }

  ma_sound_set_volume(&entry.sound, params.volume);
  ma_sound_set_pitch(&entry.sound, params.pitch);
  ma_sound_set_looping(&entry.sound, params.loop ? MA_TRUE : MA_FALSE);

  // Rewind to start before playing.
  ma_sound_seek_to_pcm_frame(&entry.sound, 0);

  const ma_result res = ma_sound_start(&entry.sound);
  return res == MA_SUCCESS;
}

void stop_sound(SoundHandle handle) noexcept {
  if ((handle.id == 0U) || (handle.id > kMaxSounds) || !g_audio.initialized) {
    return;
  }

  SoundEntry &entry = g_audio.sounds[handle.id - 1U];
  if (entry.active) {
    ma_sound_stop(&entry.sound);
  }
}

void stop_all() noexcept {
  if (!g_audio.initialized) {
    return;
  }

  for (auto &entry : g_audio.sounds) {
    if (entry.active) {
      ma_sound_stop(&entry.sound);
    }
  }
}

void set_master_volume(float volume) noexcept {
  if (!g_audio.initialized) {
    return;
  }

  ma_engine_set_volume(&g_audio.engine, volume);
}

} // namespace engine::audio
