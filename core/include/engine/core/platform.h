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

bool initialize_platform() noexcept;
bool initialize_platform(const PlatformConfig &config) noexcept;
void shutdown_platform() noexcept;
bool is_platform_running() noexcept;
void request_platform_quit() noexcept;
bool make_render_context_current() noexcept;
void release_render_context() noexcept;
void swap_render_buffers() noexcept;
void *get_gl_proc_address(const char *name) noexcept;
void render_drawable_size(int *outWidth, int *outHeight) noexcept;
void *get_sdl_window() noexcept;
void *get_sdl_gl_context() noexcept;
std::size_t process_memory_bytes() noexcept;

bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
bool platform_get_save_dir(const char *organizationName,
                           const char *applicationName, char *outBuffer,
                           std::size_t bufferCapacity) noexcept;
bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept;
bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept;

} // namespace engine::core
