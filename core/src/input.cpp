#include "engine/core/input.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#if defined(__clang__) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include <array>
#include <cstdint>
#include <cstring>

#include "engine/core/event_bus.h"

namespace engine::core {

namespace {

constexpr int kMaxScancodes = 512;
constexpr int kMaxMouseButtons = 5;
constexpr int kMaxGamepadButtons = 16;
constexpr int kMaxGamepadAxes = 6;
constexpr std::size_t kMaxActionNameLength = 63U;

bool g_inputInitialized = false;

std::array<bool, kMaxScancodes> g_keyState{};
std::array<bool, kMaxScancodes> g_prevKeyState{};

struct MouseStateInternal final {
  int x = 0;
  int y = 0;
  int deltaX = 0;
  int deltaY = 0;
  int scrollDelta = 0;
  std::array<bool, kMaxMouseButtons> buttons{};
  std::array<bool, kMaxMouseButtons> prevButtons{};
};

MouseStateInternal g_mouse{};

struct GamepadStateInternal final {
  bool connected = false;
  std::array<bool, kMaxGamepadButtons> buttons{};
  std::array<std::int16_t, kMaxGamepadAxes> axes{};
};

GamepadStateInternal g_gamepad{};

struct ActionBinding final {
  char name[kMaxActionNameLength + 1U] = {};
  KeyScancode key = -1;
  int mouseButton = -1;
  bool occupied = false;
};

struct AxisBinding final {
  char name[kMaxActionNameLength + 1U] = {};
  KeyScancode negativeKey = -1;
  KeyScancode positiveKey = -1;
  bool occupied = false;
};

std::array<ActionBinding, kMaxActions> g_actions{};
std::array<AxisBinding, kMaxAxes> g_axes{};

const ActionBinding *find_action(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }

  for (const auto &a : g_actions) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      return &a;
    }
  }

  return nullptr;
}

const AxisBinding *find_axis(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }

  for (const auto &a : g_axes) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      return &a;
    }
  }

  return nullptr;
}

} // namespace

bool initialize_input() noexcept {
  if (g_inputInitialized) {
    return true;
  }

  g_keyState = {};
  g_prevKeyState = {};
  g_mouse = {};
  g_actions = {};
  g_axes = {};
  g_gamepad = {};
  g_inputInitialized = true;
  return true;
}

void shutdown_input() noexcept {
  g_inputInitialized = false;
  g_keyState = {};
  g_prevKeyState = {};
  g_mouse = {};
  g_actions = {};
  g_axes = {};
  g_gamepad = {};
}

void begin_input_frame() noexcept {
  g_prevKeyState = g_keyState;
  g_mouse.prevButtons = g_mouse.buttons;
  g_mouse.deltaX = 0;
  g_mouse.deltaY = 0;
  g_mouse.scrollDelta = 0;
}

void input_process_event(const void *nativeEvent) noexcept {
  if (nativeEvent == nullptr) {
    return;
  }

  const auto *event = static_cast<const SDL_Event *>(nativeEvent);

  switch (event->type) {
  case SDL_KEYDOWN:
  case SDL_KEYUP: {
    const int scancode = static_cast<int>(event->key.keysym.scancode);
    if ((scancode >= 0) && (scancode < kMaxScancodes)) {
      const bool down = (event->type == SDL_KEYDOWN);
      g_keyState[static_cast<std::size_t>(scancode)] = down;
      KeyEvent ke{};
      ke.scancode = scancode;
      ke.down = down;
      emit(ke);
    }
    break;
  }
  case SDL_MOUSEMOTION: {
    g_mouse.x = event->motion.x;
    g_mouse.y = event->motion.y;
    g_mouse.deltaX += event->motion.xrel;
    g_mouse.deltaY += event->motion.yrel;
    MouseMoveEvent me{};
    me.x = event->motion.x;
    me.y = event->motion.y;
    me.deltaX = event->motion.xrel;
    me.deltaY = event->motion.yrel;
    emit(me);
    break;
  }
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP: {
    const int button = static_cast<int>(event->button.button) - 1;
    if ((button >= 0) && (button < kMaxMouseButtons)) {
      const bool down = (event->type == SDL_MOUSEBUTTONDOWN);
      g_mouse.buttons[static_cast<std::size_t>(button)] = down;
      MouseButtonEvent mbe{};
      mbe.button = button;
      mbe.down = down;
      emit(mbe);
    }
    break;
  }
  case SDL_MOUSEWHEEL:
    g_mouse.scrollDelta += event->wheel.y;
    break;
  case SDL_CONTROLLERDEVICEADDED:
    g_gamepad.connected = true;
    break;
  case SDL_CONTROLLERDEVICEREMOVED:
    g_gamepad.connected = false;
    g_gamepad.buttons = {};
    g_gamepad.axes = {};
    break;
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    const int button = static_cast<int>(event->cbutton.button);
    if ((button >= 0) && (button < kMaxGamepadButtons)) {
      g_gamepad.buttons[static_cast<std::size_t>(button)] =
          (event->type == SDL_CONTROLLERBUTTONDOWN);
    }
    break;
  }
  case SDL_CONTROLLERAXISMOTION: {
    const int axis = static_cast<int>(event->caxis.axis);
    if ((axis >= 0) && (axis < kMaxGamepadAxes)) {
      g_gamepad.axes[static_cast<std::size_t>(axis)] = event->caxis.value;
    }
    break;
  }
  default:
    break;
  }
}

