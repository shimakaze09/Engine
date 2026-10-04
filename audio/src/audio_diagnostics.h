// Counters private to the audio module that its unit test reads to prove
// a cost is paid where the contract says: a decoder is opened once per
// sound load and never per playback, and the mixer allocates while it is
// set up and while sounds load, never while they play. With them, what is
// loaded and playing, the format a loaded sound decoded to, and a way to
// run the mixer on the null device, which has no device thread to.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/audio/audio.h"

namespace engine::audio {

/// Decoders opened since the process started. Loading a sound opens one;
/// playing it, as a one-shot or not, opens none.
std::size_t audio_decoder_opens() noexcept;

/// Heap allocations the mixer (the miniaudio engine and every sound on it)
/// has made since the process started. Playback, one-shots included, adds
/// none.
std::size_t audio_mixer_allocations() noexcept;

/// Sounds loaded now: distinct paths, each counted once however many
/// references it holds.
std::size_t audio_loaded_sound_count() noexcept;

/// The decoded format of the loaded sound `handle`: its channels, its
/// sample rate in Hz and its length in frames, each written when given.
/// False for a handle that names no loaded sound.
bool audio_sound_format(SoundHandle handle, std::uint32_t *channels,
                        std::uint32_t *sampleRate,
                        std::uint64_t *frames) noexcept;

/// Whether a music track is open.
bool audio_music_active() noexcept;

/// Mixes `frames` frames on the null device, as a device thread would, so
/// sounds advance and finish. Writes the peak absolute sample mixed to
/// `peak`, and the left and right channels' own peaks to `leftPeak` and
/// `rightPeak`, each when given. False when audio is not running on the
/// null device.
bool audio_mix_null_device(std::uint32_t frames, float *peak,
                           float *leftPeak = nullptr,
                           float *rightPeak = nullptr) noexcept;

} // namespace engine::audio
