// Counters private to the audio module that its unit test reads to prove
// a cost is paid where the contract says: a decoder is opened once per
// sound load and never per playback.

#pragma once

#include <cstddef>

namespace engine::audio {

/// Decoders opened since the process started. Loading a sound opens one;
/// playing it, as a one-shot or not, opens none.
std::size_t audio_decoder_opens() noexcept;

} // namespace engine::audio
