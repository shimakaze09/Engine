// Declares audio types and APIs for the Engine audio system: loaded
// sound handles, mixer buses, 3D one-shot playback from a fixed instance
// pool, the camera-following listener, and streamed music.

#pragma once

#include <cstdint>

#include "engine/math/vec3.h"

namespace engine::audio {

/// Opaque id of a loaded sound (0 = invalid; generation-encoded).
struct SoundHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const SoundHandle &,
                                   const SoundHandle &) = default;
};

inline constexpr SoundHandle kInvalidSound{};

/// Playback settings: volume, pitch, and looping. Playback calls validate
/// them: volume must be finite and >= 0, pitch finite and > 0; invalid
/// params make the call return false with a logged diagnostic.
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
// Releases every loaded sound, live one-shot, and the streamed music while
// the device stays up: sounds are run-scoped scene content and must not
// survive EnginePipeline::teardown into a later run (#168).
void unload_all_sounds() noexcept;
/// Releases the sound's slot; the handle becomes stale.
void unload_sound(SoundHandle handle) noexcept;

/// Starts playback with the given params; false for stale handles or
/// invalid params.
bool play_sound(SoundHandle handle, const PlayParams &params) noexcept;
/// Stops all playback of this sound.
void stop_sound(SoundHandle handle) noexcept;
/// Stops everything audible: direct playback of loaded sounds, all pooled
/// one-shot instances, and the streamed music track.
void stop_all() noexcept;

/// Sets the Master bus volume; identical to set_bus_volume(Master, v) —
/// clamped at 0, stored for bus_volume, non-finite input ignored.
void set_master_volume(float volume) noexcept;

/// Mixer buses one-shot and music playback routes through; Master scales
/// everything (it forwards to the engine volume).
enum class AudioBus : std::uint8_t {
  Master = 0,
  Music = 1,
  Sfx = 2,
};

/// Sets one bus's volume (clamped at 0, non-finite ignored; Master
/// forwards to the engine endpoint).
void set_bus_volume(AudioBus bus, float volume) noexcept;
/// Last volume set on the bus (1 when audio is unavailable).
float bus_volume(AudioBus bus) noexcept;

/// Places the 3D listener; the pipeline follows the active camera each
/// frame (forward need not be normalized). Non-finite vectors are
/// rejected and the previous listener transform is kept.
void set_listener(const math::Vec3 &position, const math::Vec3 &forward,
                  const math::Vec3 &up) noexcept;

/// Fire-and-forget spatialized one-shot at a world position on the given
/// bus. Instances come from a fixed pool recycled by update_audio; loop
/// is ignored so a one-shot can never pin a pool slot. False when the
/// handle is stale or the pool is exhausted (logged once).
bool play_sound_at(SoundHandle handle, const math::Vec3 &position,
                   const PlayParams &params,
                   AudioBus bus = AudioBus::Sfx) noexcept;

/// Fire-and-forget 2D one-shot on the given bus (same pool and loop rule
/// as play_sound_at).
bool play_sound_oneshot(SoundHandle handle, const PlayParams &params,
                        AudioBus bus = AudioBus::Sfx) noexcept;

/// Streams a music file on the Music bus; one track plays at a time and a
/// new call replaces the current track. The virtual path resolves through
/// the VFS to a loose OS file (archive-backed streaming is still pending);
/// volume must be finite and >= 0 or the call fails.
bool play_music(const char *virtualPath, float volume, bool loop) noexcept;
/// Stops and releases the streamed music track.
void stop_music() noexcept;

} // namespace engine::audio
