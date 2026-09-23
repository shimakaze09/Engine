// Implements input behavior for the Engine core engine.

#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/platform_event.h"
#include "engine/core/touch_input.h"
#include "input_steps_internal.h"

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

// ----- Fixed-step snapshots ------------------------------------------------

constexpr std::size_t kKeyWords =
    (static_cast<std::size_t>(kMaxScancodes) + 63U) / 64U;

/// One fixed step's view of the devices: what was down when the step ended
/// and what went down or up during it. Plain data, rebuilt per step.
struct InputFrame final {
  std::array<std::uint64_t, kKeyWords> keyDown{};
  std::array<std::uint64_t, kKeyWords> keyPressed{};
  std::array<std::uint64_t, kKeyWords> keyReleased{};
  int mouseX = 0;
  int mouseY = 0;
  int mouseDeltaX = 0;
  int mouseDeltaY = 0;
  int scrollDelta = 0;
  std::uint8_t mouseDown = 0U;
  std::uint8_t mousePressed = 0U;
  std::uint8_t gamepadConnected = 0U;
  std::array<std::uint16_t, static_cast<std::size_t>(kMaxGamepads)>
      gamepadDown{};
  std::array<std::uint16_t, static_cast<std::size_t>(kMaxGamepads)>
      gamepadPressed{};
  std::array<std::array<std::int16_t, kMaxGamepadAxes>,
             static_cast<std::size_t>(kMaxGamepads)>
      gamepadAxes{};
};

bool bit(const std::array<std::uint64_t, kKeyWords> &bits,
         std::size_t index) noexcept {
  return ((bits[index / 64U] >> (index % 64U)) & 1U) != 0U;
}

void set_bit(std::array<std::uint64_t, kKeyWords> &bits, std::size_t index,
             bool value) noexcept {
  const std::uint64_t mask = std::uint64_t{1} << (index % 64U);
  bits[index / 64U] =
      value ? (bits[index / 64U] | mask) : (bits[index / 64U] & ~mask);
}

enum class StepEventType : std::uint8_t {
  Key,
  MouseMove,
  MouseButton,
  MouseWheel,
  GamepadConnected,
  GamepadButton,
  GamepadAxis,
};

/// An event as the steps replay it, recorded after the live state applied
/// it, so gamepad events already carry their slot and a wheel event its
/// whole notches.
struct StepEvent final {
  std::uint64_t timestampNs = 0U;
  StepEventType type = StepEventType::Key;
  bool down = false;
  std::uint8_t slot = 0U;
  std::int16_t code = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t deltaX = 0;
  std::int32_t deltaY = 0;
};

// Events past this many before steps consume them are not recorded; the
// last step's reconciliation with the live state keeps what is held right.
constexpr std::size_t kMaxStepEvents = 512U;
std::array<StepEvent, kMaxStepEvents> g_stepEvents =
    std::array<StepEvent, kMaxStepEvents>();
std::size_t g_stepEventCount = 0U;
bool g_stepEventsOverflowed = false;
// The recorded events span [g_stepWindowStartNs, g_stepWindowEndNs]: from
// the pump that followed the last steps to the latest pump.
std::uint64_t g_stepWindowStartNs = 0U;
std::uint64_t g_stepWindowEndNs = 0U;

InputFrame g_stepHeld{};     // the last step's snapshot
InputFrame g_stepCurrent{};  // the snapshot queries answer from
InputFrame g_stepPrevious{}; // the step before the current one
const InputFrame *g_activeFrame = nullptr;
std::uint32_t g_stepCount = 0U;
std::uint32_t g_stepNext = 0U;
std::size_t g_stepCursor = 0U;

void record_step_event(const StepEvent &event) noexcept {
  // Consecutive motion folds into one record: it is the common case and
  // the only one that could fill the table on its own.
  if ((event.type == StepEventType::MouseMove) && (g_stepEventCount > 0U) &&
      (g_stepEvents[g_stepEventCount - 1U].type == StepEventType::MouseMove)) {
    StepEvent &last = g_stepEvents[g_stepEventCount - 1U];
    last.timestampNs = event.timestampNs;
    last.x = event.x;
    last.y = event.y;
    last.deltaX += event.deltaX;
    last.deltaY += event.deltaY;
    return;
  }
  if (g_stepEventCount >= kMaxStepEvents) {
    if (!g_stepEventsOverflowed) {
      g_stepEventsOverflowed = true;
      log_message(LogLevel::Warning, "input",
                  "more input events arrived between fixed steps than can "
                  "be split across them; the rest land on the last step");
    }
    return;
  }
  g_stepEvents[g_stepEventCount++] = event;
}

