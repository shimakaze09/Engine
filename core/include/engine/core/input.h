#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

// Scancode type. Values match SDL_SCANCODE_* from the SDL2 backend.
using KeyScancode = int;

// ----- Lifecycle -----------------------------------------------------------

bool initialize_input() noexcept;
void shutdown_input() noexcept;

// Called once per frame around the platform event loop.
void begin_input_frame() noexcept;
void input_process_event(const void *nativeEvent) noexcept;
void end_input_frame() noexcept;

// ----- Keyboard ------------------------------------------------------------

bool is_key_down(KeyScancode scancode) noexcept;
bool is_key_pressed(KeyScancode scancode) noexcept;
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

MouseState mouse_state() noexcept;
bool is_mouse_button_down(int button) noexcept;
bool is_mouse_button_pressed(int button) noexcept;

// ----- Action Mappings -----------------------------------------------------

inline constexpr std::size_t kMaxActions = 64U;
inline constexpr std::size_t kMaxAxes = 64U;

bool register_action(const char *name, KeyScancode key,
                     int mouseButton = -1) noexcept;
bool is_action_down(const char *name) noexcept;
bool is_action_pressed(const char *name) noexcept;
float action_value(const char *name) noexcept;

bool register_axis(const char *name, KeyScancode negativeKey,
                   KeyScancode positiveKey) noexcept;
float axis_value(const char *name) noexcept;

// ----- Input Events (for Event Bus subscribers) ----------------------------

struct KeyEvent final {
  KeyScancode scancode = 0;
  bool down = false;
};

struct MouseMoveEvent final {
  int x = 0;
  int y = 0;
  int deltaX = 0;
  int deltaY = 0;
};

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
