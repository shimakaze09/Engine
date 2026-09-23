// The platform's events in the engine's own vocabulary. The platform
// layer translates each native event into one of these, and everything
// above it -- input, touch, the frame pipeline, the editor -- reads this
// instead of SDL's event union, so none of them has to know SDL exists.
//
// Scancodes, mouse buttons and gamepad buttons and axes carry the values
// input.h names (kKey_*, 0 for the left mouse button, kGamepadButton_*,
// kGamepadAxis_*); the platform owns converting to them. Positions are in
// window pixels; finger positions are normalized to [0, 1] across the
// touch surface, as the device reports them.

#pragma once

#include <cstdint>

namespace engine::core {

enum class PlatformEventKind : std::uint8_t {
  /// A native event with no engine meaning. Still delivered, because the
  /// editor's ImGui backend consumes native events the engine does not
  /// model (pointer enter and leave, clipboard, IME).
  Other = 0,
  Quit,
  KeyDown,
  KeyUp,
  /// Text entry and composition. No payload: only the ImGui backend reads
  /// the text, through the native event. Carried so a caller can tell a
  /// keyboard-driven event from the rest.
  TextInput,
  TextEditing,
  MouseMove,
  MouseButtonDown,
  MouseButtonUp,
  MouseWheel,
  FingerDown,
  FingerMove,
  FingerUp,
  /// The OS took the touch away -- palm rejection, a system gesture.
  FingerCanceled,
  GamepadAdded,
  GamepadRemoved,
  GamepadButtonDown,
  GamepadButtonUp,
  GamepadAxis,
  WindowFocusGained,
  WindowFocusLost,
  WindowResized,
};

struct PlatformEvent final {
  PlatformEventKind kind = PlatformEventKind::Other;
  /// Nanoseconds on the platform's monotonic clock; 0 when unknown.
  std::uint64_t timestampNs = 0U;
  /// The gamepad instance an event belongs to, for the gamepad kinds.
  std::uint32_t deviceId = 0U;

  /// KeyDown / KeyUp.
  int scancode = 0;
  /// Key auto-repeat from the OS rather than a new press.
  bool repeat = false;

  /// Mouse kinds: cursor position, and for MouseMove the motion since the
  /// previous one.
  float x = 0.0F;
  float y = 0.0F;
  float deltaX = 0.0F;
  float deltaY = 0.0F;
  /// MouseButtonDown / MouseButtonUp: 0 left, 1 middle, 2 right, ...
  int mouseButton = 0;
  /// MouseWheel: notches, fractional on a precise trackpad.
  float wheelY = 0.0F;

  /// Finger kinds. Position and motion are normalized, as above.
  std::int64_t fingerId = 0;
  float fingerX = 0.0F;
  float fingerY = 0.0F;
  float fingerDeltaX = 0.0F;
  float fingerDeltaY = 0.0F;
  float pressure = 0.0F;

  /// GamepadButtonDown / GamepadButtonUp.
  int gamepadButton = 0;
  /// GamepadAxis: which axis, and its raw value in [-32768, 32767].
  int gamepadAxis = 0;
  std::int16_t axisValue = 0;

  /// WindowResized: the new size in window units.
  int width = 0;
  int height = 0;

  /// The native event this was translated from, or null for an event the
  /// engine made itself. Valid only until the next poll. Its one reader
  /// is the editor's ImGui SDL3 backend, which is written against SDL;
  /// nothing else may cast it.
  const void *native = nullptr;
};

/// Takes the next pending platform event. False when the queue is empty.
/// Main thread only, like the native event queue under it.
bool platform_poll_event(PlatformEvent *outEvent) noexcept;

/// Translates one native event exactly as platform_poll_event does, for a
/// caller that already holds one -- a test that builds native events and
/// wants them to go through the production translation rather than a copy
/// of it. `native` in the result points at `nativeEvent`. False, with
/// `outEvent` untouched, for a null argument.
bool platform_translate_native_event(const void *nativeEvent,
                                     PlatformEvent *outEvent) noexcept;

} // namespace engine::core