/// The live device state as a snapshot with no edges.
InputFrame live_frame() noexcept {
  InputFrame frame{};
  for (std::size_t i = 0U; i < g_keyState.size(); ++i) {
    set_bit(frame.keyDown, i, g_keyState[i]);
  }
  frame.mouseX = g_mouse.x;
  frame.mouseY = g_mouse.y;
  for (std::size_t i = 0U; i < g_mouse.buttons.size(); ++i) {
    if (g_mouse.buttons[i]) {
      frame.mouseDown = static_cast<std::uint8_t>(frame.mouseDown | (1U << i));
    }
  }
  for (std::size_t p = 0U; p < g_gamepads.size(); ++p) {
    const GamepadStateInternal &pad = g_gamepads[p];
    if (!pad.connected) {
      continue;
    }
    frame.gamepadConnected =
        static_cast<std::uint8_t>(frame.gamepadConnected | (1U << p));
    for (std::size_t b = 0U; b < pad.buttons.size(); ++b) {
      if (pad.buttons[b]) {
        frame.gamepadDown[p] =
            static_cast<std::uint16_t>(frame.gamepadDown[p] | (1U << b));
      }
    }
    frame.gamepadAxes[p] = pad.axes;
  }
  return frame;
}

/// Applies one recorded event to a step's snapshot, recording its edges.
void apply_step_event(InputFrame &frame, const StepEvent &event) noexcept {
  switch (event.type) {
  case StepEventType::Key: {
    const auto idx = static_cast<std::size_t>(event.code);
    if (event.down && !bit(frame.keyDown, idx)) {
      set_bit(frame.keyPressed, idx, true);
    } else if (!event.down && bit(frame.keyDown, idx)) {
      set_bit(frame.keyReleased, idx, true);
    }
    set_bit(frame.keyDown, idx, event.down);
    break;
  }
  case StepEventType::MouseMove:
    frame.mouseX = event.x;
    frame.mouseY = event.y;
    frame.mouseDeltaX += event.deltaX;
    frame.mouseDeltaY += event.deltaY;
    break;
  case StepEventType::MouseButton: {
    const auto mask = static_cast<std::uint8_t>(1U << event.code);
    frame.mouseX = event.x;
    frame.mouseY = event.y;
    if (event.down && ((frame.mouseDown & mask) == 0U)) {
      frame.mousePressed = static_cast<std::uint8_t>(frame.mousePressed | mask);
    }
    frame.mouseDown = event.down
                          ? static_cast<std::uint8_t>(frame.mouseDown | mask)
                          : static_cast<std::uint8_t>(frame.mouseDown & ~mask);
    break;
  }
  case StepEventType::MouseWheel:
    frame.scrollDelta += event.deltaY;
    break;
  case StepEventType::GamepadConnected: {
    const auto mask = static_cast<std::uint8_t>(1U << event.slot);
    frame.gamepadConnected =
        event.down ? static_cast<std::uint8_t>(frame.gamepadConnected | mask)
                   : static_cast<std::uint8_t>(frame.gamepadConnected & ~mask);
    // A slot starts and ends empty, as the live slot does.
    frame.gamepadDown[event.slot] = 0U;
    frame.gamepadAxes[event.slot] = {};
    break;
  }
  case StepEventType::GamepadButton: {
    const auto mask = static_cast<std::uint16_t>(1U << event.code);
    std::uint16_t &down = frame.gamepadDown[event.slot];
    if (event.down && ((down & mask) == 0U)) {
      frame.gamepadPressed[event.slot] =
          static_cast<std::uint16_t>(frame.gamepadPressed[event.slot] | mask);
    }
    down = event.down ? static_cast<std::uint16_t>(down | mask)
                      : static_cast<std::uint16_t>(down & ~mask);
    break;
  }
  case StepEventType::GamepadAxis:
    frame.gamepadAxes[event.slot][static_cast<std::size_t>(event.code)] =
        static_cast<std::int16_t>(event.x);
    break;
  }
}

