// Implements platform behavior for the Engine core engine.

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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "engine/core/logging.h"

namespace engine::core {

namespace {

bool g_platformRunning = false;
SDL_Window *g_window = nullptr;
SDL_GLContext g_glContext = nullptr;

constexpr std::size_t kPlatformPathMax = 1024U;
constexpr char kDefaultOrganizationName[] = "Engine";
constexpr char kDefaultApplicationName[] = "Engine";

/// Handles validate path output.
bool validate_path_output(char *outBuffer,
                          std::size_t bufferCapacity) noexcept {
  if ((outBuffer == nullptr) || (bufferCapacity == 0U)) {
    return false;
  }
  outBuffer[0] = '\0';
  return true;
}

/// Handles non empty env.
const char *non_empty_env(const char *name) noexcept {
#if defined(_WIN32)
  static thread_local char value[kPlatformPathMax] = {};
  if ((name == nullptr) || (name[0] == '\0')) {
    return nullptr;
  }
  const DWORD length =
      GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof(value)));
  if ((length == 0U) || (length >= sizeof(value))) {
    value[0] = '\0';
    return nullptr;
  }
  return value;
#else
  const char *value = std::getenv(name);
  if ((value == nullptr) || (value[0] == '\0')) {
    return nullptr;
  }
  return value;
#endif
}

/// Clamps and fills settings into a safe runtime range for directory path.
void normalize_directory_path(char *path) noexcept {
  if (path == nullptr) {
    return;
  }

  std::size_t length = std::strlen(path);
  for (std::size_t i = 0U; i < length; ++i) {
    if (path[i] == '\\') {
      path[i] = '/';
    }
  }

  while (length > 1U && path[length - 1U] == '/') {
    if ((length == 3U) && (path[1] == ':')) {
      break;
    }
    path[length - 1U] = '\0';
    --length;
  }
}

/// Handles copy normalized path.
bool copy_normalized_path(const char *path, char *outBuffer,
                          std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity) || (path == nullptr) ||
      (path[0] == '\0')) {
    return false;
  }

  const std::size_t length = std::strlen(path);
  if ((length + 1U) > bufferCapacity) {
    return false;
  }

  std::memcpy(outBuffer, path, length + 1U);
  normalize_directory_path(outBuffer);
  return outBuffer[0] != '\0';
}

/// Handles append path segment.
bool append_path_segment(char *base, std::size_t capacity,
                         const char *segment) noexcept {
  if ((base == nullptr) || (segment == nullptr) || (segment[0] == '\0')) {
    return false;
  }

  normalize_directory_path(base);

  const std::size_t baseLength = std::strlen(base);
  const std::size_t segmentLength = std::strlen(segment);
  const bool needsSeparator = (baseLength > 0U) && (base[baseLength - 1U] != '/');
  const std::size_t totalLength =
      baseLength + (needsSeparator ? 1U : 0U) + segmentLength;
  if ((totalLength + 1U) > capacity) {
    return false;
  }

  std::size_t writeOffset = baseLength;
  if (needsSeparator) {
    base[writeOffset] = '/';
    ++writeOffset;
  }
  std::memcpy(base + writeOffset, segment, segmentLength);
  base[totalLength] = '\0';
  normalize_directory_path(base);
  return true;
}

/// Builds the requested runtime data for save base.
bool build_save_base(char *outBuffer, std::size_t bufferCapacity) noexcept {
#if defined(_WIN32)
  if (const char *appData = non_empty_env("APPDATA")) {
    return copy_normalized_path(appData, outBuffer, bufferCapacity);
  }
  if (const char *userProfile = non_empty_env("USERPROFILE")) {
    if (!copy_normalized_path(userProfile, outBuffer, bufferCapacity)) {
      return false;
    }
    return append_path_segment(outBuffer, bufferCapacity, "AppData") &&
           append_path_segment(outBuffer, bufferCapacity, "Roaming");
  }
  return false;
#elif defined(__APPLE__)
  const char *home = non_empty_env("HOME");
  if (home == nullptr) {
    return false;
  }
  if (!copy_normalized_path(home, outBuffer, bufferCapacity)) {
    return false;
  }
  return append_path_segment(outBuffer, bufferCapacity, "Library") &&
         append_path_segment(outBuffer, bufferCapacity, "Application Support");
#else
  if (const char *xdgDataHome = non_empty_env("XDG_DATA_HOME")) {
    return copy_normalized_path(xdgDataHome, outBuffer, bufferCapacity);
  }
  const char *home = non_empty_env("HOME");
  if (home == nullptr) {
    return false;
  }
  if (!copy_normalized_path(home, outBuffer, bufferCapacity)) {
    return false;
  }
  return append_path_segment(outBuffer, bufferCapacity, ".local") &&
         append_path_segment(outBuffer, bufferCapacity, "share");
#endif
}

/// Handles log sdl error.
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

/// Shuts down the owning system for platform resources.
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

/// Initializes the owning system for platform impl.
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

