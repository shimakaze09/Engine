// The promised bit patterns of the deterministic scalar set on a fixed
// input table: the cross-platform contract every lane must reproduce,
// shared by the C++ suite that evaluates the functions directly and the
// Lua suite that evaluates them through the script VM's math table.
// Recorded on Linux x86-64 (gcc, strict floats). log is pinned only
// where the input is positive; its NaN for a negative input is not a
// promised word.

#pragma once

#include <cstdint>

namespace engine::tests {

/// One promised word per function and input.
struct PinnedScalarWords final {
  float input;
  std::uint32_t sinBits;
  std::uint32_t cosBits;
  std::uint32_t expBits;
  std::uint32_t atanBits;
  std::uint32_t tanBits;
  /// Meaningful only when input > 0.
  std::uint32_t logBits;
};

inline constexpr PinnedScalarWords kPinnedScalarWords[] = {
    {0.0F, 0x00000000U, 0x3F800000U, 0x3F800000U, 0x00000000U, 0x00000000U,
     0U},
    {0.005F, 0x3BA3D6DDU, 0x3F7FFF2EU, 0x3F80A440U, 0x3BA3D6B0U, 0x3BA3D763U,
     0xC0A98BD1U},
    {-0.0066666668F, 0xBBDA73A4U, 0x3F7FFE8BU, 0x3F7E4C8CU, 0xBBDA733AU,
     0xBBDA74E2U, 0U},
    {0.35F, 0x3EAF904DU, 0x3F707ABBU, 0x3FB5A402U, 0x3EAC60A3U, 0x3EBAE504U,
     0xBF866092U},
    {1.0F, 0x3F576AA4U, 0x3F0A5140U, 0x402DF854U, 0x3F490FDAU, 0x3FC75923U,
     0x00000000U},
    {-2.5F, 0xBF193579U, 0xBF4D17BFU, 0x3DA81C2EU, 0xBF985B6CU, 0x3F3F3CDCU,
     0U},
    {7.0F, 0x3F283046U, 0x3F40FFBDU, 0x44891443U, 0x3FB6E62CU, 0x3F5F1737U,
     0x3FF91395U},
    {-12.0F, 0x3F095CD8U, 0x3F5806D0U, 0x36CE2A62U, 0xBFBE6B7CU, 0x3F22C7B8U,
     0U},
};

inline constexpr int kPinnedScalarWordCount =
    static_cast<int>(sizeof(kPinnedScalarWords) / sizeof(kPinnedScalarWords[0]));

} // namespace engine::tests
