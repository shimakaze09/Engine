// Pins the engine random stream's contract: the sequence a seed produces,
// that it is reproducible and copyable, that floats stay inside [0, 1),
// and that a range is uniform over inclusive bounds rather than biased
// toward its low end.
//
// The golden vector below is the important part. A random stream whose
// output changes between compilers, platforms or engine versions silently
// breaks every seeded replay and every determinism comparison that
// covers it, and nothing else in the suite would notice. So the first
// draws for a fixed seed are written down as exact integers: if they ever
// change, the change was either deliberate and needs a new behaviour
// version, or it is a defect.

#include "engine/core/rng.h"

#include <cstdio>
#include <cstdint>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

using engine::core::Rng;
using engine::core::rng_from_seed;
using engine::core::rng_hash_append;
using engine::core::rng_next_double;
using engine::core::rng_next_float;
using engine::core::rng_next_u32;
using engine::core::rng_next_u64;
using engine::core::rng_range;

/// The first eight draws from seed 1, as this implementation produces
/// them. Exact by contract, not by tolerance.
constexpr std::uint64_t kGoldenSeedOne[8] = {
    12966619160104079557ULL, 9600361134598540522ULL,
    10590380919521690900ULL, 7218738570589545383ULL,
    12860671823995680371ULL, 2648436617965840162ULL,
    1310552918490157286ULL,  7031611932980406429ULL};

void check_golden_vector() noexcept {
  Rng rng = rng_from_seed(1U);
  bool matched = true;
  for (const std::uint64_t expected : kGoldenSeedOne) {
    const std::uint64_t drawn = rng_next_u64(&rng);
    if (drawn != expected) {
      matched = false;
      std::fprintf(stderr, "  golden mismatch: expected %llu, drew %llu\n",
                   static_cast<unsigned long long>(expected),
                   static_cast<unsigned long long>(drawn));
    }
  }
  check(matched, "seed 1 produces the sequence this engine version promises");
}

void check_reproducible_and_independent() noexcept {
  Rng first = rng_from_seed(12345U);
  Rng second = rng_from_seed(12345U);
  bool same = true;
  for (int i = 0; i < 64; ++i) {
    same = same && (rng_next_u64(&first) == rng_next_u64(&second));
  }
  check(same, "one seed gives one sequence");

  Rng other = rng_from_seed(12346U);
  Rng baseline = rng_from_seed(12345U);
  check(rng_next_u64(&other) != rng_next_u64(&baseline),
        "adjacent seeds do not give the same first draw");

  // A copy is a fork: advancing one must not touch the other, which is
  // what makes a stream restorable from a snapshot.
  Rng source = rng_from_seed(99U);
  static_cast<void>(rng_next_u64(&source));
  Rng fork = source;
  const std::uint64_t fromSource = rng_next_u64(&source);
  const std::uint64_t fromFork = rng_next_u64(&fork);
  check(fromSource == fromFork, "a copied stream continues from the same "
                                "place");
  static_cast<void>(rng_next_u64(&fork));
  check(source.state[0] != fork.state[0] || source.state[1] != fork.state[1],
        "advancing a copy leaves the original alone");

  // Zero is a seed like any other: splitmix64 has no zero fixed point, so
  // the all-zero state xoshiro cannot escape is unreachable.
  Rng zero = rng_from_seed(0U);
  check((zero.state[0] | zero.state[1] | zero.state[2] | zero.state[3]) != 0U,
        "seed zero still fills the state");
  check(rng_next_u64(&zero) != 0U, "seed zero produces a usable stream");
}

void check_null_is_survivable() noexcept {
  check(rng_next_u64(nullptr) == 0U, "a null stream yields zero");
  check(rng_next_u32(nullptr) == 0U, "a null stream yields zero for 32 bits");
  check(rng_next_float(nullptr) == 0.0F, "a null stream yields zero float");
}

void check_float_interval() noexcept {
  Rng rng = rng_from_seed(7U);
  bool inside = true;
  bool sawLowHalf = false;
  bool sawHighHalf = false;
  for (int i = 0; i < 4096; ++i) {
    const float value = rng_next_float(&rng);
    inside = inside && (value >= 0.0F) && (value < 1.0F);
    sawLowHalf = sawLowHalf || (value < 0.5F);
    sawHighHalf = sawHighHalf || (value >= 0.5F);
  }
  check(inside, "every float lands in [0, 1)");
  check(sawLowHalf && sawHighHalf, "floats reach both halves of the interval");

  Rng doubles = rng_from_seed(8U);
  bool doublesInside = true;
  for (int i = 0; i < 4096; ++i) {
    const double value = rng_next_double(&doubles);
    doublesInside = doublesInside && (value >= 0.0) && (value < 1.0);
  }
  check(doublesInside, "every double lands in [0, 1)");
}

