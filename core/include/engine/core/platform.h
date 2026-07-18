// Declares platform types and APIs for the Engine core engine.

#pragma once

#include <cstddef>

namespace engine::core {

// Configuration for window creation.
struct PlatformConfig final {
  int width = 1280;
  int height = 720;
  const char *title = "engine";
  bool vsync = true;
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
/// Makes the GL context current on this thread; false when headless.
bool make_render_context_current() noexcept;
/// Releases the GL context from this thread.
void release_render_context() noexcept;
/// Swaps the window's front/back buffers.
void swap_render_buffers() noexcept;
/// GL function loader (wraps SDL_GL_GetProcAddress).
void *get_gl_proc_address(const char *name) noexcept;
/// Drawable size in pixels (may differ from window size on HiDPI).
void render_drawable_size(int *outWidth, int *outHeight) noexcept;
/// Underlying SDL_Window* (opaque; platform/editor glue only).
void *get_sdl_window() noexcept;
/// Underlying SDL_GLContext (opaque; platform/editor glue only).
void *get_sdl_gl_context() noexcept;
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
