// Implements platform behavior for the Engine core engine.

#include "engine/core/platform.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <cstdint>
#include <array>
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
bool g_headless = false;
bool g_gamepadSubsystem = false;

/// One open controller: the instance id SDL announced it under and the
/// handle its events are delivered through while open.
struct OpenGamepad final {
  std::uint32_t instanceId = 0U;
  SDL_Gamepad *gamepad = nullptr;
};
std::array<OpenGamepad, static_cast<std::size_t>(kMaxGamepads)>
    g_openGamepads{};

/// Closes every open controller and the subsystem behind them.
void shutdown_gamepads() noexcept {
  for (OpenGamepad &entry : g_openGamepads) {
    if (entry.gamepad != nullptr) {
      SDL_CloseGamepad(entry.gamepad);
    }
    entry = OpenGamepad{};
  }
  if (g_gamepadSubsystem) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    g_gamepadSubsystem = false;
  }
}
constexpr std::size_t kPlatformPathMax = 1024U;
constexpr char kDefaultOrganizationName[] = "Engine";
constexpr char kDefaultApplicationName[] = "Engine";

bool validate_path_output(char *outBuffer,
                          std::size_t bufferCapacity) noexcept {
  if ((outBuffer == nullptr) || (bufferCapacity == 0U)) {
    return false;
  }
  outBuffer[0] = '\0';
  return true;
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
  if (g_window != nullptr) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
  if (g_headless) {
    static_cast<void>(SDL_ResetHint(SDL_HINT_VIDEO_DRIVER));
    static_cast<void>(SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS));
  }
  g_headless = false;

  shutdown_gamepads();
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/// Initializes the owning system for platform impl.
bool initialize_platform_impl(int width, int height, const char *title,
                              bool headless) noexcept {
  if (g_window != nullptr) {
    g_platformRunning = true;
    return true;
  }

  // #196: headless is self-contained — force SDL's dummy video driver so
  // CI runners with no display still initialize the video subsystem.
  if (headless) {
    static_cast<void>(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    // SDL drops controller events while no window has keyboard focus, and
    // a hidden headless window never takes focus, so headless runs opt in
    // to background delivery or would never see a gamepad edge.
    static_cast<void>(
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"));
  }

  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    log_sdl_error("failed to initialize SDL video subsystem");
    return false;
  }

  // Controllers are optional hardware: a platform without an input
  // backend still runs, it just never announces a gamepad.
  g_gamepadSubsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!g_gamepadSubsystem) {
    log_sdl_error("gamepad subsystem unavailable; controllers disabled");
  }

  // #196: headless is a hidden window on the dummy driver; the render
  // device then stays on the null backend.
  if (headless) {
    g_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_HIDDEN);
    if (g_window == nullptr) {
      log_sdl_error("failed to create headless SDL window");
      shutdown_platform_resources();
      return false;
    }
    g_headless = true;
    g_platformRunning = true;
    return true;
  }

  // The render backend owns its device and swapchain: the window is
  // created without an OpenGL context, the backend reads the native
  // handles below, and vsync is applied by the backend at its reset.
  g_window = SDL_CreateWindow(title, width, height,
                              SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (g_window == nullptr) {
    log_sdl_error("failed to create SDL window");
    shutdown_platform_resources();
    return false;
  }
  static_cast<void>(SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED));
  g_platformRunning = true;
  return true;
}

} // namespace

bool platform_gamepads_available() noexcept { return g_gamepadSubsystem; }

void platform_open_gamepad(std::uint32_t instanceId) noexcept {
  if (!g_gamepadSubsystem) {
    return;
  }
  OpenGamepad *free = nullptr;
  for (OpenGamepad &entry : g_openGamepads) {
    if ((entry.gamepad != nullptr) && (entry.instanceId == instanceId)) {
      return;
    }
    if ((entry.gamepad == nullptr) && (free == nullptr)) {
      free = &entry;
    }
  }
  if (free == nullptr) {
    log_message(LogLevel::Warning, "platform",
                "gamepad ignored: every controller slot is in use");
    return;
  }
  SDL_Gamepad *gamepad = SDL_OpenGamepad(instanceId);
  if (gamepad == nullptr) {
    log_sdl_error("failed to open gamepad; its input will not be delivered");
    return;
  }
  free->instanceId = instanceId;
  free->gamepad = gamepad;
}

