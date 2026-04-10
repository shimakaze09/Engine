#pragma once

#include <cstddef>
#include <cstdint>

// Debug Draw API.
// Submit shapes (lines, spheres, text) with a frame lifetime.  The renderer or
// an overlay system calls debug_draw_tick() once per frame to decrement
// lifetimes and expire old entries, then reads geometry via the accessor
// functions.  All functions are noexcept.
//
// This module uses its own DebugVec3 / DebugColor types so it can reside in
// core without depending on the math module.

namespace engine::core {

struct DebugVec3 final {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

struct DebugColor final {
  float r = 1.0F;
  float g = 1.0F;
  float b = 1.0F;
  float a = 1.0F;
};

struct DebugLine final {
  DebugVec3 from{};
  DebugVec3 to{};
  DebugColor color{};
  std::uint32_t lifeFrames = 1U;
};

struct DebugSphere final {
  DebugVec3 center{};
  float radius = 1.0F;
  DebugColor color{};
  std::uint32_t lifeFrames = 1U;
};

struct DebugText final {
  DebugVec3 position{};
  char text[128] = {};
  DebugColor color{};
  std::uint32_t lifeFrames = 1U;
};

bool initialize_debug_draw() noexcept;
void shutdown_debug_draw() noexcept;

// Submit draw commands.  lifeFrames == 1 means visible for the current frame
// only; lifetime is decremented by debug_draw_tick() at the end of each frame.
void debug_draw_line(DebugVec3 from, DebugVec3 to, DebugColor color = {},
                     std::uint32_t lifeFrames = 1U) noexcept;
void debug_draw_sphere(DebugVec3 center, float radius, DebugColor color = {},
                       std::uint32_t lifeFrames = 1U) noexcept;
void debug_draw_text(DebugVec3 position, const char *text,
                     DebugColor color = {},
                     std::uint32_t lifeFrames = 1U) noexcept;

// Decrement lifetimes; remove entries that reach zero.  Call once per frame.
void debug_draw_tick() noexcept;

// Query current live entries.  Returns the number of entries written to `out`.
std::size_t debug_draw_get_lines(DebugLine *out,
                                 std::size_t maxEntries) noexcept;
std::size_t debug_draw_get_spheres(DebugSphere *out,
                                   std::size_t maxEntries) noexcept;
std::size_t debug_draw_get_texts(DebugText *out,
                                 std::size_t maxEntries) noexcept;

} // namespace engine::core
