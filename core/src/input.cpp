// Implements input behavior for the Engine core engine.

#include "engine/core/input.h"
#include "engine/core/platform_event.h"
#include "engine/core/input_map.h"
#include "engine/core/touch_input.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/event_bus.h"
#include "engine/core/logging.h"

namespace engine::core {

namespace {

constexpr int kMaxScancodes = kMaxKeyCode + 1;
constexpr int kMaxMouseButtons = 5;
constexpr int kMaxGamepadButtons = 16;
constexpr int kMaxGamepadAxes = 6;

bool g_inputInitialized = false;

std::array<bool, kMaxScancodes> g_keyState{};
// Edges recorded from the events themselves and cleared when a frame
// begins. Comparing a frame's end state with the previous frame's cannot
// see a press and release that both land inside one frame -- the key is up
// at both ends -- so a quick tap at a low frame rate was lost.
std::array<bool, kMaxScancodes> g_keyPressedEdge{};
std::array<bool, kMaxScancodes> g_keyReleasedEdge{};

struct MouseStateInternal final {
  int x = 0;
  int y = 0;
  int deltaX = 0;
  int deltaY = 0;
  int scrollDelta = 0;
  std::array<bool, kMaxMouseButtons> buttons{};
  std::array<bool, kMaxMouseButtons> pressedEdge{};
};

MouseStateInternal g_mouse{};
// Fraction of a wheel notch not yet reported.
float g_wheelCarry = 0.0F;

/// One controller slot, keyed to the platform instance id it was announced
/// under so a second controller's events never land on the first.
struct GamepadStateInternal final {
  bool connected = false;
  std::uint32_t instanceId = 0U;
  std::array<bool, kMaxGamepadButtons> buttons{};
  std::array<bool, kMaxGamepadButtons> pressedEdge{};
  std::array<std::int16_t, kMaxGamepadAxes> axes{};
};

std::array<GamepadStateInternal, static_cast<std::size_t>(kMaxGamepads)>
    g_gamepads{};


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

} // namespace

/// Initializes the owning system for input. Persisted per-user rebindings
/// are restored here when the file exists, and take precedence over script
/// defaults registered later.
bool initialize_input() noexcept {
  if (g_inputInitialized) {
    return true;
  }

  g_keyState = {};
  g_keyPressedEdge = {};
  g_keyReleasedEdge = {};
  g_mouse = {};
  g_wheelCarry = 0.0F;
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
/// persisted input map stay untouched.
void clear_gameplay_bindings() noexcept {
  clear_unpersisted_input_mappings();
  clear_action_callbacks();
  clear_touch_callbacks();
}

/// Live script-registered actions (teardown-regression introspection).
std::size_t gameplay_action_count() noexcept {
  return unpersisted_input_action_count();
}

/// Live script-registered axes (teardown-regression introspection).
std::size_t gameplay_axis_count() noexcept {
  return unpersisted_input_axis_count();
}

void shutdown_input() noexcept {
  shutdown_touch_input();
  shutdown_input_mapper();
  g_inputInitialized = false;
  g_keyState = {};
  g_keyPressedEdge = {};
  g_keyReleasedEdge = {};
  g_mouse = {};
  g_wheelCarry = 0.0F;
  g_gamepads = {};
}

/// Begins the requested operation or profiling range for input frame.
void begin_input_frame() noexcept {
  g_keyPressedEdge = {};
  g_keyReleasedEdge = {};
  g_mouse.pressedEdge = {};
  for (GamepadStateInternal &pad : g_gamepads) {
    pad.pressedEdge = {};
  }
  g_mouse.deltaX = 0;
  g_mouse.deltaY = 0;
  g_mouse.scrollDelta = 0;
  input_mapper_begin_frame();
  touch_begin_frame();
}

namespace {

/// Releases everything the window can no longer see being released.
///
/// Losing focus stops key, mouse and -- in a windowed run, where SDL does
/// not deliver background controller events -- gamepad events. Anything
/// held at that moment would otherwise stay down until the same input
/// happened to be pressed and released again after focus returned: the
/// Alt-Tab-while-walking character that keeps walking.
///
/// Released rather than silently cleared: each held key records a release
/// edge, so is_key_released and the mapper's release callbacks fire
/// exactly as for a real release, and each
/// key and button emits its up event so event-bus listeners that track
/// held state see the same edge. A charge-and-release mechanic resolves
/// instead of hanging.
void release_all_held_input() noexcept {
  for (std::size_t i = 0U; i < g_keyState.size(); ++i) {
    if (g_keyState[i]) {
      g_keyState[i] = false;
      g_keyReleasedEdge[i] = true;
      KeyEvent ke{};
      ke.scancode = static_cast<int>(i);
      ke.down = false;
      emit(ke);
    }
  }
  for (std::size_t i = 0U; i < g_mouse.buttons.size(); ++i) {
    if (g_mouse.buttons[i]) {
      g_mouse.buttons[i] = false;
      MouseButtonEvent mbe{};
      mbe.button = static_cast<int>(i);
      mbe.down = false;
      emit(mbe);
    }
  }
  // Sticks centre as well as buttons release: a stick held left when
  // focus went would otherwise keep steering.
  for (GamepadStateInternal &pad : g_gamepads) {
    pad.buttons = {};
    pad.axes = {};
  }
}

} // namespace

void input_process_event(const PlatformEvent &event) noexcept {
  switch (event.kind) {
  case PlatformEventKind::KeyDown:
  case PlatformEventKind::KeyUp: {
    const int scancode = event.scancode;
    if ((scancode >= 0) && (scancode < kMaxScancodes)) {
      const auto idx = static_cast<std::size_t>(scancode);
      const bool down = (event.kind == PlatformEventKind::KeyDown);
      // OS auto-repeat arrives as KeyDown on a key already held; it is not
      // a new press.
      if (down && !event.repeat && !g_keyState[idx]) {
        g_keyPressedEdge[idx] = true;
      } else if (!down && g_keyState[idx]) {
        g_keyReleasedEdge[idx] = true;
      }
      g_keyState[idx] = down;
      KeyEvent ke{};
      ke.scancode = scancode;
      ke.down = down;
      emit(ke);
    }
    break;
  }
  case PlatformEventKind::MouseMove: {
    g_mouse.x = static_cast<int>(event.x);
    g_mouse.y = static_cast<int>(event.y);
    g_mouse.deltaX += static_cast<int>(event.deltaX);
    g_mouse.deltaY += static_cast<int>(event.deltaY);
    MouseMoveEvent me{};
    me.x = static_cast<int>(event.x);
    me.y = static_cast<int>(event.y);
    me.deltaX = static_cast<int>(event.deltaX);
    me.deltaY = static_cast<int>(event.deltaY);
    emit(me);
    break;
  }
  case PlatformEventKind::MouseButtonDown:
  case PlatformEventKind::MouseButtonUp: {
    // A button event carries the cursor too: a touch-emulated tap is a
    // press and release with no motion between, so the position must
    // land here or the press reads a stale cursor.
    g_mouse.x = static_cast<int>(event.x);
    g_mouse.y = static_cast<int>(event.y);
    const int button = event.mouseButton;
    if ((button >= 0) && (button < kMaxMouseButtons)) {
      const auto idx = static_cast<std::size_t>(button);
      const bool down = (event.kind == PlatformEventKind::MouseButtonDown);
      if (down && !g_mouse.buttons[idx]) {
        g_mouse.pressedEdge[idx] = true;
      }
      g_mouse.buttons[idx] = down;
      MouseButtonEvent mbe{};
      mbe.button = button;
      mbe.down = down;
      emit(mbe);
    }
    break;
  }
  case PlatformEventKind::MouseWheel: {
    // Precise trackpads scroll in fractions of a notch; the fraction is
    // carried across events so it is counted once it adds up to a notch
    // instead of truncating to nothing.
    g_wheelCarry += event.wheelY;
    const int notches = static_cast<int>(g_wheelCarry);
    g_mouse.scrollDelta += notches;
    g_wheelCarry -= static_cast<float>(notches);
    break;
  }
  case PlatformEventKind::WindowFocusLost:
    release_all_held_input();
    break;
  case PlatformEventKind::GamepadAdded:
    attach_gamepad(event.deviceId);
    break;
  case PlatformEventKind::GamepadRemoved:
    detach_gamepad(event.deviceId);
    break;
  case PlatformEventKind::GamepadButtonDown:
  case PlatformEventKind::GamepadButtonUp: {
    // A device that was never announced (or was removed) has no slot;
    // its late events are dropped rather than applied to another slot.
    GamepadStateInternal *slot = find_gamepad(event.deviceId);
    const int button = event.gamepadButton;
    if ((slot != nullptr) && (button >= 0) && (button < kMaxGamepadButtons)) {
      const auto idx = static_cast<std::size_t>(button);
      const bool down = (event.kind == PlatformEventKind::GamepadButtonDown);
      if (down && !slot->buttons[idx]) {
        slot->pressedEdge[idx] = true;
      }
      slot->buttons[idx] = down;
    }
    break;
  }
  case PlatformEventKind::GamepadAxis: {
    GamepadStateInternal *slot = find_gamepad(event.deviceId);
    const int axis = event.gamepadAxis;
    if ((slot != nullptr) && (axis >= 0) && (axis < kMaxGamepadAxes)) {
      slot->axes[static_cast<std::size_t>(axis)] = event.axisValue;
    }
    break;
  }
  default:
    break;
  }

  input_mapper_process_event(event);
  touch_process_event(event);
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
  return g_keyPressedEdge[static_cast<std::size_t>(scancode)];
}

/// Returns whether is key released.
bool is_key_released(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  return g_keyReleasedEdge[static_cast<std::size_t>(scancode)];
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
  return g_mouse.pressedEdge[static_cast<std::size_t>(button)];
}

// The legacy action and axis calls are a thin layer over the input
// mapper, so there is one registry: an action a script registers here is
// the one add_input_action, rebinding and the bindings document see, and a
// persisted binding outranks a script default either way.

bool register_action(const char *name, KeyScancode key,
                     int mouseButton) noexcept {
  InputBinding bindings[2] = {};
  std::uint32_t count = 0U;
  if (key >= 0) {
    bindings[count].type = InputBindingType::Key;
    bindings[count].code = key;
    ++count;
  }
  if (mouseButton >= 0) {
    bindings[count].type = InputBindingType::MouseButton;
    bindings[count].code = mouseButton;
    ++count;
  }
  return add_input_action(name, bindings, count);
}

bool is_action_down(const char *name) noexcept {
  return is_mapped_action_down(name);
}

bool is_action_pressed(const char *name) noexcept {
  return is_mapped_action_pressed(name);
}

float action_value(const char *name) noexcept {
  return is_mapped_action_down(name) ? 1.0F : 0.0F;
}

bool register_axis(const char *name, KeyScancode negativeKey,
                   KeyScancode positiveKey) noexcept {
  InputAxisSource source{};
  source.type = AxisSourceType::KeyPair;
  source.negativeKey = negativeKey;
  source.positiveKey = positiveKey;
  return add_input_axis(name, &source, 1U);
}

float axis_value(const char *name) noexcept {
  return mapped_axis_value(name);
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

bool is_gamepad_button_pressed(int button, int gamepad) noexcept {
  const GamepadStateInternal *slot = gamepad_slot(gamepad);
  if ((slot == nullptr) || (button < 0) || (button >= kMaxGamepadButtons)) {
    return false;
  }
  return slot->pressedEdge[static_cast<std::size_t>(button)];
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