void platform_close_gamepad(std::uint32_t instanceId) noexcept {
  for (OpenGamepad &entry : g_openGamepads) {
    if ((entry.gamepad != nullptr) && (entry.instanceId == instanceId)) {
      SDL_CloseGamepad(entry.gamepad);
      entry = OpenGamepad{};
      return;
    }
  }
}

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

/// Initializes the owning system for platform.
bool initialize_platform() noexcept {
  return initialize_platform_impl(1280, 720, "engine", false);
}

/// Initializes the owning system for platform.
bool initialize_platform(const PlatformConfig &config) noexcept {
  const int w = (config.width > 0) ? config.width : 1280;
  const int h = (config.height > 0) ? config.height : 720;
  const char *title = (config.title != nullptr) ? config.title : "engine";
  return initialize_platform_impl(w, h, title, config.headless);
}

/// Shuts down the owning system for platform.
void shutdown_platform() noexcept {
  g_platformRunning = false;
  shutdown_platform_resources();
}

/// Returns whether is platform running.
bool is_platform_running() noexcept { return g_platformRunning; }

void request_platform_quit() noexcept { g_platformRunning = false; }

void render_drawable_size(int *outWidth, int *outHeight) noexcept {
  if ((outWidth == nullptr) || (outHeight == nullptr)) {
    return;
  }

  if (g_window == nullptr) {
    *outWidth = 1280;
    *outHeight = 720;
    return;
  }

  static_cast<void>(SDL_GetWindowSizeInPixels(g_window, outWidth, outHeight));
}

void *get_sdl_window() noexcept { return g_window; }


bool platform_window_is_wayland() noexcept {
  const char *driver = SDL_GetCurrentVideoDriver();
  return (driver != nullptr) && (SDL_strcmp(driver, "wayland") == 0);
}

void *platform_native_window_handle() noexcept {
  if ((g_window == nullptr) || g_headless) {
    return nullptr;
  }
#if defined(ENGINE_PLATFORM_WEB)
  // Emscripten: bgfx takes the canvas CSS selector as the window handle.
  return const_cast<char *>("#canvas");
#else
  const SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
#if defined(_WIN32)
  return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                nullptr);
#elif defined(__APPLE__)
  return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                nullptr);
#else
  if (platform_window_is_wayland()) {
    return SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  }
  // X11 exposes the window as a numeric id; external backends consume
  // it through the same opaque pointer channel.
  const Sint64 xid =
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
  return reinterpret_cast<void *>(static_cast<std::uintptr_t>(xid));
#endif
#endif
}

void *platform_native_display_handle() noexcept {
  if ((g_window == nullptr) || g_headless) {
    return nullptr;
  }
#if defined(_WIN32) || defined(__APPLE__)
  return nullptr;
#else
  const SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
  if (platform_window_is_wayland()) {
    return SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  }
  return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER,
                                nullptr);
#endif
}

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

bool platform_get_save_dir(char *outBuffer,
                           std::size_t bufferCapacity) noexcept {
  return platform_get_save_dir(kDefaultOrganizationName,
                               kDefaultApplicationName, outBuffer,
                               bufferCapacity);
}

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

bool platform_get_app_dir(char *outBuffer,
                          std::size_t bufferCapacity) noexcept {
  if (!validate_path_output(outBuffer, bufferCapacity)) {
    return false;
  }

  // SDL3 owns the returned base-path string; it must not be freed.
  const char *basePath = SDL_GetBasePath();
  if (basePath == nullptr) {
    return false;
  }

  return copy_normalized_path(basePath, outBuffer, bufferCapacity);
}

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
