#include "engine/core/platform.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include <cstddef>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <psapi.h>
#include <windows.h>


#endif

#include "engine/core/logging.h"

namespace engine::core {

namespace {

bool g_platformRunning = false;
SDL_Window *g_window = nullptr;
SDL_GLContext g_glContext = nullptr;

void log_sdl_error(const char *message) noexcept {
  const char *sdlError = SDL_GetError();
  if ((sdlError == nullptr) || (sdlError[0] == '\0')) {
    log_message(LogLevel::Error, "platform", message);
    return;
  }

  char buffer[256] = {};
  std::snprintf(buffer, sizeof(buffer), "%s: %s", message, sdlError);
  log_message(LogLevel::Error, "platform", buffer);
}

void shutdown_platform_resources() noexcept {
  if (g_glContext != nullptr) {
    SDL_GL_MakeCurrent(g_window, nullptr);
    SDL_GL_DeleteContext(g_glContext);
    g_glContext = nullptr;
  }

  if (g_window != nullptr) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }

  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool initialize_platform_impl(int width, int height, const char *title,
                              bool vsync) noexcept {
  SDL_SetMainReady();

  if (g_window != nullptr) {
    g_platformRunning = true;
    return true;
  }

  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    log_sdl_error("failed to initialize SDL video subsystem");
    return false;
  }

  if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          SDL_GL_CONTEXT_PROFILE_CORE) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24) != 0) {
    log_sdl_error("failed to configure OpenGL context attributes");
    shutdown_platform_resources();
    return false;
  }

  g_window = SDL_CreateWindow(
      title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (g_window == nullptr) {
    log_sdl_error("failed to create SDL window");
    shutdown_platform_resources();
    return false;
  }

  g_glContext = SDL_GL_CreateContext(g_window);
  if (g_glContext == nullptr) {
    log_sdl_error("failed to create OpenGL context");
    shutdown_platform_resources();
    return false;
  }

  if (SDL_GL_MakeCurrent(g_window, g_glContext) != 0) {
    log_sdl_error("failed to make OpenGL context current");
    shutdown_platform_resources();
    return false;
  }

  if (SDL_GL_SetSwapInterval(vsync ? 1 : 0) != 0) {
    log_sdl_error("failed to set swap interval");
  }

  SDL_GL_MakeCurrent(g_window, nullptr);
  g_platformRunning = true;
  return true;
}

} // namespace

bool initialize_platform() noexcept {
  return initialize_platform_impl(1280, 720, "engine", true);
}

bool initialize_platform(const PlatformConfig &config) noexcept {
  const int w = (config.width > 0) ? config.width : 1280;
  const int h = (config.height > 0) ? config.height : 720;
  const char *title = (config.title != nullptr) ? config.title : "engine";
  return initialize_platform_impl(w, h, title, config.vsync);
}

void shutdown_platform() noexcept {
  g_platformRunning = false;
  shutdown_platform_resources();
}

bool is_platform_running() noexcept { return g_platformRunning; }

void request_platform_quit() noexcept { g_platformRunning = false; }

bool make_render_context_current() noexcept {
  if ((g_window == nullptr) || (g_glContext == nullptr)) {
    return false;
  }

  return SDL_GL_MakeCurrent(g_window, g_glContext) == 0;
}

void release_render_context() noexcept {
  if (g_window != nullptr) {
    static_cast<void>(SDL_GL_MakeCurrent(g_window, nullptr));
  }
}

void swap_render_buffers() noexcept {
  if (g_window != nullptr) {
    SDL_GL_SwapWindow(g_window);
  }
}

void *get_gl_proc_address(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }

  return SDL_GL_GetProcAddress(name);
}

void render_drawable_size(int *outWidth, int *outHeight) noexcept {
  if ((outWidth == nullptr) || (outHeight == nullptr)) {
    return;
  }

  if (g_window == nullptr) {
    *outWidth = 1280;
    *outHeight = 720;
    return;
  }

  SDL_GL_GetDrawableSize(g_window, outWidth, outHeight);
}

void *get_sdl_window() noexcept { return g_window; }

void *get_sdl_gl_context() noexcept { return g_glContext; }

std::size_t process_memory_bytes() noexcept {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                           sizeof(pmc)) == 0) {
    return 0U;
  }
  return static_cast<std::size_t>(pmc.WorkingSetSize);
#else
  return 0U;
#endif
}

} // namespace engine::core
