#pragma once

#include <cstdint>

namespace engine::audio {

struct SoundHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const SoundHandle &,
                                   const SoundHandle &) = default;
};

inline constexpr SoundHandle kInvalidSound{};

struct PlayParams final {
  float volume = 1.0F;
  float pitch = 1.0F;
  bool loop = false;
};

bool initialize_audio() noexcept;
void shutdown_audio() noexcept;

// Drive the audio engine pump.  Call once per frame.
void update_audio() noexcept;

// Load a sound from a VFS path (.wav, .mp3, .ogg, .flac).
SoundHandle load_sound(const char *virtualPath) noexcept;
void unload_sound(SoundHandle handle) noexcept;

bool play_sound(SoundHandle handle, const PlayParams &params) noexcept;
void stop_sound(SoundHandle handle) noexcept;
void stop_all() noexcept;

void set_master_volume(float volume) noexcept;

} // namespace engine::audio
