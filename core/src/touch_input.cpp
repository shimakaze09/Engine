#include "engine/core/touch_input.h"
#include "engine/core/input.h"
#include "engine/core/logging.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&         \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include <array>
#include <cmath>
#include <cstring>

namespace engine::core {

namespace {

constexpr const char *kLogChannel = "TouchInput";

bool g_touchInitialized = false;
bool g_mouseEmulation = false;

// Active touch tracking.
struct ActiveTouch final {
  std::int64_t touchId = 0;
  float x = 0.0F;
  float y = 0.0F;
  float startX = 0.0F;
  float startY = 0.0F;
  float pressure = 0.0F;
  TouchPhase phase = TouchPhase::Ended;
  bool active = false;
};

std::array<ActiveTouch, kMaxActiveTouches> g_touches{};

// Touch callbacks.
struct TouchCallbackEntry final {
  TouchCallback callback = nullptr;
  void *userData = nullptr;
  bool occupied = false;
};

std::array<TouchCallbackEntry, kMaxTouchCallbacks> g_touchCallbacks{};

// Gesture callbacks (per gesture type).
struct GestureCallbackEntry final {
  GestureCallback callback = nullptr;
  void *userData = nullptr;
  bool occupied = false;
};

constexpr std::size_t kGestureTypeCount = 4U;
std::array<std::array<GestureCallbackEntry, kMaxGestureCallbacks>,
           kGestureTypeCount>
    g_gestureCallbacks{};

// Gesture recognizer state.
constexpr float kSwipeMinDistance = 0.05F;
constexpr float kTapMaxDistance = 0.02F;
constexpr float kPinchMinDelta = 0.01F;

// Track timing for gesture recognition (frame-based approximation).
struct TouchTiming final {
  std::int64_t touchId = 0;
  std::uint32_t framesBegan = 0U;
  bool active = false;
};

std::array<TouchTiming, kMaxActiveTouches> g_touchTimings{};
std::uint32_t g_frameCounter = 0U;

// Previous frame 2-finger state for pinch/rotate.
float g_prevTwoFingerDist = 0.0F;
float g_prevTwoFingerAngle = 0.0F;
bool g_twoFingerTracking = false;

void fire_touch_callbacks(const TouchEvent &event) noexcept {
  for (const auto &entry : g_touchCallbacks) {
    if (entry.occupied && (entry.callback != nullptr)) {
      entry.callback(event, entry.userData);
    }
  }
}

void fire_gesture_callbacks(const GestureEvent &event) noexcept {
  const auto idx = static_cast<std::size_t>(event.type);
  if (idx >= kGestureTypeCount) {
    return;
  }
  for (const auto &entry : g_gestureCallbacks[idx]) {
    if (entry.occupied && (entry.callback != nullptr)) {
      entry.callback(event, entry.userData);
    }
  }
}

ActiveTouch *find_touch(std::int64_t touchId) noexcept {
  for (auto &t : g_touches) {
    if (t.active && (t.touchId == touchId)) {
      return &t;
    }
  }
  return nullptr;
}

ActiveTouch *find_empty_touch() noexcept {
  for (auto &t : g_touches) {
    if (!t.active) {
      return &t;
    }
  }
  return nullptr;
}

TouchTiming *find_timing(std::int64_t touchId) noexcept {
  for (auto &t : g_touchTimings) {
    if (t.active && (t.touchId == touchId)) {
      return &t;
    }
  }
  return nullptr;
}

float distance(float x1, float y1, float x2, float y2) noexcept {
  const float dx = x2 - x1;
  const float dy = y2 - y1;
  return std::sqrt(dx * dx + dy * dy);
}

float angle_between(float x1, float y1, float x2, float y2) noexcept {
  return std::atan2(y2 - y1, x2 - x1);
}

void try_recognize_tap(const ActiveTouch &touch) noexcept {
  const TouchTiming *timing = find_timing(touch.touchId);
  if (timing == nullptr) {
    return;
  }
  const std::uint32_t frameDuration = g_frameCounter - timing->framesBegan;
  // Approximate time: assume 60fps → 0.3s ≈ 18 frames.
  constexpr std::uint32_t kMaxTapFrames = 18U;

  const float dist = distance(touch.startX, touch.startY, touch.x, touch.y);
  if ((dist <= kTapMaxDistance) && (frameDuration <= kMaxTapFrames)) {
    GestureEvent ge{};
    ge.type = GestureType::Tap;
    ge.tapX = touch.x;
    ge.tapY = touch.y;
    ge.tapCount = 1U;
    fire_gesture_callbacks(ge);
  }
}

void try_recognize_swipe(const ActiveTouch &touch) noexcept {
  const TouchTiming *timing = find_timing(touch.touchId);
  if (timing == nullptr) {
    return;
  }
  const std::uint32_t frameDuration = g_frameCounter - timing->framesBegan;
  constexpr std::uint32_t kMaxSwipeFrames = 30U;
  if (frameDuration > kMaxSwipeFrames) {
    return;
  }

  const float dx = touch.x - touch.startX;
  const float dy = touch.y - touch.startY;
  const float dist = distance(touch.startX, touch.startY, touch.x, touch.y);

  if (dist < kSwipeMinDistance) {
    return;
  }

  GestureEvent ge{};
  ge.type = GestureType::Swipe;
  ge.swipeVelocity = dist;

  const float absDx = dx > 0.0F ? dx : -dx;
  const float absDy = dy > 0.0F ? dy : -dy;

  if (absDx > absDy) {
    ge.swipeDir = (dx > 0.0F) ? SwipeDirection::Right : SwipeDirection::Left;
  } else {
    ge.swipeDir = (dy > 0.0F) ? SwipeDirection::Down : SwipeDirection::Up;
  }

  fire_gesture_callbacks(ge);
}

void update_two_finger_gestures() noexcept {
  // Find the first two active touches.
  const ActiveTouch *t1 = nullptr;
  const ActiveTouch *t2 = nullptr;
  for (const auto &t : g_touches) {
    if (!t.active) {
      continue;
    }
    if (t1 == nullptr) {
      t1 = &t;
    } else if (t2 == nullptr) {
      t2 = &t;
      break;
    }
  }

  if ((t1 == nullptr) || (t2 == nullptr)) {
    g_twoFingerTracking = false;
    return;
  }

  const float dist = distance(t1->x, t1->y, t2->x, t2->y);
  const float ang = angle_between(t1->x, t1->y, t2->x, t2->y);

  if (g_twoFingerTracking) {
    // Pinch.
    const float distDelta = dist - g_prevTwoFingerDist;
    if ((distDelta > kPinchMinDelta) || (distDelta < -kPinchMinDelta)) {
      GestureEvent ge{};
      ge.type = GestureType::Pinch;
      ge.pinchScale = (g_prevTwoFingerDist > 0.001F)
                          ? (dist / g_prevTwoFingerDist)
                          : 1.0F;
      ge.pinchCenterX = (t1->x + t2->x) * 0.5F;
      ge.pinchCenterY = (t1->y + t2->y) * 0.5F;
      fire_gesture_callbacks(ge);
    }

    // Rotate.
    float angleDelta = ang - g_prevTwoFingerAngle;
    constexpr float kPi = 3.14159265358979323846F;
    if (angleDelta > kPi) {
      angleDelta -= 2.0F * kPi;
    }
    if (angleDelta < -kPi) {
      angleDelta += 2.0F * kPi;
    }
    constexpr float kMinRotation = 0.01F;
    if ((angleDelta > kMinRotation) || (angleDelta < -kMinRotation)) {
      GestureEvent ge{};
      ge.type = GestureType::Rotate;
      ge.rotationRadians = angleDelta;
      fire_gesture_callbacks(ge);
    }
  }

  g_prevTwoFingerDist = dist;
  g_prevTwoFingerAngle = ang;
  g_twoFingerTracking = true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool initialize_touch_input() noexcept {
  if (g_touchInitialized) {
    return true;
  }
  g_touches = {};
  g_touchCallbacks = {};
  g_gestureCallbacks = {};
  g_touchTimings = {};
  g_frameCounter = 0U;
  g_prevTwoFingerDist = 0.0F;
  g_prevTwoFingerAngle = 0.0F;
  g_twoFingerTracking = false;
  g_mouseEmulation = false;
  g_touchInitialized = true;
  return true;
}

void shutdown_touch_input() noexcept {
  g_touchInitialized = false;
  g_touches = {};
  g_touchCallbacks = {};
  g_gestureCallbacks = {};
  g_touchTimings = {};
  g_mouseEmulation = false;
}

// ---------------------------------------------------------------------------
// Event processing
// ---------------------------------------------------------------------------

void touch_process_event(const void *nativeEvent) noexcept {
  if (nativeEvent == nullptr) {
    return;
  }
  const auto *event = static_cast<const SDL_Event *>(nativeEvent);

  switch (event->type) {
  case SDL_FINGERDOWN: {
    const auto fingerId = static_cast<std::int64_t>(event->tfinger.fingerId);
    const float x = event->tfinger.x;
    const float y = event->tfinger.y;
    const float pressure = event->tfinger.pressure;

    ActiveTouch *slot = find_empty_touch();
    if (slot != nullptr) {
      slot->touchId = fingerId;
      slot->x = x;
      slot->y = y;
      slot->startX = x;
      slot->startY = y;
      slot->pressure = pressure;
      slot->phase = TouchPhase::Began;
      slot->active = true;
    }

    // Record timing for gesture recognition.
    for (auto &timing : g_touchTimings) {
      if (!timing.active) {
        timing.touchId = fingerId;
        timing.framesBegan = g_frameCounter;
        timing.active = true;
        break;
      }
    }

    TouchEvent te{};
    te.touchId = fingerId;
    te.x = x;
    te.y = y;
    te.pressure = pressure;
    te.phase = TouchPhase::Began;
    fire_touch_callbacks(te);

    // Mouse emulation: first finger acts as mouse.
    if (g_mouseEmulation && (slot == &g_touches[0])) {
      SDL_Event fakeEvent{};
      fakeEvent.type = SDL_MOUSEBUTTONDOWN;
      fakeEvent.button.button = SDL_BUTTON_LEFT;
      fakeEvent.button.x = static_cast<int>(x * 1920.0F);
      fakeEvent.button.y = static_cast<int>(y * 1080.0F);
      input_process_event(&fakeEvent);
    }
    break;
  }

  case SDL_FINGERMOTION: {
    const auto fingerId = static_cast<std::int64_t>(event->tfinger.fingerId);
    ActiveTouch *touch = find_touch(fingerId);
    if (touch != nullptr) {
      touch->x = event->tfinger.x;
      touch->y = event->tfinger.y;
      touch->pressure = event->tfinger.pressure;
      touch->phase = TouchPhase::Moved;
    }

    TouchEvent te{};
    te.touchId = fingerId;
    te.x = event->tfinger.x;
    te.y = event->tfinger.y;
    te.pressure = event->tfinger.pressure;
    te.phase = TouchPhase::Moved;
    fire_touch_callbacks(te);

    // Mouse emulation.
    if (g_mouseEmulation && (touch == &g_touches[0])) {
      SDL_Event fakeEvent{};
      fakeEvent.type = SDL_MOUSEMOTION;
      fakeEvent.motion.x = static_cast<int>(event->tfinger.x * 1920.0F);
      fakeEvent.motion.y = static_cast<int>(event->tfinger.y * 1080.0F);
      fakeEvent.motion.xrel = static_cast<int>(event->tfinger.dx * 1920.0F);
      fakeEvent.motion.yrel = static_cast<int>(event->tfinger.dy * 1080.0F);
      input_process_event(&fakeEvent);
    }

    // Update two-finger gestures on move.
    update_two_finger_gestures();
    break;
  }

  case SDL_FINGERUP: {
    const auto fingerId = static_cast<std::int64_t>(event->tfinger.fingerId);
    ActiveTouch *touch = find_touch(fingerId);
    if (touch != nullptr) {
      touch->x = event->tfinger.x;
      touch->y = event->tfinger.y;
      touch->phase = TouchPhase::Ended;

      // Try gesture recognition before deactivating.
      try_recognize_tap(*touch);
      try_recognize_swipe(*touch);

      touch->active = false;
    }

    // Clean up timing.
    TouchTiming *timing = find_timing(fingerId);
    if (timing != nullptr) {
      timing->active = false;
    }

    TouchEvent te{};
    te.touchId = fingerId;
    te.x = event->tfinger.x;
    te.y = event->tfinger.y;
    te.pressure = 0.0F;
    te.phase = TouchPhase::Ended;
    fire_touch_callbacks(te);

    // Mouse emulation.
    if (g_mouseEmulation && (touch == &g_touches[0])) {
      SDL_Event fakeEvent{};
      fakeEvent.type = SDL_MOUSEBUTTONUP;
      fakeEvent.button.button = SDL_BUTTON_LEFT;
      input_process_event(&fakeEvent);
    }

    // Reset two-finger tracking if we lose a finger.
    std::uint32_t activeCount = 0;
    for (const auto &t : g_touches) {
      if (t.active) {
        ++activeCount;
      }
    }
    if (activeCount < 2U) {
      g_twoFingerTracking = false;
    }
    break;
  }

  default:
    break;
  }
}

void touch_begin_frame() noexcept { /* Nothing needed currently. */ }

void touch_end_frame() noexcept { ++g_frameCounter; }

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

std::uint32_t active_touch_count() noexcept {
  std::uint32_t count = 0U;
  for (const auto &t : g_touches) {
    if (t.active) {
      ++count;
    }
  }
  return count;
}

bool get_active_touch(std::uint32_t index, TouchEvent *outTouch) noexcept {
  if (outTouch == nullptr) {
    return false;
  }
  std::uint32_t seen = 0U;
  for (const auto &t : g_touches) {
    if (!t.active) {
      continue;
    }
    if (seen == index) {
      outTouch->touchId = t.touchId;
      outTouch->x = t.x;
      outTouch->y = t.y;
      outTouch->pressure = t.pressure;
      outTouch->phase = t.phase;
      return true;
    }
    ++seen;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

bool register_touch_callback(TouchCallback cb, void *userData) noexcept {
  if (cb == nullptr) {
    return false;
  }
  for (auto &entry : g_touchCallbacks) {
    if (!entry.occupied) {
      entry.callback = cb;
      entry.userData = userData;
      entry.occupied = true;
      return true;
    }
  }
  log_message(LogLevel::Warning, kLogChannel,
              "register_touch_callback: no free slots");
  return false;
}

bool unregister_touch_callback(TouchCallback cb, void *userData) noexcept {
  for (auto &entry : g_touchCallbacks) {
    if (entry.occupied && (entry.callback == cb) &&
        (entry.userData == userData)) {
      entry = {};
      return true;
    }
  }
  return false;
}

bool register_gesture_callback(GestureType type, GestureCallback cb,
                               void *userData) noexcept {
  if (cb == nullptr) {
    return false;
  }
  const auto idx = static_cast<std::size_t>(type);
  if (idx >= kGestureTypeCount) {
    return false;
  }
  for (auto &entry : g_gestureCallbacks[idx]) {
    if (!entry.occupied) {
      entry.callback = cb;
      entry.userData = userData;
      entry.occupied = true;
      return true;
    }
  }
  log_message(LogLevel::Warning, kLogChannel,
              "register_gesture_callback: no free slots");
  return false;
}

bool unregister_gesture_callback(GestureType type, GestureCallback cb,
                                 void *userData) noexcept {
  const auto idx = static_cast<std::size_t>(type);
  if (idx >= kGestureTypeCount) {
    return false;
  }
  for (auto &entry : g_gestureCallbacks[idx]) {
    if (entry.occupied && (entry.callback == cb) &&
        (entry.userData == userData)) {
      entry = {};
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Mouse emulation
// ---------------------------------------------------------------------------

void set_touch_mouse_emulation(bool enabled) noexcept {
  g_mouseEmulation = enabled;
}

bool is_touch_mouse_emulation_enabled() noexcept { return g_mouseEmulation; }

} // namespace engine::core