void end_input_frame() noexcept {
  // Keyboard and mouse state is maintained per-event; no sync needed.
}

bool is_key_down(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  return g_keyState[static_cast<std::size_t>(scancode)];
}

bool is_key_pressed(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  const auto idx = static_cast<std::size_t>(scancode);
  return g_keyState[idx] && !g_prevKeyState[idx];
}

bool is_key_released(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  const auto idx = static_cast<std::size_t>(scancode);
  return !g_keyState[idx] && g_prevKeyState[idx];
}

MouseState mouse_state() noexcept {
  MouseState state{};
  state.x = g_mouse.x;
  state.y = g_mouse.y;
  state.deltaX = g_mouse.deltaX;
  state.deltaY = g_mouse.deltaY;
  state.scrollDelta = g_mouse.scrollDelta;
  for (int i = 0; i < kMaxMouseButtons; ++i) {
    state.buttons[i] = g_mouse.buttons[static_cast<std::size_t>(i)];
  }
  return state;
}

bool is_mouse_button_down(int button) noexcept {
  if ((button < 0) || (button >= kMaxMouseButtons)) {
    return false;
  }
  return g_mouse.buttons[static_cast<std::size_t>(button)];
}

bool is_mouse_button_pressed(int button) noexcept {
  if ((button < 0) || (button >= kMaxMouseButtons)) {
    return false;
  }
  const auto idx = static_cast<std::size_t>(button);
  return g_mouse.buttons[idx] && !g_mouse.prevButtons[idx];
}

bool register_action(const char *name, KeyScancode key,
                     int mouseButton) noexcept {
  if (name == nullptr) {
    return false;
  }

  const std::size_t nameLen = std::strlen(name);
  if ((nameLen == 0U) || (nameLen > kMaxActionNameLength)) {
    return false;
  }

  for (auto &a : g_actions) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      a.key = key;
      a.mouseButton = mouseButton;
      return true;
    }
  }

  for (auto &a : g_actions) {
    if (!a.occupied) {
      std::memcpy(a.name, name, nameLen + 1U);
      a.key = key;
      a.mouseButton = mouseButton;
      a.occupied = true;
      return true;
    }
  }

  return false;
}

bool is_action_down(const char *name) noexcept {
  const ActionBinding *a = find_action(name);
  if (a == nullptr) {
    return false;
  }

  if ((a->key >= 0) && is_key_down(a->key)) {
    return true;
  }

  if ((a->mouseButton >= 0) && is_mouse_button_down(a->mouseButton)) {
    return true;
  }

  return false;
}

bool is_action_pressed(const char *name) noexcept {
  const ActionBinding *a = find_action(name);
  if (a == nullptr) {
    return false;
  }

  if ((a->key >= 0) && is_key_pressed(a->key)) {
    return true;
  }

  if ((a->mouseButton >= 0) && is_mouse_button_pressed(a->mouseButton)) {
    return true;
  }

  return false;
}

float action_value(const char *name) noexcept {
  return is_action_down(name) ? 1.0F : 0.0F;
}

bool register_axis(const char *name, KeyScancode negativeKey,
                   KeyScancode positiveKey) noexcept {
  if (name == nullptr) {
    return false;
  }

  const std::size_t nameLen = std::strlen(name);
  if ((nameLen == 0U) || (nameLen > kMaxActionNameLength)) {
    return false;
  }

  for (auto &a : g_axes) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      a.negativeKey = negativeKey;
      a.positiveKey = positiveKey;
      return true;
    }
  }

  for (auto &a : g_axes) {
    if (!a.occupied) {
      std::memcpy(a.name, name, nameLen + 1U);
      a.negativeKey = negativeKey;
      a.positiveKey = positiveKey;
      a.occupied = true;
      return true;
    }
  }

  return false;
}

float axis_value(const char *name) noexcept {
  const AxisBinding *axis = find_axis(name);
  if (axis == nullptr) {
    return 0.0F;
  }

  const bool negDown =
      (axis->negativeKey >= 0) && is_key_down(axis->negativeKey);
  const bool posDown =
      (axis->positiveKey >= 0) && is_key_down(axis->positiveKey);

  if (negDown == posDown) {
    return 0.0F;
  }

  return posDown ? 1.0F : -1.0F;
}

bool is_gamepad_connected() noexcept { return g_gamepad.connected; }

bool is_gamepad_button_down(int button) noexcept {
  if ((button < 0) || (button >= kMaxGamepadButtons) || !g_gamepad.connected) {
    return false;
  }
  return g_gamepad.buttons[static_cast<std::size_t>(button)];
}

float gamepad_axis_value(int axis, int deadzone) noexcept {
  if ((axis < 0) || (axis >= kMaxGamepadAxes) || !g_gamepad.connected) {
    return 0.0F;
  }

  const int raw =
      static_cast<int>(g_gamepad.axes[static_cast<std::size_t>(axis)]);
  const int absRaw = (raw < 0) ? -raw : raw;
  const int dz = (deadzone < 0) ? 0 : deadzone;
  if (absRaw <= dz) {
    return 0.0F;
  }

  const float denom = 32767.0F - static_cast<float>(dz);
  if (denom <= 0.0F) {
    return 0.0F;
  }

  float normalized = (static_cast<float>(absRaw - dz) / denom);
  if (normalized > 1.0F) {
    normalized = 1.0F;
  }
  return (raw < 0) ? -normalized : normalized;
}

} // namespace engine::core
