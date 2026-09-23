// Declares input types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/input_map.h"
#include "engine/core/platform.h"

namespace engine::core {

struct PlatformEvent;

// A physical key, named by its USB HID keyboard usage ID (HID Usage Tables,
// Keyboard/Keypad page 0x07): the engine's own key vocabulary, defined by
// that standard rather than by whichever platform library is underneath.
// It is what input_bindings.json persists and what Lua's engine.KEY_*
// carries. The platform translates native key codes into it; a native key
// with no usage ID on the page never reaches input as a key.
using KeyScancode = int;
/// The highest usage ID on the keyboard page (Right GUI).
inline constexpr KeyScancode kMaxKeyCode = 0xE7;

// ----- Lifecycle -----------------------------------------------------------

bool initialize_input() noexcept;
/// Shuts down the owning system for input.
void shutdown_input() noexcept;

// Called once per frame around the platform event loop.
void begin_input_frame() noexcept;
/// Applies one platform event to key, mouse and gamepad state, then hands
/// it to the action mapper and touch. Every event the pump polls goes
/// through here unless the editor captured it.
void input_process_event(const PlatformEvent &event) noexcept;
/// Ends the requested operation or profiling range for input frame.
void end_input_frame() noexcept;

// ----- Fixed steps ---------------------------------------------------------
// A frame that simulates several fixed steps gives each its own view of the
// input. The pump records every key, mouse and gamepad event with its
// timestamp; the steps split the time since the last steps ran evenly and
// each takes the events that fall in its share. A step's snapshot holds what
// was down when the step ended and what went down or up during it, so a tap
// inside one step is pressed in that step alone, and one held across steps
// is pressed in the first. The last step always ends in the live state, so a
// release the pump saw without an event (focus loss) or one past the record
// capacity is never lost. During a replay the steps read the input log
// instead (below).
//
// While a step is current, every query in this header, and the action
// mapper's, answers from its snapshot; touch stays per frame. Main thread
// only, like the pump.

/// Readies `stepCount` step snapshots from the events recorded since the
/// last steps ran. With zero steps the events carry over to the next call.
/// `firstTick` is the first step's SimulationClock::tickIndex before it
/// runs (the steps simulated ahead of it); the input log keys each step by
/// it.
void begin_input_steps(std::uint32_t stepCount,
                       std::uint64_t firstTick) noexcept;
/// Makes the next step's snapshot current. False when every step readied
/// by begin_input_steps has been taken.
bool advance_input_step() noexcept;
/// Queries answer from the live, per-frame state again.
void end_input_steps() noexcept;
/// Drops the recorded events and restarts the step snapshots from the live
/// state. For frames that simulate nothing -- stopped or paused play -- so
/// input from then is never replayed into the next step that runs.
void reset_input_steps() noexcept;

// ----- Recording and replay ------------------------------------------------
// Which step an event lands in follows its wall-clock timestamp, so the same
// key sequence can split differently across steps on two runs. The input log
// records what each step read instead -- its snapshot and the previous one
// the action mapper compares it with -- keyed by the step's tick, so a run
// replays step for step at any frame schedule. Replayed at one and at three
// steps a frame, a run recorded at three reaches the recorded
// World::state_hash at every tick both observe
// (engine_integration_input_replay).
//
// Only the step snapshots are recorded: what on_fixed_tick and the mapper
// read inside a step. Per-frame queries (on_tick, touch, the event bus)
// answer from the live devices during a replay, as they always do.
//
// The log is a versioned binary document (docs/architecture.md,
// "Serialization"). A recording streams through a fixed ring into a staged
// sibling temporary and replaces the destination only when it ends, so a
// recording that fails or is abandoned never touches an earlier log there.
// The pipeline ends both with the play session, whose ticks restart.

/// The one log revision this build reads and writes.
inline constexpr std::uint32_t kInputLogVersion = 1U;
/// Steps a recording holds in memory before it drains them to its staged
/// file, so a recorded step costs a copy and one step in this many a write.
/// Reaching it drains the ring; it limits nothing about the recording's
/// length.
inline constexpr std::size_t kInputLogRingCapacity = 256U;

/// Starts recording every fixed step to `path`. False, with nothing
/// changed, while a recording is open or when the staged file cannot be
/// created.
bool begin_input_recording(const char *path) noexcept;
/// Seals the recording and atomically replaces its destination. False when
/// no recording is open or a write failed, which leaves the destination as
/// it was.
bool end_input_recording() noexcept;
/// True while a recording is open and has not failed.
bool input_recording_active() noexcept;

/// Loads the log at `path` and makes the steps read it from then on in
/// place of the recorded events, starting with the step whose tick is the
/// log's first. The whole log is validated before anything changes, then
/// held in memory and decoded a step at a time. A missing, truncated,
/// malformed or other-version log, or one too large to hold, is refused
/// with a logged reason and changes nothing, a replay already running
/// included. A step whose tick is not the log's next ends the replay with
/// an error. The live steps are still built from the events throughout, so
/// the steps after a replay read the live devices as if it had never run,
/// with no event lost.
bool begin_input_replay(const char *path) noexcept;
/// Stops a replay; the next step reads the live devices.
void end_input_replay() noexcept;
/// True while a replay has steps left to give.
bool input_replay_active() noexcept;

// ----- Keyboard ------------------------------------------------------------

bool is_key_down(KeyScancode scancode) noexcept;
/// True in the frame the key went down, even if it came up again before
/// the frame ended. OS auto-repeat is not a press.
bool is_key_pressed(KeyScancode scancode) noexcept;
/// True in the frame the key came up, including a release that followed a
/// press inside the same frame.
bool is_key_released(KeyScancode scancode) noexcept;

// ----- Mouse ---------------------------------------------------------------

struct MouseState final {
  int x = 0;
  int y = 0;
  int deltaX = 0;
  int deltaY = 0;
  int scrollDelta = 0;
  bool buttons[5] = {};
};

/// Current mouse position and button state.
MouseState mouse_state() noexcept;
/// Returns whether is mouse button down.
bool is_mouse_button_down(int button) noexcept;
/// True in the frame the button went down, even if it came up again
/// before the frame ended.
bool is_mouse_button_pressed(int button) noexcept;

// ----- Action Mappings -----------------------------------------------------
// The script-facing shorthand for the input mapper (input_map.h): one
// registry, so a name registered here is the action add_input_action,
// rebinding and the bindings document see. A binding the user persisted
// outranks the default a script registers here.

inline constexpr std::size_t kMaxActions = kMaxInputActions;
inline constexpr std::size_t kMaxAxes = kMaxInputAxes;

/// Registers a default binding for a named action: a key, and optionally a
/// mouse button (-1 for none). Registering the name again replaces the
/// default. False when the name is empty, too long or the mapper is full.
bool register_action(const char *name, KeyScancode key,
                     int mouseButton = -1) noexcept;
/// Whether the action is active this frame (is_mapped_action_down).
bool is_action_down(const char *name) noexcept;
/// Whether the action became active this frame (is_mapped_action_pressed).
bool is_action_pressed(const char *name) noexcept;
/// 1 when the action is active, else 0.
float action_value(const char *name) noexcept;

/// Registers a default negative/positive key pair for a named axis.
bool register_axis(const char *name, KeyScancode negativeKey,
                   KeyScancode positiveKey) noexcept;
/// The axis value in [-1, 1] (mapped_axis_value).
float axis_value(const char *name) noexcept;

// Clears run-scoped gameplay registrations — script-registered actions and
// axes plus the action/touch callback tables that carry script-owned
// userData — while keeping device state and the persisted bindings.
// EnginePipeline::teardown calls it so no binding outlives its run.
void clear_gameplay_bindings() noexcept;

// Introspection: live action/axis registrations. Exercised by the
// pipeline-teardown regression; a correct teardown drains both to zero.
std::size_t gameplay_action_count() noexcept;
/// Live axis registrations (see gameplay_action_count).
std::size_t gameplay_axis_count() noexcept;

// ----- Gamepad ------------------------------------------------------------
// Up to kMaxGamepads controllers are tracked in the order they arrive;
// `gamepad` is that slot index, and slot 0 is the first controller, so
// single-controller callers omit it. A slot follows its device's hotplug
// arrival and removal (the platform opens and closes the device behind
// it), and its button and axis state is keyed to that device's instance
// id, so a second controller never aliases the first.

/// True while a device occupies the slot.
bool is_gamepad_connected(int gamepad = 0) noexcept;
/// Number of slots a device currently occupies.
int connected_gamepad_count() noexcept;
/// Returns whether is gamepad button down.
bool is_gamepad_button_down(int button, int gamepad = 0) noexcept;
/// True in the frame the button went down, even if it came up again
/// before the frame ended.
bool is_gamepad_button_pressed(int button, int gamepad = 0) noexcept;
// Returns normalized axis value in [-1, 1] with deadzone applied.
float gamepad_axis_value(int axis, int deadzone = 8000,
                         int gamepad = 0) noexcept;

// Gamepad button and axis codes: the engine's own vocabulary for scripts
// and persisted bindings. Their values match SDL_GAMEPAD_BUTTON_* and
// SDL_GAMEPAD_AXIS_* (pinned by static_asserts in the input backend) so a
// binding written before the names existed keeps its meaning.
// clang-format off
inline constexpr int kGamepadButton_South         =  0;
inline constexpr int kGamepadButton_East          =  1;
inline constexpr int kGamepadButton_West          =  2;
inline constexpr int kGamepadButton_North         =  3;
inline constexpr int kGamepadButton_Back          =  4;
inline constexpr int kGamepadButton_Guide         =  5;
inline constexpr int kGamepadButton_Start         =  6;
inline constexpr int kGamepadButton_LeftStick     =  7;
inline constexpr int kGamepadButton_RightStick    =  8;
inline constexpr int kGamepadButton_LeftShoulder  =  9;
inline constexpr int kGamepadButton_RightShoulder = 10;
inline constexpr int kGamepadButton_DpadUp        = 11;
inline constexpr int kGamepadButton_DpadDown      = 12;
inline constexpr int kGamepadButton_DpadLeft      = 13;
inline constexpr int kGamepadButton_DpadRight     = 14;
inline constexpr int kGamepadAxis_LeftX           =  0;
inline constexpr int kGamepadAxis_LeftY           =  1;
inline constexpr int kGamepadAxis_RightX          =  2;
inline constexpr int kGamepadAxis_RightY          =  3;
inline constexpr int kGamepadAxis_LeftTrigger     =  4;
inline constexpr int kGamepadAxis_RightTrigger    =  5;
// clang-format on

// ----- Input Events (for Event Bus subscribers) ----------------------------

struct KeyEvent final {
  KeyScancode scancode = 0;
  bool down = false;
};

/// Mouse move event payload (position + delta).
struct MouseMoveEvent final {
  int x = 0;
  int y = 0;
  int deltaX = 0;
  int deltaY = 0;
};

/// Mouse button event payload.
struct MouseButtonEvent final {
  int button = 0;
  bool down = false;
};

// ----- Key Constants -------------------------------------------------------
// clang-format off
inline constexpr KeyScancode kKey_A         =   4;
inline constexpr KeyScancode kKey_B         =   5;
inline constexpr KeyScancode kKey_C         =   6;
inline constexpr KeyScancode kKey_D         =   7;
inline constexpr KeyScancode kKey_E         =   8;
inline constexpr KeyScancode kKey_F         =   9;
inline constexpr KeyScancode kKey_G         =  10;
inline constexpr KeyScancode kKey_H         =  11;
inline constexpr KeyScancode kKey_I         =  12;
inline constexpr KeyScancode kKey_J         =  13;
inline constexpr KeyScancode kKey_K         =  14;
inline constexpr KeyScancode kKey_L         =  15;
inline constexpr KeyScancode kKey_M         =  16;
inline constexpr KeyScancode kKey_N         =  17;
inline constexpr KeyScancode kKey_O         =  18;
inline constexpr KeyScancode kKey_P         =  19;
inline constexpr KeyScancode kKey_Q         =  20;
inline constexpr KeyScancode kKey_R         =  21;
inline constexpr KeyScancode kKey_S         =  22;
inline constexpr KeyScancode kKey_T         =  23;
inline constexpr KeyScancode kKey_U         =  24;
inline constexpr KeyScancode kKey_V         =  25;
inline constexpr KeyScancode kKey_W         =  26;
inline constexpr KeyScancode kKey_X         =  27;
inline constexpr KeyScancode kKey_Y         =  28;
inline constexpr KeyScancode kKey_Z         =  29;
inline constexpr KeyScancode kKey_1         =  30;
inline constexpr KeyScancode kKey_2         =  31;
inline constexpr KeyScancode kKey_3         =  32;
inline constexpr KeyScancode kKey_4         =  33;
inline constexpr KeyScancode kKey_5         =  34;
inline constexpr KeyScancode kKey_6         =  35;
inline constexpr KeyScancode kKey_7         =  36;
inline constexpr KeyScancode kKey_8         =  37;
inline constexpr KeyScancode kKey_9         =  38;
inline constexpr KeyScancode kKey_0         =  39;
inline constexpr KeyScancode kKey_Return    =  40;
inline constexpr KeyScancode kKey_Escape    =  41;
inline constexpr KeyScancode kKey_Backspace =  42;
inline constexpr KeyScancode kKey_Tab       =  43;
inline constexpr KeyScancode kKey_Space     =  44;
inline constexpr KeyScancode kKey_F1        =  58;
inline constexpr KeyScancode kKey_F2        =  59;
inline constexpr KeyScancode kKey_F3        =  60;
inline constexpr KeyScancode kKey_F4        =  61;
inline constexpr KeyScancode kKey_F5        =  62;
inline constexpr KeyScancode kKey_F6        =  63;
inline constexpr KeyScancode kKey_F7        =  64;
inline constexpr KeyScancode kKey_F8        =  65;
inline constexpr KeyScancode kKey_F9        =  66;
inline constexpr KeyScancode kKey_F10       =  67;
inline constexpr KeyScancode kKey_F11       =  68;
inline constexpr KeyScancode kKey_F12       =  69;
inline constexpr KeyScancode kKey_Delete    =  76;
inline constexpr KeyScancode kKey_Right     =  79;
inline constexpr KeyScancode kKey_Left      =  80;
inline constexpr KeyScancode kKey_Down      =  81;
inline constexpr KeyScancode kKey_Up        =  82;
inline constexpr KeyScancode kKey_LCtrl     = 224;
inline constexpr KeyScancode kKey_LShift    = 225;
inline constexpr KeyScancode kKey_LAlt      = 226;
// clang-format on

} // namespace engine::core
