#pragma once

namespace engine::core {

// Configuration for window creation.
struct PlatformConfig final {
  int width = 1280;
  int height = 720;
  const char *title = "engine";
  bool vsync = true;
};

bool initialize_platform() noexcept;
bool initialize_platform(const PlatformConfig &config) noexcept;
void shutdown_platform() noexcept;
void process_input() noexcept;
bool is_platform_running() noexcept;
void request_platform_quit() noexcept;
bool make_render_context_current() noexcept;
void release_render_context() noexcept;
void swap_render_buffers() noexcept;
void *get_gl_proc_address(const char *name) noexcept;
void render_drawable_size(int *outWidth, int *outHeight) noexcept;
void *get_sdl_window() noexcept;
void *get_sdl_gl_context() noexcept;

// ---------- Input ----------------------------------------------------------
// Scancode type. Values match SDL_SCANCODE_* from the SDL2 backend.
using KeyScancode = int;

bool is_key_down(KeyScancode scancode) noexcept;
bool is_key_pressed(KeyScancode scancode) noexcept;

// clang-format off
inline constexpr KeyScancode kKey_A      = 4;
inline constexpr KeyScancode kKey_B      = 5;
inline constexpr KeyScancode kKey_C      = 6;
inline constexpr KeyScancode kKey_D      = 7;
inline constexpr KeyScancode kKey_E      = 8;
inline constexpr KeyScancode kKey_F      = 9;
inline constexpr KeyScancode kKey_G      = 10;
inline constexpr KeyScancode kKey_H      = 11;
inline constexpr KeyScancode kKey_I      = 12;
inline constexpr KeyScancode kKey_J      = 13;
inline constexpr KeyScancode kKey_K      = 14;
inline constexpr KeyScancode kKey_L      = 15;
inline constexpr KeyScancode kKey_M      = 16;
inline constexpr KeyScancode kKey_N      = 17;
inline constexpr KeyScancode kKey_O      = 18;
inline constexpr KeyScancode kKey_P      = 19;
inline constexpr KeyScancode kKey_Q      = 20;
inline constexpr KeyScancode kKey_R      = 21;
inline constexpr KeyScancode kKey_S      = 22;
inline constexpr KeyScancode kKey_T      = 23;
inline constexpr KeyScancode kKey_U      = 24;
inline constexpr KeyScancode kKey_V      = 25;
inline constexpr KeyScancode kKey_W      = 26;
inline constexpr KeyScancode kKey_X      = 27;
inline constexpr KeyScancode kKey_Y      = 28;
inline constexpr KeyScancode kKey_Z      = 29;
inline constexpr KeyScancode kKey_1      = 30;
inline constexpr KeyScancode kKey_2      = 31;
inline constexpr KeyScancode kKey_3      = 32;
inline constexpr KeyScancode kKey_4      = 33;
inline constexpr KeyScancode kKey_5      = 34;
inline constexpr KeyScancode kKey_6      = 35;
inline constexpr KeyScancode kKey_7      = 36;
inline constexpr KeyScancode kKey_8      = 37;
inline constexpr KeyScancode kKey_9      = 38;
inline constexpr KeyScancode kKey_0      = 39;
inline constexpr KeyScancode kKey_Return = 40;
inline constexpr KeyScancode kKey_Escape = 41;
inline constexpr KeyScancode kKey_Space  = 44;
inline constexpr KeyScancode kKey_Right  = 79;
inline constexpr KeyScancode kKey_Left   = 80;
inline constexpr KeyScancode kKey_Down   = 81;
inline constexpr KeyScancode kKey_Up     = 82;
inline constexpr KeyScancode kKey_LShift = 225;
inline constexpr KeyScancode kKey_LCtrl  = 224;
inline constexpr KeyScancode kKey_LAlt   = 226;
// clang-format on

} // namespace engine::core
