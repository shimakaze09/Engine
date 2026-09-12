// Declares platform types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Gamepads the platform keeps open and the input layer tracks at once;
/// a controller announced past this many is logged and ignored.
inline constexpr int kMaxGamepads = 4;

// Configuration for window creation.
struct PlatformConfig final {
  int width = 1280;
  int height = 720;
  const char *title = "engine";
  // Hidden window on SDL's dummy video driver, so bootstrap completes on a
  // machine with no display; the render device then stays on the null
  // backend. A visible window is created without an OpenGL context: the
  // render backend owns its device and swapchain and reads the native
  // handles below.
  bool headless = false;
};

/// Initializes the owning system for platform.
bool initialize_platform() noexcept;
/// Initializes the owning system for platform.
bool initialize_platform(const PlatformConfig &config) noexcept;
/// Shuts down the owning system for platform.
void shutdown_platform() noexcept;
/// Returns whether is platform running.
bool is_platform_running() noexcept;
/// Requests the platform loop to exit after the current frame.
void request_platform_quit() noexcept;
/// Drawable size in pixels (may differ from window size on HiDPI).
void render_drawable_size(int *outWidth, int *outHeight) noexcept;
/// Environment variable value, or nullptr when unset or empty (the
/// Windows path avoids the CRT getenv deprecation).
const char *non_empty_env(const char *name) noexcept;

// ----- Gamepad devices -------------------------------------------------------
// The platform owns the OS-level gamepad subsystem and the open device
// handles: SDL announces a controller and delivers its button and axis
// events only for devices that were opened, so the input layer asks the
// platform to open a device on its hotplug arrival and to close it on
// removal. Device identity is the instance id the arrival event carries.

/// True when the gamepad subsystem initialized with the platform; false
/// when it is unavailable (then no device is ever opened and the input
/// layer sees only synthetic events).
bool platform_gamepads_available() noexcept;
/// Opens the device behind an instance id so its events are delivered;
/// a failure (subsystem unavailable, table full, device refused) is logged
/// and leaves the device closed. Idempotent for an already open id.
void platform_open_gamepad(std::uint32_t instanceId) noexcept;
/// Closes the device behind an instance id; a no-op for an unknown id.
void platform_close_gamepad(std::uint32_t instanceId) noexcept;
/// Underlying SDL_Window* (opaque; platform/editor glue only).
void *get_sdl_window() noexcept;
/// Native window handle for external render backends (#138): X11 window
/// id / Wayland wl_surface / Win32 HWND / Cocoa NSWindow, null when
/// headless or before initialization.
void *platform_native_window_handle() noexcept;
/// Native display handle for external render backends (X11 Display /
/// Wayland wl_display); null on platforms without a display connection.
void *platform_native_display_handle() noexcept;
/// True when the platform window runs on the Wayland video driver (an
/// external backend must use Wayland platform-data semantics).
bool platform_window_is_wayland() noexcept;
/// Resident memory of the process in bytes (0 when unsupported).
std::size_t process_memory_bytes() noexcept;

/// Per-user save directory using the engine's default org/app names.
bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Per-user save directory for an explicit org/app pair.
bool platform_get_save_dir(const char *organizationName,
                           const char *applicationName, char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Directory containing the running executable.
bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept;
/// OS temp directory.
bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;

} // namespace engine::core
