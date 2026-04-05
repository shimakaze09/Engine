#pragma once

namespace engine::core {

bool initialize_platform() noexcept;
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

} // namespace engine::core
