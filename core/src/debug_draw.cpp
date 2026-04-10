#include "engine/core/debug_draw.h"

#include <array>
#include <cstring>

namespace engine::core {

namespace {

constexpr std::size_t kMaxLines = 1024U;
constexpr std::size_t kMaxSpheres = 512U;
constexpr std::size_t kMaxTexts = 256U;

struct LineStore final {
  std::array<DebugLine, kMaxLines> entries{};
  std::size_t count = 0U;
};

struct SphereStore final {
  std::array<DebugSphere, kMaxSpheres> entries{};
  std::size_t count = 0U;
};

struct TextStore final {
  std::array<DebugText, kMaxTexts> entries{};
  std::size_t count = 0U;
};

bool g_initialized = false;
LineStore g_lines{};
SphereStore g_spheres{};
TextStore g_texts{};

// Compact-remove entries with lifeFrames == 0 from the front T[] store.
template <typename T, std::size_t N>
void compact(std::array<T, N> &arr, std::size_t &count) noexcept {
  std::size_t write = 0U;
  for (std::size_t read = 0U; read < count; ++read) {
    if (arr[read].lifeFrames > 0U) {
      if (write != read) {
        arr[write] = arr[read];
      }
      ++write;
    }
  }
  count = write;
}

} // namespace

bool initialize_debug_draw() noexcept {
  if (g_initialized) {
    return true;
  }
  g_lines = {};
  g_spheres = {};
  g_texts = {};
  g_initialized = true;
  return true;
}

void shutdown_debug_draw() noexcept {
  g_lines = {};
  g_spheres = {};
  g_texts = {};
  g_initialized = false;
}

void debug_draw_line(DebugVec3 from, DebugVec3 to, DebugColor color,
                     std::uint32_t lifeFrames) noexcept {
  if (g_lines.count >= kMaxLines) {
    return;
  }
  DebugLine &l = g_lines.entries[g_lines.count++];
  l.from = from;
  l.to = to;
  l.color = color;
  l.lifeFrames = (lifeFrames > 0U) ? lifeFrames : 1U;
}

void debug_draw_sphere(DebugVec3 center, float radius, DebugColor color,
                       std::uint32_t lifeFrames) noexcept {
  if (g_spheres.count >= kMaxSpheres) {
    return;
  }
  DebugSphere &s = g_spheres.entries[g_spheres.count++];
  s.center = center;
  s.radius = radius;
  s.color = color;
  s.lifeFrames = (lifeFrames > 0U) ? lifeFrames : 1U;
}

void debug_draw_text(DebugVec3 position, const char *text, DebugColor color,
                     std::uint32_t lifeFrames) noexcept {
  if ((text == nullptr) || (g_texts.count >= kMaxTexts)) {
    return;
  }
  DebugText &t = g_texts.entries[g_texts.count++];
  t.position = position;
  std::snprintf(t.text, sizeof(t.text), "%s", text);
  t.color = color;
  t.lifeFrames = (lifeFrames > 0U) ? lifeFrames : 1U;
}

void debug_draw_tick() noexcept {
  // Decrement lifetimes
  for (std::size_t i = 0U; i < g_lines.count; ++i) {
    if (g_lines.entries[i].lifeFrames > 0U) {
      --g_lines.entries[i].lifeFrames;
    }
  }
  for (std::size_t i = 0U; i < g_spheres.count; ++i) {
    if (g_spheres.entries[i].lifeFrames > 0U) {
      --g_spheres.entries[i].lifeFrames;
    }
  }
  for (std::size_t i = 0U; i < g_texts.count; ++i) {
    if (g_texts.entries[i].lifeFrames > 0U) {
      --g_texts.entries[i].lifeFrames;
    }
  }
  // Remove expired
  compact(g_lines.entries, g_lines.count);
  compact(g_spheres.entries, g_spheres.count);
  compact(g_texts.entries, g_texts.count);
}

std::size_t debug_draw_get_lines(DebugLine *out,
                                 std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  const std::size_t n =
      (g_lines.count < maxEntries) ? g_lines.count : maxEntries;
  for (std::size_t i = 0U; i < n; ++i) {
    out[i] = g_lines.entries[i];
  }
  return n;
}

std::size_t debug_draw_get_spheres(DebugSphere *out,
                                   std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  const std::size_t n =
      (g_spheres.count < maxEntries) ? g_spheres.count : maxEntries;
  for (std::size_t i = 0U; i < n; ++i) {
    out[i] = g_spheres.entries[i];
  }
  return n;
}

std::size_t debug_draw_get_texts(DebugText *out,
                                 std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  const std::size_t n =
      (g_texts.count < maxEntries) ? g_texts.count : maxEntries;
  for (std::size_t i = 0U; i < n; ++i) {
    out[i] = g_texts.entries[i];
  }
  return n;
}

} // namespace engine::core
