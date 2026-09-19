// Sound handle encoding shared by audio.cpp and its unit test: the
// slot/generation split of the 32-bit SoundHandle and the generation
// advance that skips the invalid encoding. Private to the audio module;
// the test includes it to drive the generation counter through its wrap
// without loading eight million sounds.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::audio {

inline constexpr std::size_t kMaxSounds = 256U;
inline constexpr unsigned kSoundSlotBits = 9U;
inline constexpr std::uint32_t kSoundSlotMask = (1U << kSoundSlotBits) - 1U;
// A handle packs (generation << kSoundSlotBits) | slotToken into 32 bits, so
// the generation counter must wrap within the bits that survive the encode or
// old slots would eventually mint handles that never validate.
inline constexpr std::uint32_t kSoundGenerationBits = 32U - kSoundSlotBits;
inline constexpr std::uint32_t kSoundGenerationMask =
    (1U << kSoundGenerationBits) - 1U;

static_assert(kMaxSounds < (1U << kSoundSlotBits),
              "slot tokens (slot + 1) must fit the handle slot bits");

/// Advances a generation counter within the handle-encodable width,
/// skipping zero.
constexpr std::uint32_t
next_sound_generation(std::uint32_t generation) noexcept {
  generation = (generation + 1U) & kSoundGenerationMask;
  if (generation == 0U) {
    generation = 1U;
  }
  return generation;
}

} // namespace engine::audio