void check_range_bounds() noexcept {
  Rng rng = rng_from_seed(4242U);

  // Both ends are reachable and nothing escapes them. A range of six is
  // the die roll every game writes first.
  bool withinBounds = true;
  int counts[6] = {};
  for (int i = 0; i < 60000; ++i) {
    const std::int64_t value = rng_range(&rng, 1, 6);
    if ((value < 1) || (value > 6)) {
      withinBounds = false;
      continue;
    }
    ++counts[value - 1];
  }
  check(withinBounds, "a range never leaves its inclusive bounds");
  bool everyValueSeen = true;
  for (const int count : counts) {
    everyValueSeen = everyValueSeen && (count > 0);
  }
  check(everyValueSeen, "both ends of a range are reachable");

  // Uniformity, loosely but meaningfully: a modulo of a 64-bit generator
  // over six would bias the low faces by a part in 3e18 — far too small
  // to catch here — but a botched rejection bound biases them grossly, so
  // this is a guard against the error that is actually plausible. The
  // bound is a wide multiple of the expected 2.7-sigma spread at 10000
  // per face, so it fails on a real skew, not on a run of luck.
  int lowest = counts[0];
  int highest = counts[0];
  for (const int count : counts) {
    lowest = (count < lowest) ? count : lowest;
    highest = (count > highest) ? count : highest;
  }
  check((highest - lowest) < 1200,
        "a range is not grossly skewed toward either end");

  // Degenerate and extreme bounds.
  check(rng_range(&rng, 5, 5) == 5, "an empty range is its single value");
  check(rng_range(&rng, 9, 3) == 9, "an inverted range is its low bound");
  bool binaryBoth[2] = {false, false};
  for (int i = 0; i < 64; ++i) {
    const std::int64_t value = rng_range(&rng, 0, 1);
    if ((value == 0) || (value == 1)) {
      binaryBoth[static_cast<std::size_t>(value)] = true;
    }
  }
  check(binaryBoth[0] && binaryBoth[1], "a two-value range reaches both");

  // The whole signed range: the span computation must not overflow, and
  // the full-span shortcut must still draw.
  bool spanned = true;
  for (int i = 0; i < 64; ++i) {
    const std::int64_t value =
        rng_range(&rng, INT64_MIN, INT64_MAX);
    spanned = spanned && (value >= INT64_MIN) && (value <= INT64_MAX);
  }
  check(spanned, "the full signed range draws without overflowing");

  // A range whose count is a power of two exercises the rejection bound's
  // exact-division case, where nothing should ever be rejected.
  bool powerOfTwoInside = true;
  for (int i = 0; i < 1024; ++i) {
    const std::int64_t value = rng_range(&rng, 0, 255);
    powerOfTwoInside =
        powerOfTwoInside && (value >= 0) && (value <= 255);
  }
  check(powerOfTwoInside, "a power-of-two count stays inside its bounds");
}

void check_hash_does_not_advance() noexcept {
  Rng rng = rng_from_seed(31337U);
  static_cast<void>(rng_next_u64(&rng));
  const Rng before = rng;
  const std::uint64_t first = rng_hash_append(0U, rng);
  const std::uint64_t second = rng_hash_append(0U, rng);
  check(first == second, "hashing a stream twice gives one answer");
  check((rng.state[0] == before.state[0]) &&
            (rng.state[1] == before.state[1]) &&
            (rng.state[2] == before.state[2]) &&
            (rng.state[3] == before.state[3]),
        "hashing a stream does not advance it");

  Rng advanced = rng;
  static_cast<void>(rng_next_u64(&advanced));
  check(rng_hash_append(0U, advanced) != first,
        "a stream that moved hashes differently");
}

} // namespace

/// Runs this executable or test program.
int main() {
  check_golden_vector();
  check_reproducible_and_independent();
  check_null_is_survivable();
  check_float_interval();
  check_range_bounds();
  check_hash_does_not_advance();

  if (g_failures != 0) {
    std::fprintf(stderr, "rng_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("rng_test: the random stream's contract holds\n");
  return 0;
}
