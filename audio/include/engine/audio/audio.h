// Declares audio types and APIs for the Engine audio system.

#pragma once

#include <cstdint>

namespace engine::audio {

/// Stores sound handle data used by the engine.
struct SoundHandle final {
  std::uint32_t id = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const SoundHandle &,
                                   const SoundHandle &) = default;
};

inline constexpr SoundHandle kInvalidSound{};

/// Stores play params data used by the engine.
struct PlayParams final {
  float volume = 1.0F;
  float pitch = 1.0F;
  bool loop = false;
};

/// Initializes the owning system for audio.
bool initialize_audio() noexcept;
/// Shuts down the owning system for audio.
void shutdown_audio() noexcept;

// Drive the audio engine pump.  Call once per frame.
void update_audio() noexcept;

// Load a sound from a VFS path (.wav, .mp3, .ogg, .flac).
SoundHandle load_sound(const char *virtualPath) noexcept;
/// Handles unload sound.
void unload_sound(SoundHandle handle) noexcept;

/// Handles play sound.
bool play_sound(SoundHandle handle, const PlayParams &params) noexcept;
/// Handles stop sound.
void stop_sound(SoundHandle handle) noexcept;
/// Handles stop all.
void stop_all() noexcept;

/// Sets the requested value for master volume.
void set_master_volume(float volume) noexcept;

} // namespace engine::audio
