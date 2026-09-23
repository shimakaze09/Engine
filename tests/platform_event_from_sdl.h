// Test-only bridge from a hand-built SDL_Event to the engine's
// PlatformEvent. Suites that drive input through SDL events keep doing so,
// and every event goes through the platform's production translation
// (platform_translate_native_event) rather than a copy of it, so the
// translation is exercised by every call instead of bypassed.

#pragma once

#include "engine/core/platform_event.h"

#include <SDL3/SDL.h>

namespace engine::tests {

/// The PlatformEvent the platform would deliver for `event`. Its native
/// pointer refers to `event`, which must outlive the use of the result.
inline core::PlatformEvent from_sdl(const SDL_Event &event) noexcept {
  core::PlatformEvent out{};
  static_cast<void>(core::platform_translate_native_event(&event, &out));
  return out;
}

} // namespace engine::tests
