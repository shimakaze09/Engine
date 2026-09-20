// Pins the deterministic scalar set (engine/math/scalar.h): each function
// tracks the C library within a few ulp across the simulation's ranges,
// holds its identities, and produces exact promised bit patterns on a
// fixed input table. The bit patterns are the cross-platform contract:
// every lane must print the same words, which the libm versions they
// replace do not (glibc, UCRT and libSystem round differently).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../test_harness.h"
#include "engine/math/scalar.h"

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

std::uint32_t bits(float value) noexcept {
  std::uint32_t out = 0U;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

/// The series truncation and the reduction each cost about one ulp and
/// libm's own answer is within one ulp of the true value, so 8 ulp of
/// relative slack (about 1e-6) plus 1e-7 absolute near zero separates
/// a correct implementation from a wrong quadrant, sign or reduction.
bool close(float actual, float expected) noexcept {
  const float scale = std::fabs(expected);
  return std::fabs(actual - expected) <= (1.0e-7F + (1.0e-6F * scale));
}

void test_tracks_libm() noexcept {
  bool sinOk = true;
  bool cosOk = true;
  bool identityOk = true;
  for (int i = -4000; i <= 4000; ++i) {
    const float x = static_cast<float>(i) * 0.005F;
    const float s = engine::math::det_sin(x);
    const float c = engine::math::det_cos(x);
    sinOk = sinOk && close(s, std::sin(x));
    cosOk = cosOk && close(c, std::cos(x));
    identityOk = identityOk && (std::fabs(((s * s) + (c * c)) - 1.0F) <= 4.0e-7F);
  }
  check(sinOk, "det_sin tracks sin on [-20, 20]");
  check(cosOk, "det_cos tracks cos on [-20, 20]");
  check(identityOk, "sin^2 + cos^2 stays 1 within 4e-7");

  bool expOk = true;
  bool expIdentityOk = true;
  for (int i = -800; i <= 800; ++i) {
    const float x = static_cast<float>(i) * 0.1F;
    expOk = expOk && close(engine::math::det_exp(x), std::exp(x));
    if (i <= 0) {
      const float product =
          engine::math::det_exp(x) * engine::math::det_exp(-x);
      expIdentityOk = expIdentityOk && (std::fabs(product - 1.0F) <= 1.0e-6F);
    }
  }
  check(expOk, "det_exp tracks exp on [-80, 80]");
  check(expIdentityOk, "exp(x) * exp(-x) stays 1 within 1e-6");
  check(engine::math::det_exp(0.0F) == 1.0F, "exp(0) is exactly 1");
  check(engine::math::det_exp(200.0F) == INFINITY, "exp overflows to inf");
  check(engine::math::det_exp(-200.0F) == 0.0F, "exp underflows to zero");

  bool atanOk = true;
  for (int i = -2000; i <= 2000; ++i) {
    const float x = static_cast<float>(i) * 0.01F;
    atanOk = atanOk && close(engine::math::det_atan(x), std::atan(x));
    const float big = static_cast<float>(i) * 100.0F;
    atanOk = atanOk && close(engine::math::det_atan(big), std::atan(big));
  }
  check(atanOk, "det_atan tracks atan on [-20, 20] and [-2e5, 2e5]");

  bool atan2Ok = true;
  for (int yi = -40; yi <= 40; ++yi) {
    for (int xi = -40; xi <= 40; ++xi) {
      const float y = static_cast<float>(yi) * 0.25F;
      const float x = static_cast<float>(xi) * 0.25F;
      atan2Ok = atan2Ok && close(engine::math::det_atan2(y, x), std::atan2(y, x));
    }
  }
  check(atan2Ok, "det_atan2 tracks atan2 in every quadrant and on the axes");
  // The C standard's signed-zero results, pinned as exact bits rather than
  // compared with libm: atan2(+0, -0) = +pi, atan2(-0, -0) = -pi,
  // atan2(+0, +0) = +0, atan2(-0, +0) = -0. libm itself is not the same
  // at these points on every platform, which is the reason this set
  // exists.
  const bool signedZeroOk =
      (bits(engine::math::det_atan2(0.0F, -0.0F)) == bits(engine::math::kDetPi)) &&
      (bits(engine::math::det_atan2(-0.0F, -0.0F)) == bits(-engine::math::kDetPi)) &&
      (bits(engine::math::det_atan2(0.0F, 0.0F)) == 0x00000000U) &&
      (bits(engine::math::det_atan2(-0.0F, 0.0F)) == 0x80000000U);
  if (!signedZeroOk) {
    std::printf("  atan2(+0,-0) %08X atan2(-0,-0) %08X atan2(+0,+0) %08X "
                "atan2(-0,+0) %08X\n",
                bits(engine::math::det_atan2(0.0F, -0.0F)),
                bits(engine::math::det_atan2(-0.0F, -0.0F)),
                bits(engine::math::det_atan2(0.0F, 0.0F)),
                bits(engine::math::det_atan2(-0.0F, 0.0F)));
  }
  check(signedZeroOk, "det_atan2 keeps the signed-zero conventions");

  bool asinOk = true;
  bool acosOk = true;
  for (int i = -1000; i <= 1000; ++i) {
    const float x = static_cast<float>(i) * 0.001F;
    asinOk = asinOk && close(engine::math::det_asin(x), std::asin(x));
    acosOk = acosOk && close(engine::math::det_acos(x), std::acos(x));
  }
  check(asinOk, "det_asin tracks asin on [-1, 1]");
  check(acosOk, "det_acos tracks acos on [-1, 1]");
  check(engine::math::det_acos(1.0F) == 0.0F, "acos(1) is exactly 0");
  check(engine::math::det_acos(-1.0F) == engine::math::kDetPi,
        "acos(-1) is exactly pi");
}

/// One promised word per function and input: the bits every platform
/// must reproduce. Recorded on Linux x86-64 (gcc, strict floats).
struct Pinned final {
  float input;
  std::uint32_t sinBits;
  std::uint32_t cosBits;
  std::uint32_t expBits;
  std::uint32_t atanBits;
};

constexpr Pinned kPinned[] = {
    {0.0F, 0x00000000U, 0x3F800000U, 0x3F800000U, 0x00000000U},
    {0.005F, 0x3BA3D6DDU, 0x3F7FFF2EU, 0x3F80A440U, 0x3BA3D6B0U},
    {-0.0066666668F, 0xBBDA73A4U, 0x3F7FFE8BU, 0x3F7E4C8CU, 0xBBDA733AU},
    {0.35F, 0x3EAF904DU, 0x3F707ABBU, 0x3FB5A402U, 0x3EAC60A3U},
    {1.0F, 0x3F576AA4U, 0x3F0A5140U, 0x402DF854U, 0x3F490FDAU},
    {-2.5F, 0xBF193579U, 0xBF4D17BFU, 0x3DA81C2EU, 0xBF985B6CU},
    {7.0F, 0x3F283046U, 0x3F40FFBDU, 0x44891443U, 0x3FB6E62CU},
    {-12.0F, 0x3F095CD8U, 0x3F5806D0U, 0x36CE2A62U, 0xBFBE6B7CU},
};

void test_pinned_bits() noexcept {
  for (const Pinned &row : kPinned) {
    char label[96] = {};
    std::snprintf(label, sizeof(label), "pinned bits for input %g",
                  static_cast<double>(row.input));
    const bool ok = (bits(engine::math::det_sin(row.input)) == row.sinBits) &&
                    (bits(engine::math::det_cos(row.input)) == row.cosBits) &&
                    (bits(engine::math::det_exp(row.input)) == row.expBits) &&
                    (bits(engine::math::det_atan(row.input)) == row.atanBits);
    if (!ok) {
      std::printf("  input %g: sin %08X cos %08X exp %08X atan %08X\n",
                  static_cast<double>(row.input),
                  bits(engine::math::det_sin(row.input)),
                  bits(engine::math::det_cos(row.input)),
                  bits(engine::math::det_exp(row.input)),
                  bits(engine::math::det_atan(row.input)));
    }
    check(ok, label);
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_tracks_libm();
  test_pinned_bits();
  return g_tests.finish("scalar determinism tests");
}