/// Makes the last step end in the live state. Whatever differs is a
/// change the recorded events did not carry -- focus loss, or events past
/// the record capacity -- and is applied here, with its edge.
void reconcile_with_live(InputFrame &frame) noexcept {
  const InputFrame live = live_frame();
  for (std::size_t i = 0U; i < static_cast<std::size_t>(kMaxScancodes); ++i) {
    const bool was = bit(frame.keyDown, i);
    const bool now = bit(live.keyDown, i);
    if (now && !was) {
      set_bit(frame.keyPressed, i, true);
    } else if (!now && was) {
      set_bit(frame.keyReleased, i, true);
    }
  }
  frame.keyDown = live.keyDown;
  frame.mousePressed = static_cast<std::uint8_t>(
      frame.mousePressed | (live.mouseDown & ~frame.mouseDown));
  frame.mouseDown = live.mouseDown;
  frame.mouseX = live.mouseX;
  frame.mouseY = live.mouseY;
  for (std::size_t p = 0U; p < frame.gamepadDown.size(); ++p) {
    frame.gamepadPressed[p] = static_cast<std::uint16_t>(
        frame.gamepadPressed[p] |
        (live.gamepadDown[p] & ~frame.gamepadDown[p]));
  }
  frame.gamepadDown = live.gamepadDown;
  frame.gamepadAxes = live.gamepadAxes;
  frame.gamepadConnected = live.gamepadConnected;
}

/// The step, out of `stepCount`, whose share of the window holds `ns`.
std::uint32_t step_for(std::uint64_t ns, std::uint32_t stepCount) noexcept {
  if ((stepCount <= 1U) || (ns <= g_stepWindowStartNs)) {
    return 0U;
  }
  // Past the window's end goes last even when the window has no length:
  // two clock reads can return the same tick, and an event stamped after
  // both still happened after everything before it.
  if (ns >= g_stepWindowEndNs) {
    return stepCount - 1U;
  }
  const std::uint64_t span = g_stepWindowEndNs - g_stepWindowStartNs;
  const std::uint64_t offset = ns - g_stepWindowStartNs;
  // offset < span, so the product fits unless a window spans centuries.
  const auto step = static_cast<std::uint32_t>((offset * stepCount) / span);
  return (step < stepCount) ? step : (stepCount - 1U);
}

/// Clears the recorded events and restarts the window at the latest pump.
void consume_step_events() noexcept {
  g_stepEventCount = 0U;
  g_stepEventsOverflowed = false;
  g_stepWindowStartNs = g_stepWindowEndNs;
}

