// Implements input behavior for the Engine core engine.

#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/touch_input.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/event_bus.h"
#include "engine/core/logging.h"

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

/// One controller slot, keyed to the SDL instance id it was announced
/// under so a second controller's events never land on the first.
struct GamepadStateInternal final {
  bool connected = false;
  std::uint32_t instanceId = 0U;
  std::array<bool, kMaxGamepadButtons> buttons{};
  std::array<std::int16_t, kMaxGamepadAxes> axes{};
};

std::array<GamepadStateInternal, static_cast<std::size_t>(kMaxGamepads)>
    g_gamepads{};

// The engine vocabulary must stay SDL's numbering: persisted bindings and
// scripts written against the raw codes keep their meaning.
static_assert(kGamepadButton_South == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert(kGamepadButton_East == SDL_GAMEPAD_BUTTON_EAST);
static_assert(kGamepadButton_West == SDL_GAMEPAD_BUTTON_WEST);
static_assert(kGamepadButton_North == SDL_GAMEPAD_BUTTON_NORTH);
static_assert(kGamepadButton_Back == SDL_GAMEPAD_BUTTON_BACK);
static_assert(kGamepadButton_Guide == SDL_GAMEPAD_BUTTON_GUIDE);
static_assert(kGamepadButton_Start == SDL_GAMEPAD_BUTTON_START);
static_assert(kGamepadButton_LeftStick == SDL_GAMEPAD_BUTTON_LEFT_STICK);
static_assert(kGamepadButton_RightStick == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
static_assert(kGamepadButton_LeftShoulder == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
static_assert(kGamepadButton_RightShoulder ==
              SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
static_assert(kGamepadButton_DpadUp == SDL_GAMEPAD_BUTTON_DPAD_UP);
static_assert(kGamepadButton_DpadDown == SDL_GAMEPAD_BUTTON_DPAD_DOWN);
static_assert(kGamepadButton_DpadLeft == SDL_GAMEPAD_BUTTON_DPAD_LEFT);
static_assert(kGamepadButton_DpadRight == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(kGamepadAxis_LeftX == SDL_GAMEPAD_AXIS_LEFTX);
static_assert(kGamepadAxis_LeftY == SDL_GAMEPAD_AXIS_LEFTY);
static_assert(kGamepadAxis_RightX == SDL_GAMEPAD_AXIS_RIGHTX);
static_assert(kGamepadAxis_RightY == SDL_GAMEPAD_AXIS_RIGHTY);
static_assert(kGamepadAxis_LeftTrigger == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
static_assert(kGamepadAxis_RightTrigger == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

/// Slot holding the device with this instance id, or nullptr.
GamepadStateInternal *find_gamepad(std::uint32_t instanceId) noexcept {
  for (GamepadStateInternal &slot : g_gamepads) {
    if (slot.connected && (slot.instanceId == instanceId)) {
      return &slot;
    }
  }
  return nullptr;
}

/// Records a device's arrival in its existing slot or the first free one
/// and asks the platform to open it so its events are delivered. A device
/// past the slot table is logged and left closed.
void attach_gamepad(std::uint32_t instanceId) noexcept {
  GamepadStateInternal *slot = find_gamepad(instanceId);
  if (slot == nullptr) {
    for (GamepadStateInternal &candidate : g_gamepads) {
      if (!candidate.connected) {
        slot = &candidate;
        break;
      }
    }
  }
  if (slot == nullptr) {
    log_message(LogLevel::Warning, "input",
                "gamepad ignored: every controller slot is in use");
    return;
  }
  *slot = GamepadStateInternal{};
  slot->connected = true;
  slot->instanceId = instanceId;
  platform_open_gamepad(instanceId);
}

/// Releases a device's slot and closes the device behind it.
void detach_gamepad(std::uint32_t instanceId) noexcept {
  GamepadStateInternal *slot = find_gamepad(instanceId);
  if (slot != nullptr) {
    *slot = GamepadStateInternal{};
  }
  platform_close_gamepad(instanceId);
}

/// Slot for a query index, or nullptr when out of range or empty.
const GamepadStateInternal *gamepad_slot(int gamepad) noexcept {
  if ((gamepad < 0) || (gamepad >= kMaxGamepads)) {
    return nullptr;
  }
  const GamepadStateInternal &slot =
      g_gamepads[static_cast<std::size_t>(gamepad)];
  return slot.connected ? &slot : nullptr;
}

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

/// Finds the matching object or resource for action.
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

/// Finds the matching object or resource for axis.
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

/// Initializes the owning system for input. Persisted per-user rebindings
/// are restored here when the file exists, and take precedence over script
/// defaults registered later.
bool initialize_input() noexcept {
  if (g_inputInitialized) {
    return true;
  }

  g_keyState = {};
  g_prevKeyState = {};
  g_mouse = {};
  g_actions = {};
  g_axes = {};
  g_gamepads = {};
  g_inputInitialized = true;

  static_cast<void>(initialize_input_mapper());
  static_cast<void>(initialize_touch_input());

  char bindingsPath[512] = {};
  if (input_bindings_default_path(bindingsPath, sizeof(bindingsPath))) {
    FILE *probe = nullptr;
#ifdef _WIN32
    if ((fopen_s(&probe, bindingsPath, "rb") == 0) && (probe != nullptr)) {
#else
    probe = std::fopen(bindingsPath, "rb");
    if (probe != nullptr) {
#endif
      std::fclose(probe);
      if (load_input_bindings(bindingsPath)) {
        log_message(LogLevel::Info, "input", "loaded saved input bindings");
      }
    }
  }
  return true;
}

/// Shuts down the owning system for input, including the touch subsystem
/// whose events and frames route through the general input entry points.
/// Clears run-scoped gameplay registrations; device state and the
/// persisted input map stay untouched (#168).
void clear_gameplay_bindings() noexcept {
  g_actions = {};
  g_axes = {};
  clear_action_callbacks();
  clear_touch_callbacks();
}

/// Live action registrations (teardown-regression introspection).
std::size_t gameplay_action_count() noexcept {
  std::size_t count = 0U;
  for (const auto &a : g_actions) {
    if (a.occupied) {
      ++count;
    }
  }
  return count;
}

/// Live axis registrations (teardown-regression introspection).
std::size_t gameplay_axis_count() noexcept {
  std::size_t count = 0U;
  for (const auto &a : g_axes) {
    if (a.occupied) {
      ++count;
    }
  }
  return count;
}

void shutdown_input() noexcept {
  shutdown_touch_input();
  shutdown_input_mapper();
  g_inputInitialized = false;
  g_keyState = {};
  g_prevKeyState = {};
  g_mouse = {};
  g_actions = {};
  g_axes = {};
  g_gamepads = {};
}

/// Begins the requested operation or profiling range for input frame.
void begin_input_frame() noexcept {
  g_prevKeyState = g_keyState;
  g_mouse.prevButtons = g_mouse.buttons;
  g_mouse.deltaX = 0;
  g_mouse.deltaY = 0;
  g_mouse.scrollDelta = 0;
  input_mapper_begin_frame();
  touch_begin_frame();
}

void input_process_event(const void *nativeEvent) noexcept {
  if (nativeEvent == nullptr) {
    return;
  }

  const auto *event = static_cast<const SDL_Event *>(nativeEvent);

  switch (event->type) {
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP: {
    const int scancode = static_cast<int>(event->key.scancode);
    if ((scancode >= 0) && (scancode < kMaxScancodes)) {
      const bool down = (event->type == SDL_EVENT_KEY_DOWN);
      g_keyState[static_cast<std::size_t>(scancode)] = down;
      KeyEvent ke{};
      ke.scancode = scancode;
      ke.down = down;
      emit(ke);
    }
    break;
  }
  case SDL_EVENT_MOUSE_MOTION: {
    g_mouse.x = static_cast<int>(event->motion.x);
    g_mouse.y = static_cast<int>(event->motion.y);
    g_mouse.deltaX += static_cast<int>(event->motion.xrel);
    g_mouse.deltaY += static_cast<int>(event->motion.yrel);
    MouseMoveEvent me{};
    me.x = static_cast<int>(event->motion.x);
    me.y = static_cast<int>(event->motion.y);
    me.deltaX = static_cast<int>(event->motion.xrel);
    me.deltaY = static_cast<int>(event->motion.yrel);
    emit(me);
    break;
  }
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    const int button = static_cast<int>(event->button.button) - 1;
    if ((button >= 0) && (button < kMaxMouseButtons)) {
      const bool down = (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      g_mouse.buttons[static_cast<std::size_t>(button)] = down;
      MouseButtonEvent mbe{};
      mbe.button = button;
      mbe.down = down;
      emit(mbe);
    }
    break;
  }
  case SDL_EVENT_MOUSE_WHEEL:
    g_mouse.scrollDelta += static_cast<int>(event->wheel.y);
    break;
  case SDL_EVENT_GAMEPAD_ADDED:
    attach_gamepad(static_cast<std::uint32_t>(event->gdevice.which));
    break;
  case SDL_EVENT_GAMEPAD_REMOVED:
    detach_gamepad(static_cast<std::uint32_t>(event->gdevice.which));
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_GAMEPAD_BUTTON_UP: {
    // A device that was never announced (or was removed) has no slot;
    // its late events are dropped rather than applied to another slot.
    GamepadStateInternal *slot =
        find_gamepad(static_cast<std::uint32_t>(event->gbutton.which));
    const int button = static_cast<int>(event->gbutton.button);
    if ((slot != nullptr) && (button >= 0) && (button < kMaxGamepadButtons)) {
      slot->buttons[static_cast<std::size_t>(button)] =
          (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    }
    break;
  }
  case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
    GamepadStateInternal *slot =
        find_gamepad(static_cast<std::uint32_t>(event->gaxis.which));
    const int axis = static_cast<int>(event->gaxis.axis);
    if ((slot != nullptr) && (axis >= 0) && (axis < kMaxGamepadAxes)) {
      slot->axes[static_cast<std::size_t>(axis)] = event->gaxis.value;
    }
    break;
  }
  default:
    break;
  }

  input_mapper_process_event(nativeEvent);
  touch_process_event(nativeEvent);
}

/// Ends the input frame for the mapper and touch subsystems; keyboard and
/// mouse state is maintained per-event and needs no end-of-frame sync.
void end_input_frame() noexcept {
  input_mapper_end_frame();
  touch_end_frame();
}

/// Returns whether is key down.
bool is_key_down(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  return g_keyState[static_cast<std::size_t>(scancode)];
}

/// Returns whether is key pressed.
bool is_key_pressed(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  const auto idx = static_cast<std::size_t>(scancode);
  return g_keyState[idx] && !g_prevKeyState[idx];
}

/// Returns whether is key released.
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

/// Returns whether is mouse button down.
bool is_mouse_button_down(int button) noexcept {
  if ((button < 0) || (button >= kMaxMouseButtons)) {
    return false;
  }
  return g_mouse.buttons[static_cast<std::size_t>(button)];
}

/// Returns whether is mouse button pressed.
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

/// Returns whether is action down.
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

/// Returns whether is action pressed.
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

bool is_gamepad_connected(int gamepad) noexcept {
  return gamepad_slot(gamepad) != nullptr;
}

int connected_gamepad_count() noexcept {
  int count = 0;
  for (const GamepadStateInternal &slot : g_gamepads) {
    count += slot.connected ? 1 : 0;
  }
  return count;
}

bool is_gamepad_button_down(int button, int gamepad) noexcept {
  const GamepadStateInternal *slot = gamepad_slot(gamepad);
  if ((slot == nullptr) || (button < 0) || (button >= kMaxGamepadButtons)) {
    return false;
  }
  return slot->buttons[static_cast<std::size_t>(button)];
}

float gamepad_axis_value(int axis, int deadzone, int gamepad) noexcept {
  const GamepadStateInternal *slot = gamepad_slot(gamepad);
  if ((slot == nullptr) || (axis < 0) || (axis >= kMaxGamepadAxes)) {
    return 0.0F;
  }

  const int raw = static_cast<int>(slot->axes[static_cast<std::size_t>(axis)]);
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
