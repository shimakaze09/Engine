// Counters private to the audio module that its unit test reads to prove
// a cost is paid where the contract says: a decoder is opened once per
// sound load and never per playback, and the mixer allocates while it is
// set up and while sounds load, never while they play. With them, a way
// to run the mixer on the null device, which has no device thread to.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::audio {

/// Decoders opened since the process started. Loading a sound opens one;
/// playing it, as a one-shot or not, opens none.
std::size_t audio_decoder_opens() noexcept;

/// Heap allocations the mixer (the miniaudio engine and every sound on it)
/// has made since the process started. Playback, one-shots included, adds
/// none.
std::size_t audio_mixer_allocations() noexcept;

/// Mixes `frames` frames on the null device, as a device thread would, so
/// sounds advance and finish. Writes the peak absolute sample mixed to
/// `peak`, and the left and right channels' own peaks to `leftPeak` and
/// `rightPeak`, each when given. False when audio is not running on the
/// null device.
bool audio_mix_null_device(std::uint32_t frames, float *peak,
                           float *leftPeak = nullptr,
                           float *rightPeak = nullptr) noexcept;

} // namespace engine::audio
