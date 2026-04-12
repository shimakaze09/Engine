#include <cstdio>
#include <cstring>

#include "engine/core/input.h"
#include "engine/core/touch_input.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
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

using namespace engine::core;

namespace {

// Callback tracking state.
struct TouchCBState {
  int beganCount = 0;
  int movedCount = 0;
  int endedCount = 0;
  float lastX = 0.0F;
  float lastY = 0.0F;
};

void touch_cb(const TouchEvent &event, void *userData) noexcept {
  auto *state = static_cast<TouchCBState *>(userData);
  switch (event.phase) {
  case TouchPhase::Began:
    ++state->beganCount;
    break;
  case TouchPhase::Moved:
    ++state->movedCount;
    break;
  case TouchPhase::Ended:
    ++state->endedCount;
    break;
  default:
    break;
  }
  state->lastX = event.x;
  state->lastY = event.y;
}

struct GestureCBState {
  int tapCount = 0;
  int swipeCount = 0;
  int pinchCount = 0;
  int rotateCount = 0;
  SwipeDirection lastSwipeDir = SwipeDirection::Right;
  float lastPinchScale = 1.0F;
};

void tap_cb(const GestureEvent &event, void *userData) noexcept {
  auto *state = static_cast<GestureCBState *>(userData);
  if (event.type == GestureType::Tap) {
    ++state->tapCount;
  }
}

void swipe_cb(const GestureEvent &event, void *userData) noexcept {
  auto *state = static_cast<GestureCBState *>(userData);
  if (event.type == GestureType::Swipe) {
    ++state->swipeCount;
    state->lastSwipeDir = event.swipeDir;
  }
}

void pinch_cb(const GestureEvent &event, void *userData) noexcept {
  auto *state = static_cast<GestureCBState *>(userData);
  if (event.type == GestureType::Pinch) {
    ++state->pinchCount;
    state->lastPinchScale = event.pinchScale;
  }
}

bool init_all() noexcept {
  if (!initialize_input()) {
    return false;
  }
  if (!initialize_touch_input()) {
    shutdown_input();
    return false;
  }
  return true;
}

void shutdown_all() noexcept {
  shutdown_touch_input();
  shutdown_input();
}

// Helper to simulate finger events.
void sim_finger_down(SDL_FingerID fingerId, float x, float y) noexcept {
  SDL_Event ev{};
  ev.type = SDL_FINGERDOWN;
  ev.tfinger.fingerId = fingerId;
  ev.tfinger.x = x;
  ev.tfinger.y = y;
  ev.tfinger.pressure = 1.0F;
  input_process_event(&ev);
}

void sim_finger_move(SDL_FingerID fingerId, float x, float y, float dx,
                     float dy) noexcept {
  SDL_Event ev{};
  ev.type = SDL_FINGERMOTION;
  ev.tfinger.fingerId = fingerId;
  ev.tfinger.x = x;
  ev.tfinger.y = y;
  ev.tfinger.dx = dx;
  ev.tfinger.dy = dy;
  ev.tfinger.pressure = 1.0F;
  input_process_event(&ev);
}

void sim_finger_up(SDL_FingerID fingerId, float x, float y) noexcept {
  SDL_Event ev{};
  ev.type = SDL_FINGERUP;
  ev.tfinger.fingerId = fingerId;
  ev.tfinger.x = x;
  ev.tfinger.y = y;
  ev.tfinger.pressure = 0.0F;
  input_process_event(&ev);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

bool test_touch_lifecycle() noexcept {
  if (!init_all()) {
    return false;
  }

  TouchCBState cbState{};
  register_touch_callback(&touch_cb, &cbState);

  begin_input_frame();
  sim_finger_down(1, 0.5F, 0.5F);
  end_input_frame();

  if (cbState.beganCount != 1) {
    shutdown_all();
    return false;
  }
  if (active_touch_count() != 1U) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_finger_move(1, 0.6F, 0.6F, 0.1F, 0.1F);
  end_input_frame();

  if (cbState.movedCount != 1) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_finger_up(1, 0.6F, 0.6F);
  end_input_frame();

  if (cbState.endedCount != 1) {
    shutdown_all();
    return false;
  }
  if (active_touch_count() != 0U) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_multi_touch() noexcept {
  if (!init_all()) {
    return false;
  }

  begin_input_frame();
  sim_finger_down(1, 0.3F, 0.3F);
  sim_finger_down(2, 0.7F, 0.7F);
  end_input_frame();

  if (active_touch_count() != 2U) {
    shutdown_all();
    return false;
  }

  TouchEvent te{};
  if (!get_active_touch(0U, &te)) {
    shutdown_all();
    return false;
  }
  if (!get_active_touch(1U, &te)) {
    shutdown_all();
    return false;
  }

  // Out of range.
  if (get_active_touch(2U, &te)) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_finger_up(1, 0.3F, 0.3F);
  end_input_frame();

  if (active_touch_count() != 1U) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_finger_up(2, 0.7F, 0.7F);
  end_input_frame();

  if (active_touch_count() != 0U) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_tap_gesture() noexcept {
  if (!init_all()) {
    return false;
  }

  GestureCBState gState{};
  register_gesture_callback(GestureType::Tap, &tap_cb, &gState);

  // Quick tap: finger down and up at same position within a few frames.
  begin_input_frame();
  sim_finger_down(1, 0.5F, 0.5F);
  end_input_frame();

  begin_input_frame();
  sim_finger_up(1, 0.5F, 0.5F);
  end_input_frame();

  if (gState.tapCount != 1) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_swipe_gesture() noexcept {
  if (!init_all()) {
    return false;
  }

  GestureCBState gState{};
  register_gesture_callback(GestureType::Swipe, &swipe_cb, &gState);

  // Swipe right: finger down, move right significantly, finger up.
  begin_input_frame();
  sim_finger_down(1, 0.2F, 0.5F);
  end_input_frame();

  begin_input_frame();
  sim_finger_move(1, 0.4F, 0.5F, 0.2F, 0.0F);
  end_input_frame();

  begin_input_frame();
  sim_finger_up(1, 0.4F, 0.5F);
  end_input_frame();

  if (gState.swipeCount != 1) {
    shutdown_all();
    return false;
  }
  if (gState.lastSwipeDir != SwipeDirection::Right) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_pinch_gesture() noexcept {
  if (!init_all()) {
    return false;
  }

  GestureCBState gState{};
  register_gesture_callback(GestureType::Pinch, &pinch_cb, &gState);

  // Two fingers start close, then spread apart.
  begin_input_frame();
  sim_finger_down(1, 0.4F, 0.5F);
  sim_finger_down(2, 0.6F, 0.5F);
  end_input_frame();

  // Move them apart.
  begin_input_frame();
  sim_finger_move(1, 0.3F, 0.5F, -0.1F, 0.0F);
  end_input_frame();

  begin_input_frame();
  sim_finger_move(2, 0.7F, 0.5F, 0.1F, 0.0F);
  end_input_frame();

  // At least one pinch event should have fired.
  if (gState.pinchCount < 1) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_finger_up(1, 0.3F, 0.5F);
  sim_finger_up(2, 0.7F, 0.5F);
  end_input_frame();

  shutdown_all();
  return true;
}

bool test_mouse_emulation() noexcept {
  if (!init_all()) {
    return false;
  }

  set_touch_mouse_emulation(true);
  if (!is_touch_mouse_emulation_enabled()) {
    shutdown_all();
    return false;
  }

  // A finger down should also trigger mouse button down via emulation.
  begin_input_frame();
  sim_finger_down(1, 0.5F, 0.5F);
  end_input_frame();

  // Mouse button 0 (left) should be down.
  if (!is_mouse_button_down(0)) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_finger_up(1, 0.5F, 0.5F);
  end_input_frame();

  set_touch_mouse_emulation(false);
  shutdown_all();
  return true;
}

bool test_callback_register_unregister() noexcept {
  if (!init_all()) {
    return false;
  }

  TouchCBState cbState{};
  if (!register_touch_callback(&touch_cb, &cbState)) {
    shutdown_all();
    return false;
  }

  if (!unregister_touch_callback(&touch_cb, &cbState)) {
    shutdown_all();
    return false;
  }

  // After unregister, callback should not fire.
  begin_input_frame();
  sim_finger_down(1, 0.5F, 0.5F);
  end_input_frame();

  if (cbState.beganCount != 0) {
    shutdown_all();
    return false;
  }

  // Clean up the touch.
  begin_input_frame();
  sim_finger_up(1, 0.5F, 0.5F);
  end_input_frame();

  // Null callback should fail.
  if (register_touch_callback(nullptr)) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_null_edge_cases() noexcept {
  if (!init_all()) {
    return false;
  }

  if (get_active_touch(0U, nullptr)) {
    shutdown_all();
    return false;
  }

  // Process null event should not crash.
  touch_process_event(nullptr);

  shutdown_all();
  return true;
}

} // namespace

int main() {
  int passed = 0;
  int failed = 0;

  auto run = [&](const char *name, bool (*fn)() noexcept) {
    if (fn()) {
      ++passed;
      std::printf("  PASS  %s\n", name);
    } else {
      ++failed;
      std::printf("  FAIL  %s\n", name);
    }
  };

  std::printf("--- touch_input tests ---\n");
  run("touch_lifecycle", &test_touch_lifecycle);
  run("multi_touch", &test_multi_touch);
  run("tap_gesture", &test_tap_gesture);
  run("swipe_gesture", &test_swipe_gesture);
  run("pinch_gesture", &test_pinch_gesture);
  run("mouse_emulation", &test_mouse_emulation);
  run("callback_register_unregister", &test_callback_register_unregister);
  run("null_edge_cases", &test_null_edge_cases);

  std::printf("--- %d passed, %d failed ---\n", passed, failed);
  return (failed > 0) ? 1 : 0;
}
