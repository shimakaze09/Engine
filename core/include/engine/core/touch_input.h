// Declares touch input types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

// ---------------------------------------------------------------------------
// Touch Phase
// ---------------------------------------------------------------------------

enum class TouchPhase : std::uint8_t {
  Began,
  Moved,
  Stationary,
  Ended,
  Cancelled
};

// ---------------------------------------------------------------------------
// Touch Event
// ---------------------------------------------------------------------------

struct TouchEvent final {
  std::int64_t touchId = 0;
  float x = 0.0F;
  float y = 0.0F;
  float pressure = 0.0F;
  TouchPhase phase = TouchPhase::Began;
};

// ---------------------------------------------------------------------------
// Gesture Types
// ---------------------------------------------------------------------------

enum class GestureType : std::uint8_t { Tap, Swipe, Pinch, Rotate };

/// Enumerates swipe direction values used by the engine.
enum class SwipeDirection : std::uint8_t { Left, Right, Up, Down };

/// Recognized gesture payload (tap/swipe/pinch/rotate).
struct GestureEvent final {
  GestureType type = GestureType::Tap;

  // Tap data.
  float tapX = 0.0F;
  float tapY = 0.0F;
  std::uint32_t tapCount = 1U;

  // Swipe data.
  SwipeDirection swipeDir = SwipeDirection::Right;
  float swipeVelocity = 0.0F;

  // Pinch data.
  float pinchScale = 1.0F;
  float pinchCenterX = 0.0F;
  float pinchCenterY = 0.0F;

  // Rotate data.
  float rotationRadians = 0.0F;
};

// ---------------------------------------------------------------------------
// Touch Callbacks
// ---------------------------------------------------------------------------

using TouchCallback = void (*)(const TouchEvent &event,
                               void *userData) noexcept;
using GestureCallback = void (*)(const GestureEvent &event,
                                 void *userData) noexcept;

// ---------------------------------------------------------------------------
// Touch Input API
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxActiveTouches = 10U;
inline constexpr std::size_t kMaxTouchCallbacks = 16U;
inline constexpr std::size_t kMaxGestureCallbacks = 16U;

/// Initializes the owning system for touch input.
bool initialize_touch_input() noexcept;
/// Shuts down the owning system for touch input.
void shutdown_touch_input() noexcept;

/// Converts touch process event into the target representation.
void touch_process_event(const void *nativeEvent) noexcept;
/// Converts touch begin frame into the target representation.
void touch_begin_frame() noexcept;
/// Converts touch end frame into the target representation.
void touch_end_frame() noexcept;

// Query active touches.
std::uint32_t active_touch_count() noexcept;
/// Copies the active touch at index; false out of range.
bool get_active_touch(std::uint32_t index, TouchEvent *outTouch) noexcept;

// Callback registration.
bool register_touch_callback(TouchCallback cb,
                             void *userData = nullptr) noexcept;
/// Removes a touch callback registration; false when not found.
bool unregister_touch_callback(TouchCallback cb,
                               void *userData = nullptr) noexcept;

/// Registers a callback for one gesture type; false when full.
bool register_gesture_callback(GestureType type, GestureCallback cb,
                               void *userData = nullptr) noexcept;
/// Removes a gesture callback registration; false when not found.
bool unregister_gesture_callback(GestureType type, GestureCallback cb,
                                 void *userData = nullptr) noexcept;

// Touch-to-mouse emulation (C3d).
void set_touch_mouse_emulation(bool enabled) noexcept;
/// Returns whether is touch mouse emulation enabled.
bool is_touch_mouse_emulation_enabled() noexcept;

} // namespace engine::core
