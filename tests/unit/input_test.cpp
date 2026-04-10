#include <cstdio>
#include <cstring>

#include "engine/core/input.h"

#if defined(__clang__) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H
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

bool test_init_shutdown() noexcept {
  if (!initialize_input()) {
    return false;
  }
  shutdown_input();
  return true;
}

bool test_key_state_defaults() noexcept {
  if (!initialize_input()) {
    return false;
  }

  if (is_key_down(kKey_A)) {
    shutdown_input();
    return false;
  }
  if (is_key_pressed(kKey_A)) {
    shutdown_input();
    return false;
  }
  if (is_key_released(kKey_A)) {
    shutdown_input();
    return false;
  }

  shutdown_input();
  return true;
}

bool test_mouse_state_defaults() noexcept {
  if (!initialize_input()) {
    return false;
  }

  const MouseState ms = mouse_state();
  if ((ms.x != 0) || (ms.y != 0)) {
    shutdown_input();
    return false;
  }
  if ((ms.deltaX != 0) || (ms.deltaY != 0)) {
    shutdown_input();
    return false;
  }
  if (ms.scrollDelta != 0) {
    shutdown_input();
    return false;
  }
  for (int i = 0; i < 5; ++i) {
    if (ms.buttons[i]) {
      shutdown_input();
      return false;
    }
  }

  shutdown_input();
  return true;
}

bool test_action_register() noexcept {
  if (!initialize_input()) {
    return false;
  }

  if (!register_action("jump", kKey_Space)) {
    shutdown_input();
    return false;
  }

  // Not down (key is not down).
  if (is_action_down("jump")) {
    shutdown_input();
    return false;
  }
  if (is_action_pressed("jump")) {
    shutdown_input();
    return false;
  }

  // Unknown action.
  if (is_action_down("unknown")) {
    shutdown_input();
    return false;
  }

  if (action_value("jump") != 0.0F) {
    shutdown_input();
    return false;
  }

  shutdown_input();
  return true;
}

bool test_action_overwrite() noexcept {
  if (!initialize_input()) {
    return false;
  }

  if (!register_action("fire", kKey_Space)) {
    shutdown_input();
    return false;
  }
  if (!register_action("fire", kKey_Return)) {
    shutdown_input();
    return false;
  }

  shutdown_input();
  return true;
}

bool test_max_actions() noexcept {
  if (!initialize_input()) {
    return false;
  }

  char name[16] = {};
  for (std::size_t i = 0U; i < kMaxActions; ++i) {
    std::snprintf(name, sizeof(name), "act_%zu", i);
    if (!register_action(name, static_cast<KeyScancode>(i + 4))) {
      shutdown_input();
      return false;
    }
  }

  // One more should fail.
  if (register_action("overflow", kKey_A)) {
    shutdown_input();
    return false;
  }

  shutdown_input();
  return true;
}

bool test_axis_register_and_rebind() noexcept {
  if (!initialize_input()) {
    return false;
  }

  if (!register_axis("move_x", kKey_A, kKey_D)) {
    shutdown_input();
    return false;
  }

  if (!register_axis("move_x", kKey_Left, kKey_Right)) {
    shutdown_input();
    return false;
  }

  if (axis_value("move_x") != 0.0F) {
    shutdown_input();
    return false;
  }

  shutdown_input();
  return true;
}

bool test_axis_value_from_key_events() noexcept {
  if (!initialize_input()) {
    return false;
  }

  if (!register_axis("move_x", kKey_A, kKey_D)) {
    shutdown_input();
    return false;
  }

  SDL_Event ev{};

  begin_input_frame();
  ev.type = SDL_KEYDOWN;
  ev.key.keysym.scancode = static_cast<SDL_Scancode>(kKey_D);
  input_process_event(&ev);
  end_input_frame();
  if (axis_value("move_x") != 1.0F) {
    shutdown_input();
    return false;
  }

  begin_input_frame();
  ev.type = SDL_KEYUP;
  ev.key.keysym.scancode = static_cast<SDL_Scancode>(kKey_D);
  input_process_event(&ev);
  ev.type = SDL_KEYDOWN;
  ev.key.keysym.scancode = static_cast<SDL_Scancode>(kKey_A);
  input_process_event(&ev);
  end_input_frame();
  if (axis_value("move_x") != -1.0F) {
    shutdown_input();
    return false;
  }

  begin_input_frame();
  ev.type = SDL_KEYDOWN;
  ev.key.keysym.scancode = static_cast<SDL_Scancode>(kKey_D);
  input_process_event(&ev);
  end_input_frame();
  if (axis_value("move_x") != 0.0F) {
    shutdown_input();
    return false;
  }

  shutdown_input();
  return true;
}

bool test_bounds_check() noexcept {
  if (!initialize_input()) {
    return false;
  }

  if (is_key_down(-1)) {
    shutdown_input();
    return false;
  }
  if (is_key_down(999)) {
    shutdown_input();
    return false;
  }
  if (is_key_pressed(-1)) {
    shutdown_input();
    return false;
  }
  if (is_key_released(999)) {
    shutdown_input();
    return false;
  }

  if (is_mouse_button_down(-1)) {
    shutdown_input();
    return false;
  }
  if (is_mouse_button_down(5)) {
    shutdown_input();
    return false;
  }
  if (is_mouse_button_pressed(-1)) {
    shutdown_input();
    return false;
  }

  if (register_action(nullptr, kKey_A)) {
    shutdown_input();
    return false;
  }
  if (is_action_down(nullptr)) {
    shutdown_input();
    return false;
  }

  shutdown_input();
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

  std::printf("--- input tests ---\n");
  run("init_shutdown", &test_init_shutdown);
  run("key_state_defaults", &test_key_state_defaults);
  run("mouse_state_defaults", &test_mouse_state_defaults);
  run("action_register", &test_action_register);
  run("action_overwrite", &test_action_overwrite);
  run("max_actions", &test_max_actions);
  run("axis_register_and_rebind", &test_axis_register_and_rebind);
  run("axis_value_from_key_events", &test_axis_value_from_key_events);
  run("bounds_check", &test_bounds_check);

  std::printf("--- %d passed, %d failed ---\n", passed, failed);
  return (failed > 0) ? 1 : 0;
}
