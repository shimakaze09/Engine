// The engine's random stream: xoshiro256** over explicit state, seeded
// through splitmix64. Every value comes from state the caller holds, so a
// run is reproducible from its seed and a stream can be hashed, copied and
// restored like any other simulation state. Nothing here reads a clock or
// an operating-system entropy source; seeding is always the caller's
// decision (see docs/decisions/0019).

#pragma once

#include <cstdint>

#include "engine/core/hash.h"

namespace engine::core {

/// One random stream's whole state. Trivially copyable on purpose: a
/// stream is saved, restored and compared like the rest of simulation
/// state, and a copy advances independently of its origin.
struct Rng final {
  std::uint64_t state[4] = {0U, 0U, 0U, 0U};
};

/// Mixes one 64-bit value, advancing `state` — splitmix64, used to fill a
/// stream from a single seed. Exposed because it is also the right way to
/// derive one seed from another (a per-scene seed from a run seed, say)
/// without correlating the resulting streams.
constexpr std::uint64_t splitmix64_next(std::uint64_t *state) noexcept {
  *state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = *state;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

/// A stream from one seed. Every seed, zero included, produces a usable
/// stream: splitmix64 has no fixed point at zero, so the all-zero state
/// xoshiro cannot recover from is unreachable here.
constexpr Rng rng_from_seed(std::uint64_t seed) noexcept {
  Rng rng{};
  std::uint64_t mixer = seed;
  for (std::uint64_t &word : rng.state) {
    word = splitmix64_next(&mixer);
  }
  return rng;
}

/// The next 64 bits, advancing the stream. Null yields zero rather than
/// dereferencing: a caller that lost its stream gets a constant it can
/// notice, not a crash.
constexpr std::uint64_t rng_next_u64(Rng *rng) noexcept {
  if (rng == nullptr) {
    return 0U;
  }
  // xoshiro256**: the scrambler is (s1 * 5) rotated left 7 and times 9.
  const std::uint64_t s1 = rng->state[1];
  const std::uint64_t scrambled = s1 * 5ULL;
  const std::uint64_t rotated = (scrambled << 7U) | (scrambled >> 57U);
  const std::uint64_t result = rotated * 9ULL;

  const std::uint64_t t = s1 << 17U;
  rng->state[2] ^= rng->state[0];
  rng->state[3] ^= s1;
  rng->state[1] ^= rng->state[2];
  rng->state[0] ^= rng->state[3];
  rng->state[2] ^= t;
  rng->state[3] = (rng->state[3] << 45U) | (rng->state[3] >> 19U);
  return result;
}

/// The next 32 bits, taken from the high half: xoshiro's low bits are the
/// weakest, and a caller asking for 32 should not be handed them.
constexpr std::uint32_t rng_next_u32(Rng *rng) noexcept {
  return static_cast<std::uint32_t>(rng_next_u64(rng) >> 32U);
}

/// A float in [0, 1). Built by scaling 24 random bits by 2^-24, which is
/// exact in binary32, so the result is identical on every platform and
/// needs no library call. The interval excludes one deliberately: callers
/// scale it into a range, and a returned one would land a value one past
/// the end.
constexpr float rng_next_float(Rng *rng) noexcept {
  constexpr float kScale = 1.0F / 16777216.0F; // 2^-24, exact
  return static_cast<float>(rng_next_u64(rng) >> 40U) * kScale;
}

/// A double in [0, 1), from 53 bits scaled by 2^-53 on the same terms.
constexpr double rng_next_double(Rng *rng) noexcept {
  constexpr double kScale = 1.0 / 9007199254740992.0; // 2^-53, exact
  return static_cast<double>(rng_next_u64(rng) >> 11U) * kScale;
}

/// A uniform integer in [minimum, maximum], both ends included. An
/// inverted range is treated as the single value `minimum` rather than
/// refused, because the caller is usually passing computed bounds and a
/// silently empty range has no honest answer.
///
/// Uniform by rejection rather than by modulo: the modulo of a power-of-two
/// generator over a range that does not divide it favours the low values,
/// which shows up as bias in exactly the places games use it (loot, spawn
/// choice). The loop's expected iteration count is below two and the
/// probability of many is vanishing, so it is bounded in practice while
/// being exactly uniform, which the alternative is not.
constexpr std::int64_t rng_range(Rng *rng, std::int64_t minimum,
                                 std::int64_t maximum) noexcept {
  if (maximum <= minimum) {
    return minimum;
  }
  // The span fits an unsigned 64-bit value even when the bounds span the
  // whole signed range, which a signed subtraction would overflow.
  const std::uint64_t span = static_cast<std::uint64_t>(maximum) -
                             static_cast<std::uint64_t>(minimum);
  if (span == UINT64_MAX) {
    return static_cast<std::int64_t>(rng_next_u64(rng));
  }
  const std::uint64_t count = span + 1U;
  // Every draw at or above this limit would land in a partial final
  // block, so those are the draws to reject.
  const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % count) - 1U;
  std::uint64_t draw = rng_next_u64(rng);
  while (draw > limit) {
    draw = rng_next_u64(rng);
  }
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(minimum) +
                                   (draw % count));
}

/// Folds a stream's state into a running hash, so a state hash covering
/// the simulation covers where its randomness is. Takes the stream by
/// value: hashing must not advance it.
constexpr std::uint64_t rng_hash_append(std::uint64_t hash,
                                        const Rng &rng) noexcept {
  for (const std::uint64_t word : rng.state) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      hash = fnv1a_64_append(hash, static_cast<std::uint8_t>(word >> shift));
    }
  }
  return hash;
}

} // namespace engine::core