void reset_step_state() noexcept {
  g_stepEventCount = 0U;
  g_stepEventsOverflowed = false;
  g_stepWindowStartNs = 0U;
  g_stepWindowEndNs = 0U;
  g_stepHeld = InputFrame{};
  g_stepCurrent = InputFrame{};
  g_stepPrevious = InputFrame{};
  g_activeFrame = nullptr;
  g_stepCount = 0U;
  g_stepNext = 0U;
  g_stepCursor = 0U;
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
  reset_step_state();
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
  reset_step_state();
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
      if (!(down && event.repeat)) {
        StepEvent step{};
        step.timestampNs = event.timestampNs;
        step.type = StepEventType::Key;
        step.down = down;
        step.code = static_cast<std::int16_t>(scancode);
        record_step_event(step);
      }
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
    {
      StepEvent step{};
      step.timestampNs = event.timestampNs;
      step.type = StepEventType::MouseMove;
      step.x = g_mouse.x;
      step.y = g_mouse.y;
      step.deltaX = static_cast<std::int32_t>(event.deltaX);
      step.deltaY = static_cast<std::int32_t>(event.deltaY);
      record_step_event(step);
    }
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
      StepEvent step{};
      step.timestampNs = event.timestampNs;
      step.type = StepEventType::MouseButton;
      step.down = down;
      step.code = static_cast<std::int16_t>(button);
      step.x = g_mouse.x;
      step.y = g_mouse.y;
      record_step_event(step);
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
    if (notches != 0) {
      StepEvent step{};
      step.timestampNs = event.timestampNs;
      step.type = StepEventType::MouseWheel;
      step.deltaY = notches;
      record_step_event(step);
    }
    break;
  }
  case PlatformEventKind::WindowFocusLost:
    release_all_held_input();
    break;
  case PlatformEventKind::GamepadAdded:
  case PlatformEventKind::GamepadRemoved: {
    const bool added = (event.kind == PlatformEventKind::GamepadAdded);
    const GamepadStateInternal *before = find_gamepad(event.deviceId);
    if (added) {
      attach_gamepad(event.deviceId);
    } else {
      detach_gamepad(event.deviceId);
    }
    const GamepadStateInternal *slot =
        added ? find_gamepad(event.deviceId) : before;
    if (slot != nullptr) {
      StepEvent step{};
      step.timestampNs = event.timestampNs;
      step.type = StepEventType::GamepadConnected;
      step.down = added;
      step.slot = static_cast<std::uint8_t>(slot - g_gamepads.data());
      record_step_event(step);
    }
    break;
  }
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
      StepEvent step{};
      step.timestampNs = event.timestampNs;
      step.type = StepEventType::GamepadButton;
      step.down = down;
      step.slot = static_cast<std::uint8_t>(slot - g_gamepads.data());
      step.code = static_cast<std::int16_t>(button);
      record_step_event(step);
    }
    break;
  }
  case PlatformEventKind::GamepadAxis: {
    GamepadStateInternal *slot = find_gamepad(event.deviceId);
    const int axis = event.gamepadAxis;
    if ((slot != nullptr) && (axis >= 0) && (axis < kMaxGamepadAxes)) {
      slot->axes[static_cast<std::size_t>(axis)] = event.axisValue;
      StepEvent step{};
      step.timestampNs = event.timestampNs;
      step.type = StepEventType::GamepadAxis;
      step.slot = static_cast<std::uint8_t>(slot - g_gamepads.data());
      step.code = static_cast<std::int16_t>(axis);
      step.x = event.axisValue;
      record_step_event(step);
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
  // Every event this pump delivered happened before now, so now closes the
  // window the next steps split.
  g_stepWindowEndNs = platform_ticks_ns();
  if (g_stepWindowStartNs == 0U) {
    g_stepWindowStartNs = g_stepWindowEndNs;
  }
}

void begin_input_steps(std::uint32_t stepCount) noexcept {
  g_stepCount = stepCount;
  g_stepNext = 0U;
  g_stepCursor = 0U;
}

bool advance_input_step() noexcept {
  if (g_stepNext >= g_stepCount) {
    return false;
  }
  const std::uint32_t step = g_stepNext++;
  const bool last = (g_stepNext == g_stepCount);
  g_stepPrevious = g_stepHeld;
  InputFrame frame = g_stepHeld;
  frame.keyPressed = {};
  frame.keyReleased = {};
  frame.mouseDeltaX = 0;
  frame.mouseDeltaY = 0;
  frame.scrollDelta = 0;
  frame.mousePressed = 0U;
  frame.gamepadPressed = {};
  while ((g_stepCursor < g_stepEventCount) &&
         (last || (step_for(g_stepEvents[g_stepCursor].timestampNs,
                            g_stepCount) <= step))) {
    apply_step_event(frame, g_stepEvents[g_stepCursor]);
    ++g_stepCursor;
  }
  if (last) {
    reconcile_with_live(frame);
    consume_step_events();
  }
  g_stepHeld = frame;
  g_stepCurrent = frame;
  g_activeFrame = &g_stepCurrent;
  return true;
}

void end_input_steps() noexcept {
  g_activeFrame = nullptr;
  g_stepCount = 0U;
  g_stepNext = 0U;
}

void reset_input_steps() noexcept {
  g_stepEventCount = 0U;
  g_stepEventsOverflowed = false;
  g_stepWindowStartNs = g_stepWindowEndNs;
  g_stepHeld = live_frame();
  g_stepPrevious = g_stepHeld;
  g_activeFrame = nullptr;
  g_stepCount = 0U;
  g_stepNext = 0U;
}

bool input_step_current() noexcept { return g_activeFrame != nullptr; }

void input_step_use_previous(bool previous) noexcept {
  if (g_activeFrame != nullptr) {
    g_activeFrame = previous ? &g_stepPrevious : &g_stepCurrent;
  }
}

/// Returns whether is key down.
bool is_key_down(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  if (g_activeFrame != nullptr) {
    return bit(g_activeFrame->keyDown, static_cast<std::size_t>(scancode));
  }
  return g_keyState[static_cast<std::size_t>(scancode)];
}

/// Returns whether is key pressed.
bool is_key_pressed(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  if (g_activeFrame != nullptr) {
    return bit(g_activeFrame->keyPressed, static_cast<std::size_t>(scancode));
  }
  return g_keyPressedEdge[static_cast<std::size_t>(scancode)];
}

/// Returns whether is key released.
bool is_key_released(KeyScancode scancode) noexcept {
  if ((scancode < 0) || (scancode >= kMaxScancodes)) {
    return false;
  }
  if (g_activeFrame != nullptr) {
    return bit(g_activeFrame->keyReleased, static_cast<std::size_t>(scancode));
  }
  return g_keyReleasedEdge[static_cast<std::size_t>(scancode)];
}

MouseState mouse_state() noexcept {
  MouseState state{};
  if (g_activeFrame != nullptr) {
    state.x = g_activeFrame->mouseX;
    state.y = g_activeFrame->mouseY;
    state.deltaX = g_activeFrame->mouseDeltaX;
    state.deltaY = g_activeFrame->mouseDeltaY;
    state.scrollDelta = g_activeFrame->scrollDelta;
    for (int i = 0; i < kMaxMouseButtons; ++i) {
      state.buttons[i] = ((g_activeFrame->mouseDown >> i) & 1U) != 0U;
    }
    return state;
  }
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
  if (g_activeFrame != nullptr) {
    return ((g_activeFrame->mouseDown >> button) & 1U) != 0U;
  }
  return g_mouse.buttons[static_cast<std::size_t>(button)];
}

/// Returns whether is mouse button pressed.
bool is_mouse_button_pressed(int button) noexcept {
  if ((button < 0) || (button >= kMaxMouseButtons)) {
    return false;
  }
  if (g_activeFrame != nullptr) {
    return ((g_activeFrame->mousePressed >> button) & 1U) != 0U;
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
  if ((g_activeFrame != nullptr) && (gamepad >= 0) &&
      (gamepad < kMaxGamepads)) {
    return ((g_activeFrame->gamepadConnected >> gamepad) & 1U) != 0U;
  }
  return gamepad_slot(gamepad) != nullptr;
}

int connected_gamepad_count() noexcept {
  int count = 0;
  if (g_activeFrame != nullptr) {
    for (int p = 0; p < kMaxGamepads; ++p) {
      count += ((g_activeFrame->gamepadConnected >> p) & 1U);
    }
    return count;
  }
  for (const GamepadStateInternal &slot : g_gamepads) {
    count += slot.connected ? 1 : 0;
  }
  return count;
}

bool is_gamepad_button_down(int button, int gamepad) noexcept {
  if (g_activeFrame != nullptr) {
    return is_gamepad_connected(gamepad) && (button >= 0) &&
           (button < kMaxGamepadButtons) &&
           (((g_activeFrame->gamepadDown[static_cast<std::size_t>(gamepad)] >>
              button) &
             1U) != 0U);
  }
  const GamepadStateInternal *slot = gamepad_slot(gamepad);
  if ((slot == nullptr) || (button < 0) || (button >= kMaxGamepadButtons)) {
    return false;
  }
  return slot->buttons[static_cast<std::size_t>(button)];
}

bool is_gamepad_button_pressed(int button, int gamepad) noexcept {
  if (g_activeFrame != nullptr) {
    return is_gamepad_connected(gamepad) && (button >= 0) &&
           (button < kMaxGamepadButtons) &&
           (((g_activeFrame
                  ->gamepadPressed[static_cast<std::size_t>(gamepad)] >>
              button) &
             1U) != 0U);
  }
  const GamepadStateInternal *slot = gamepad_slot(gamepad);
  if ((slot == nullptr) || (button < 0) || (button >= kMaxGamepadButtons)) {
    return false;
  }
  return slot->pressedEdge[static_cast<std::size_t>(button)];
}

float gamepad_axis_value(int axis, int deadzone, int gamepad) noexcept {
  if (!is_gamepad_connected(gamepad) || (axis < 0) ||
      (axis >= kMaxGamepadAxes)) {
    return 0.0F;
  }
  const std::array<std::int16_t, kMaxGamepadAxes> &axes =
      (g_activeFrame != nullptr)
          ? g_activeFrame->gamepadAxes[static_cast<std::size_t>(gamepad)]
          : gamepad_slot(gamepad)->axes;
  const int raw = static_cast<int>(axes[static_cast<std::size_t>(axis)]);
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
