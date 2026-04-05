#include "engine/core/platform.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#if defined(__clang__) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include <cstdio>

#include "engine/core/logging.h"

namespace engine::core {

namespace {

bool g_platformRunning = false;
SDL_Window *g_window = nullptr;
SDL_GLContext g_glContext = nullptr;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr char kWindowTitle[] = "engine";

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

} // namespace

bool initialize_platform() noexcept {
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
      kWindowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kWindowWidth, kWindowHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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

  if (SDL_GL_SetSwapInterval(1) != 0) {
    log_sdl_error("failed to enable VSync");
  }

  SDL_GL_MakeCurrent(g_window, nullptr);
  g_platformRunning = true;
  return true;
}

void shutdown_platform() noexcept {
  g_platformRunning = false;
  shutdown_platform_resources();
}

void process_input() noexcept {
  SDL_Event event{};
  while (SDL_PollEvent(&event) != 0) {
    if (event.type == SDL_QUIT) {
      g_platformRunning = false;
      continue;
    }

    if ((event.type == SDL_KEYDOWN) && (event.key.keysym.sym == SDLK_ESCAPE)) {
      g_platformRunning = false;
    }
  }
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
    *outWidth = kWindowWidth;
    *outHeight = kWindowHeight;
    return;
  }

  SDL_GL_GetDrawableSize(g_window, outWidth, outHeight);
}

void *get_sdl_window() noexcept { return g_window; }

void *get_sdl_gl_context() noexcept { return g_glContext; }

} // namespace engine::core
