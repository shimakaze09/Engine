// Declares audio types and APIs for the Engine audio system.

#pragma once

#include <cstdint>

namespace engine::audio {

/// Opaque id of a loaded sound (0 = invalid; generation-encoded).
struct SoundHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const SoundHandle &,
                                   const SoundHandle &) = default;
};

inline constexpr SoundHandle kInvalidSound{};

/// Playback settings: volume, pitch, and looping.
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
/// Releases the sound's slot; the handle becomes stale.
void unload_sound(SoundHandle handle) noexcept;

/// Starts playback with the given params; false for stale handles.
bool play_sound(SoundHandle handle, const PlayParams &params) noexcept;
/// Stops all playback of this sound.
void stop_sound(SoundHandle handle) noexcept;
/// Stops every playing sound.
void stop_all() noexcept;

/// Sets the requested value for master volume.
void set_master_volume(float volume) noexcept;

} // namespace engine::audio