/// Initializes the owning system for platform.
bool initialize_platform() noexcept {
  return initialize_platform_impl(1280, 720, "engine", true);
}

/// Initializes the owning system for platform.
bool initialize_platform(const PlatformConfig &config) noexcept {
  const int w = (config.width > 0) ? config.width : 1280;
  const int h = (config.height > 0) ? config.height : 720;
  const char *title = (config.title != nullptr) ? config.title : "engine";
  return initialize_platform_impl(w, h, title, config.vsync);
}

/// Shuts down the owning system for platform.
void shutdown_platform() noexcept {
  g_platformRunning = false;
  shutdown_platform_resources();
}

/// Returns whether is platform running.
bool is_platform_running() noexcept { return g_platformRunning; }

/// Handles request platform quit.
void request_platform_quit() noexcept { g_platformRunning = false; }

/// Handles make render context current.
bool make_render_context_current() noexcept {
  if ((g_window == nullptr) || (g_glContext == nullptr)) {
    return false;
  }

  return SDL_GL_MakeCurrent(g_window, g_glContext) == 0;
}

/// Handles release render context.
void release_render_context() noexcept {
  if (g_window != nullptr) {
    static_cast<void>(SDL_GL_MakeCurrent(g_window, nullptr));
  }
}

/// Handles swap render buffers.
void swap_render_buffers() noexcept {
  if (g_window != nullptr) {
    SDL_GL_SwapWindow(g_window);
  }
}

/// Returns the requested value for gl proc address.
void *get_gl_proc_address(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }

  return SDL_GL_GetProcAddress(name);
}

/// Handles render drawable size.
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

/// Returns the requested value for sdl window.
void *get_sdl_window() noexcept { return g_window; }

/// Returns the requested value for sdl gl context.
void *get_sdl_gl_context() noexcept { return g_glContext; }

/// Handles process memory bytes.
std::size_t process_memory_bytes() noexcept {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                           sizeof(pmc)) == 0) {
    return 0U;
  }
  return static_cast<std::size_t>(pmc.WorkingSetSize);
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  const kern_return_t result =
      task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count);
  if (result != KERN_SUCCESS) {
    return 0U;
  }
  return static_cast<std::size_t>(info.resident_size);
#elif defined(__linux__)
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) {
    return 0U;
  }

  FILE *fp = std::fopen("/proc/self/statm", "r");
  if (fp == nullptr) {
    return 0U;
  }

  unsigned long long totalPages = 0ULL;
  unsigned long long residentPages = 0ULL;
  const int scanned = std::fscanf(fp, "%llu %llu", &totalPages, &residentPages);
  std::fclose(fp);
  if (scanned != 2) {
    return 0U;
  }

  return static_cast<std::size_t>(
      residentPages * static_cast<unsigned long long>(pageSize));
#else
  return 0U;
#endif
}

/// Handles platform get save dir.
bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept {
  return platform_get_save_dir(kDefaultOrganizationName,
                               kDefaultApplicationName, outBuffer,
                               bufferCapacity);
}

/// Handles platform get save dir.
bool platform_get_save_dir(const char *organizationName,
                           const char *applicationName, char *outBuffer,
                           std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity) ||
      (applicationName == nullptr) || (applicationName[0] == '\0')) {
    return false;
  }

  char path[kPlatformPathMax] = {};
  if (!build_save_base(path, sizeof(path))) {
    return false;
  }

  if ((organizationName != nullptr) && (organizationName[0] != '\0') &&
      !append_path_segment(path, sizeof(path), organizationName)) {
    return false;
  }

  if (!append_path_segment(path, sizeof(path), applicationName)) {
    return false;
  }

  return copy_normalized_path(path, outBuffer, bufferCapacity);
}

/// Handles platform get app dir.
bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity)) {
    return false;
  }

  char *basePath = SDL_GetBasePath();
  if (basePath == nullptr) {
    return false;
  }

  const bool result = copy_normalized_path(basePath, outBuffer, bufferCapacity);
  SDL_free(basePath);
  return result;
}

/// Handles platform get temp dir.
bool platform_get_temp_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity)) {
    return false;
  }

#if defined(_WIN32)
  char tempPath[kPlatformPathMax] = {};
  const DWORD length =
      GetTempPathA(static_cast<DWORD>(sizeof(tempPath)), tempPath);
  if ((length == 0U) || (length >= sizeof(tempPath))) {
    return false;
  }
  return copy_normalized_path(tempPath, outBuffer, bufferCapacity);
#else
  const char *tempPath = non_empty_env("TMPDIR");
  if (tempPath == nullptr) {
    tempPath = non_empty_env("TMP");
  }
  if (tempPath == nullptr) {
    tempPath = non_empty_env("TEMP");
  }
  if (tempPath == nullptr) {
    tempPath = non_empty_env("TEMPDIR");
  }
  if (tempPath == nullptr) {
    tempPath = "/tmp";
  }
  return copy_normalized_path(tempPath, outBuffer, bufferCapacity);
#endif
}

} // namespace engine::core
