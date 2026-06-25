// Implements renderer setting parsing and normalization helpers.

#include "engine/renderer/command_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "engine/math/vec3.h"

namespace engine::renderer {

namespace {

bool string_equals(const char *lhs, const char *rhs) noexcept {
  return (lhs != nullptr) && (rhs != nullptr) && (std::strcmp(lhs, rhs) == 0);
}

void skip_fog_color_separators(const char *&cursor) noexcept {
  while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\n') ||
         (*cursor == '\r') || (*cursor == ',')) {
    ++cursor;
  }
}

bool parse_fog_color_component(const char *&cursor, float *valueOut) noexcept {
  if ((cursor == nullptr) || (valueOut == nullptr)) {
    return false;
  }

  skip_fog_color_separators(cursor);
  char *end = nullptr;
  const float value = std::strtof(cursor, &end);
  if ((end == cursor) || !std::isfinite(value)) {
    return false;
  }
  *valueOut = value;
  cursor = end;
  return true;
}

std::uint32_t clamp_u32_value(std::uint32_t value, std::uint32_t minValue,
                              std::uint32_t maxValue) noexcept {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

std::uint32_t previous_power_of_two_u32(std::uint32_t value) noexcept {
  std::uint32_t result = 1U;
  while ((result <= (value / 2U)) && (result < 4096U)) {
    result *= 2U;
  }
  return result;
}

std::uint32_t max_cubemap_mip_levels_u32(std::uint32_t faceSize) noexcept {
  std::uint32_t levels = 1U;
  while (faceSize > 1U) {
    faceSize /= 2U;
    ++levels;
  }
  return levels;
}

} // namespace

/// Parses text into the engine representation for distance fog mode.
DistanceFogMode parse_distance_fog_mode(const char *mode) noexcept {
  if (string_equals(mode, "linear") || string_equals(mode, "1")) {
    return DistanceFogMode::Linear;
  }
  if (string_equals(mode, "exp") || string_equals(mode, "2") ||
      string_equals(mode, "exponential")) {
    return DistanceFogMode::Exp;
  }
  if (string_equals(mode, "exp2") || string_equals(mode, "3") ||
      string_equals(mode, "exponential2")) {
    return DistanceFogMode::Exp2;
  }
  return DistanceFogMode::Off;
}

/// Parses text into the engine representation for distance fog color.
bool parse_distance_fog_color(const char *value,
                              math::Vec3 *colorOut) noexcept {
  if ((value == nullptr) || (colorOut == nullptr)) {
    return false;
  }

  const char *cursor = value;
  math::Vec3 parsed{};
  if (!parse_fog_color_component(cursor, &parsed.x) ||
      !parse_fog_color_component(cursor, &parsed.y) ||
      !parse_fog_color_component(cursor, &parsed.z)) {
    return false;
  }

  skip_fog_color_separators(cursor);
  if (*cursor != '\0') {
    return false;
  }

  *colorOut = math::clamp(parsed, 0.0F, 1.0F);
  return true;
}

/// Clamps and fills settings into a safe runtime range for distance fog settings.
DistanceFogSettings normalize_distance_fog_settings(
    const DistanceFogSettings &settings) noexcept {
  DistanceFogSettings normalized{};
  switch (settings.mode) {
  case DistanceFogMode::Linear:
  case DistanceFogMode::Exp:
  case DistanceFogMode::Exp2:
    normalized.mode = settings.mode;
    break;
  case DistanceFogMode::Off:
  default:
    normalized.mode = DistanceFogMode::Off;
    break;
  }

  normalized.start =
      std::isfinite(settings.start) ? std::max(0.0F, settings.start) : 25.0F;
  const float requestedEnd =
      std::isfinite(settings.end) ? settings.end : 150.0F;
  normalized.end = std::max(normalized.start + 0.001F, requestedEnd);
  normalized.density = std::isfinite(settings.density)
                           ? std::max(0.0F, settings.density)
                           : 0.01F;
  normalized.color = ((std::isfinite(settings.color.x) &&
                       std::isfinite(settings.color.y) &&
                       std::isfinite(settings.color.z))
                          ? math::clamp(settings.color, 0.0F, 1.0F)
                          : math::Vec3(0.55F, 0.65F, 0.75F));
  return normalized;
}

/// Clamps and fills settings into a safe runtime range for height fog settings.
HeightFogSettings normalize_height_fog_settings(
    const HeightFogSettings &settings) noexcept {
  HeightFogSettings normalized{};
  normalized.enabled = settings.enabled;
  normalized.baseHeight =
      std::isfinite(settings.baseHeight) ? settings.baseHeight : 0.0F;
  normalized.density = std::isfinite(settings.density)
                           ? std::clamp(settings.density, 0.0F, 1.0F)
                           : 0.015F;
  normalized.falloff = std::isfinite(settings.falloff)
                           ? std::clamp(settings.falloff, 0.001F, 4.0F)
                           : 0.08F;
  normalized.stepCount = std::clamp(settings.stepCount, 1, 64);
  if (normalized.density <= 0.0F) {
    normalized.enabled = false;
  }
  return normalized;
}

/// Clamps and fills settings into a safe runtime range for reflection probe bake settings.
ReflectionProbeBakeSettings normalize_reflection_probe_bake_settings(
    const ReflectionProbeBakeSettings &settings) noexcept {
  ReflectionProbeBakeSettings normalized{};
  normalized.prefilteredFaceSize = previous_power_of_two_u32(
      clamp_u32_value(settings.prefilteredFaceSize, 16U, 1024U));
  const std::uint32_t maxMips =
      max_cubemap_mip_levels_u32(normalized.prefilteredFaceSize);
  normalized.prefilteredMipLevels =
      clamp_u32_value(settings.prefilteredMipLevels, 1U, maxMips);
  normalized.irradianceFaceSize = previous_power_of_two_u32(
      clamp_u32_value(settings.irradianceFaceSize, 8U, 256U));
  normalized.brdfLutSize = previous_power_of_two_u32(
      clamp_u32_value(settings.brdfLutSize, 64U, 1024U));
  return normalized;
}

} // namespace engine::renderer
