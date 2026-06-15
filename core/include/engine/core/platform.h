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
/// Handles request platform quit.
void request_platform_quit() noexcept;
/// Handles make render context current.
bool make_render_context_current() noexcept;
/// Handles release render context.
void release_render_context() noexcept;
/// Handles swap render buffers.
void swap_render_buffers() noexcept;
/// Returns the requested value for gl proc address.
void *get_gl_proc_address(const char *name) noexcept;
/// Handles render drawable size.
void render_drawable_size(int *outWidth, int *outHeight) noexcept;
/// Returns the requested value for sdl window.
void *get_sdl_window() noexcept;
/// Returns the requested value for sdl gl context.
void *get_sdl_gl_context() noexcept;
/// Handles process memory bytes.
std::size_t process_memory_bytes() noexcept;

/// Handles platform get save dir.
bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Handles platform get save dir.
bool platform_get_save_dir(const char *organizationName,
                           const char *applicationName, char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
/// Handles platform get app dir.
bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept;
/// Handles platform get temp dir.
bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;

} // namespace engine::core
