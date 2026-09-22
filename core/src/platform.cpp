// Implements platform behavior for the Engine core engine.

// rand_s (platform_random_bytes) is only declared when this is defined
// before the CRT headers.
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "engine/core/platform.h"
#include "engine/core/platform_event.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <cstdint>
#include <array>
#include <atomic>
#include <cstdio>
#include <cerrno>
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
#include <sys/random.h>
#include <unistd.h>
#endif

#include "engine/core/input.h"
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
  char value[kPlatformPathMax] = {};
#if defined(_WIN32)
  if (non_empty_env("APPDATA", value, sizeof(value))) {
    return copy_normalized_path(value, outBuffer, bufferCapacity);
  }
  if (non_empty_env("USERPROFILE", value, sizeof(value))) {
    if (!copy_normalized_path(value, outBuffer, bufferCapacity)) {
      return false;
    }
    return append_path_segment(outBuffer, bufferCapacity, "AppData") &&
           append_path_segment(outBuffer, bufferCapacity, "Roaming");
  }
  return false;
#elif defined(__APPLE__)
  if (!non_empty_env("HOME", value, sizeof(value))) {
    return false;
  }
  if (!copy_normalized_path(value, outBuffer, bufferCapacity)) {
    return false;
  }
  return append_path_segment(outBuffer, bufferCapacity, "Library") &&
         append_path_segment(outBuffer, bufferCapacity, "Application Support");
#else
  if (non_empty_env("XDG_DATA_HOME", value, sizeof(value))) {
    return copy_normalized_path(value, outBuffer, bufferCapacity);
  }
  if (!non_empty_env("HOME", value, sizeof(value))) {
    return false;
  }
  if (!copy_normalized_path(value, outBuffer, bufferCapacity)) {
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

  // Headless is self-contained — force SDL's dummy video driver so
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

  // Headless is a hidden window on the dummy driver; the render
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
  // The configured size is meant in the display's own scale: on a
  // platform whose window units are pixels (Windows, X11) a 200 %
  // display would otherwise open a 1280x720 window holding a 640x360
  // UI. Where window units already follow the display scale (macOS
  // points) the two readings agree and nothing changes.
  const float displayScale = SDL_GetWindowDisplayScale(g_window);
  const float pixelDensity = SDL_GetWindowPixelDensity(g_window);
  if ((displayScale > 0.0F) && (pixelDensity > 0.0F)) {
    const float factor = displayScale / pixelDensity;
    if (factor > 1.01F) {
      static_cast<void>(SDL_SetWindowSize(
          g_window, static_cast<int>(static_cast<float>(width) * factor),
          static_cast<int>(static_cast<float>(height) * factor)));
    }
  }
  static_cast<void>(SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED));
  g_platformRunning = true;
  return true;
}


/// One native dialog in flight. SDL's callback has a different signature
/// from the engine's, and SDL reads the filter array after the show call
/// returns, so both the caller's callback and a copy of its filters have
/// to live somewhere until the dialog closes: here.
///
/// The cross-thread contract is inUse. The main thread claims a slot with
/// an acquire exchange, fills it, then asks SDL to show the dialog, so
/// everything it wrote happens before SDL can invoke the trampoline. The
/// trampoline reads the slot, calls through, and releases inUse last, so
/// the main thread never refills a slot SDL may still read.
struct DialogSlot final {
  std::atomic<bool> inUse{false};
  FileDialogCallback callback = nullptr;
  void *userData = nullptr;
  std::array<SDL_DialogFileFilter,
             static_cast<std::size_t>(kMaxFileDialogFilters)>
      filters{};
};
// Dialogs are modal to the user; more than one at a time only happens when
// one outlives an editor session, which is what the spare slots are for.
constexpr std::size_t kMaxOpenDialogs = 4U;
std::array<DialogSlot, kMaxOpenDialogs> g_dialogSlots{};

/// SDL's callback, translated to the engine's: a null list is a failure,
/// an empty list a cancel, and both reach the caller as a null path.
void SDLCALL dialog_trampoline(void *userdata, const char *const *filelist,
                               int /*filter*/) noexcept {
  auto *slot = static_cast<DialogSlot *>(userdata);
  if (slot == nullptr) {
    return;
  }
  if (filelist == nullptr) {
    log_sdl_error("native file dialog failed");
  }
  const char *path =
      ((filelist != nullptr) && (filelist[0] != nullptr)) ? filelist[0]
                                                          : nullptr;
  const FileDialogCallback callback = slot->callback;
  void *const userData = slot->userData;
  if (callback != nullptr) {
    callback(userData, path);
  }
  slot->inUse.store(false, std::memory_order_release);
}

// The engine's gamepad vocabulary is SDL's numbering and must stay so:
// persisted bindings and scripts written against the raw codes keep their
// meaning. Checked here -- the one place both are in view -- so the
// translation below can pass the values through and a reordering on
// either side fails to compile.
static_assert(kGamepadButton_South == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert(kGamepadButton_East == SDL_GAMEPAD_BUTTON_EAST);
static_assert(kGamepadButton_West == SDL_GAMEPAD_BUTTON_WEST);
static_assert(kGamepadButton_North == SDL_GAMEPAD_BUTTON_NORTH);
static_assert(kGamepadButton_Back == SDL_GAMEPAD_BUTTON_BACK);
static_assert(kGamepadButton_Guide == SDL_GAMEPAD_BUTTON_GUIDE);
static_assert(kGamepadButton_Start == SDL_GAMEPAD_BUTTON_START);
static_assert(kGamepadButton_LeftStick == SDL_GAMEPAD_BUTTON_LEFT_STICK);
static_assert(kGamepadButton_RightStick == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
static_assert(kGamepadButton_LeftShoulder == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
static_assert(kGamepadButton_RightShoulder ==
              SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
static_assert(kGamepadButton_DpadUp == SDL_GAMEPAD_BUTTON_DPAD_UP);
static_assert(kGamepadButton_DpadDown == SDL_GAMEPAD_BUTTON_DPAD_DOWN);
static_assert(kGamepadButton_DpadLeft == SDL_GAMEPAD_BUTTON_DPAD_LEFT);
static_assert(kGamepadButton_DpadRight == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(kGamepadAxis_LeftX == SDL_GAMEPAD_AXIS_LEFTX);
static_assert(kGamepadAxis_LeftY == SDL_GAMEPAD_AXIS_LEFTY);
static_assert(kGamepadAxis_RightX == SDL_GAMEPAD_AXIS_RIGHTX);
static_assert(kGamepadAxis_RightY == SDL_GAMEPAD_AXIS_RIGHTY);
static_assert(kGamepadAxis_LeftTrigger == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
static_assert(kGamepadAxis_RightTrigger == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

/// Translates one SDL event. Every event produces a PlatformEvent -- the
/// ones without an engine meaning as Other -- so the editor's ImGui
/// backend, which reads the native event, still sees all of them.
PlatformEvent translate_event(const SDL_Event &event) noexcept {
  PlatformEvent out{};
  out.timestampNs = event.common.timestamp;
  out.native = &event;

  switch (event.type) {
  case SDL_EVENT_QUIT:
    out.kind = PlatformEventKind::Quit;
    break;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
    out.kind = (event.type == SDL_EVENT_KEY_DOWN) ? PlatformEventKind::KeyDown
                                                  : PlatformEventKind::KeyUp;
    out.scancode = static_cast<int>(event.key.scancode);
    out.repeat = event.key.repeat;
    break;
  case SDL_EVENT_TEXT_INPUT:
    out.kind = PlatformEventKind::TextInput;
    break;
  case SDL_EVENT_TEXT_EDITING:
    out.kind = PlatformEventKind::TextEditing;
    break;
  case SDL_EVENT_MOUSE_MOTION:
    out.kind = PlatformEventKind::MouseMove;
    out.x = event.motion.x;
    out.y = event.motion.y;
    out.deltaX = event.motion.xrel;
    out.deltaY = event.motion.yrel;
    break;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
    out.kind = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                   ? PlatformEventKind::MouseButtonDown
                   : PlatformEventKind::MouseButtonUp;
    out.x = event.button.x;
    out.y = event.button.y;
    // SDL numbers buttons from 1 (SDL_BUTTON_LEFT); the engine from 0.
    out.mouseButton = static_cast<int>(event.button.button) - 1;
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    out.kind = PlatformEventKind::MouseWheel;
    out.wheelY = event.wheel.y;
    break;
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    out.kind = (event.type == SDL_EVENT_FINGER_DOWN)
                   ? PlatformEventKind::FingerDown
               : (event.type == SDL_EVENT_FINGER_MOTION)
                   ? PlatformEventKind::FingerMove
               : (event.type == SDL_EVENT_FINGER_UP)
                   ? PlatformEventKind::FingerUp
                   : PlatformEventKind::FingerCanceled;
    out.fingerId = static_cast<std::int64_t>(event.tfinger.fingerID);
    out.fingerX = event.tfinger.x;
    out.fingerY = event.tfinger.y;
    out.fingerDeltaX = event.tfinger.dx;
    out.fingerDeltaY = event.tfinger.dy;
    out.pressure = event.tfinger.pressure;
    break;
  case SDL_EVENT_GAMEPAD_ADDED:
  case SDL_EVENT_GAMEPAD_REMOVED:
    out.kind = (event.type == SDL_EVENT_GAMEPAD_ADDED)
                   ? PlatformEventKind::GamepadAdded
                   : PlatformEventKind::GamepadRemoved;
    out.deviceId = static_cast<std::uint32_t>(event.gdevice.which);
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    out.kind = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                   ? PlatformEventKind::GamepadButtonDown
                   : PlatformEventKind::GamepadButtonUp;
    out.deviceId = static_cast<std::uint32_t>(event.gbutton.which);
    out.gamepadButton = static_cast<int>(event.gbutton.button);
    break;
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    out.kind = PlatformEventKind::GamepadAxis;
    out.deviceId = static_cast<std::uint32_t>(event.gaxis.which);
    out.gamepadAxis = static_cast<int>(event.gaxis.axis);
    out.axisValue = event.gaxis.value;
    break;
  case SDL_EVENT_WINDOW_FOCUS_GAINED:
    out.kind = PlatformEventKind::WindowFocusGained;
    break;
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    out.kind = PlatformEventKind::WindowFocusLost;
    break;
  case SDL_EVENT_WINDOW_RESIZED:
    out.kind = PlatformEventKind::WindowResized;
    out.width = static_cast<int>(event.window.data1);
    out.height = static_cast<int>(event.window.data2);
    break;
  default:
    out.kind = PlatformEventKind::Other;
    break;
  }
  return out;
}

// The event behind the most recent poll. A PlatformEvent's native pointer
// refers to it, which is why that pointer lasts only until the next poll.
SDL_Event g_polledEvent{};

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

bool non_empty_env(const char *name, char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((name == nullptr) || (name[0] == '\0')) {
    return false;
  }
#if defined(_WIN32)
  // The API reports the length the value needs when the buffer is too
  // small, and 0 when the variable is unset; an empty value fits and
  // returns 0 too, which is the same answer here.
  const DWORD length =
      GetEnvironmentVariableA(name, out, static_cast<DWORD>(capacity));
  if ((length == 0U) || (length >= capacity)) {
    out[0] = '\0';
    return false;
  }
  return true;
#else
  const char *value = std::getenv(name);
  if ((value == nullptr) || (value[0] == '\0')) {
    return false;
  }
  const std::size_t length = std::strlen(value);
  if (length >= capacity) {
    return false;
  }
  std::memcpy(out, value, length + 1U);
  return true;
#endif
}

bool platform_random_bytes(void *out, std::size_t size) noexcept {
  if (out == nullptr) {
    return false;
  }
  if (size == 0U) {
    return true;
  }
  auto *bytes = static_cast<unsigned char *>(out);

#if defined(_WIN32)
  // rand_s draws from the OS CSPRNG and needs no extra import library,
  // unlike BCryptGenRandom. It yields four bytes at a time.
  std::size_t written = 0U;
  while (written < size) {
    unsigned int value = 0U;
    if (rand_s(&value) != 0) {
      return false;
    }
    const std::size_t chunk =
        ((size - written) < sizeof(value)) ? (size - written) : sizeof(value);
    std::memcpy(bytes + written, &value, chunk);
    written += chunk;
  }
  return true;
#elif defined(__APPLE__)
  arc4random_buf(bytes, size);
  return true;
#elif defined(__linux__)
  std::size_t written = 0U;
  while (written < size) {
    const ssize_t got = getrandom(bytes + written, size - written, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    written += static_cast<std::size_t>(got);
  }
  if (written == size) {
    return true;
  }
  // getrandom is unavailable before Linux 3.17 and can be blocked by a
  // sandbox; the device is the long-standing fallback.
  FILE *device = std::fopen("/dev/urandom", "rb");
  if (device == nullptr) {
    return false;
  }
  const std::size_t read = std::fread(bytes, 1U, size, device);
  static_cast<void>(std::fclose(device));
  return read == size;
#else
  static_cast<void>(bytes);
  return false;
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

bool platform_poll_event(PlatformEvent *outEvent) noexcept {
  if ((outEvent == nullptr) || !SDL_PollEvent(&g_polledEvent)) {
    return false;
  }
  *outEvent = translate_event(g_polledEvent);
  return true;
}

bool platform_translate_native_event(const void *nativeEvent,
                                     PlatformEvent *outEvent) noexcept {
  if ((nativeEvent == nullptr) || (outEvent == nullptr)) {
    return false;
  }
  *outEvent = translate_event(*static_cast<const SDL_Event *>(nativeEvent));
  return true;
}

float platform_display_scale() noexcept {
  if (g_window == nullptr) {
    return 1.0F;
  }
  const float scale = SDL_GetWindowDisplayScale(g_window);
  // SDL reports 0 on failure; a caller multiplying by it would collapse
  // the UI to nothing.
  return (scale > 0.0F) ? scale : 1.0F;
}

bool platform_set_window_title(const char *title) noexcept {
  if ((g_window == nullptr) || (title == nullptr)) {
    return false;
  }
  if (!SDL_SetWindowTitle(g_window, title)) {
    log_sdl_error("failed to set the window title");
    return false;
  }
  return true;
}

bool platform_show_file_dialog(FileDialogKind kind,
                               FileDialogCallback callback, void *userData,
                               const FileDialogFilter *filters,
                               int filterCount,
                               const char *defaultLocation) noexcept {
  if ((callback == nullptr) || (g_window == nullptr)) {
    log_message(LogLevel::Warning, "platform",
                "native file dialog refused: no window to parent it");
    return false;
  }
  if ((filterCount < 0) || (filterCount > kMaxFileDialogFilters) ||
      ((filterCount > 0) && (filters == nullptr))) {
    log_message(LogLevel::Warning, "platform",
                "native file dialog refused: bad filter list");
    return false;
  }

  DialogSlot *slot = nullptr;
  for (DialogSlot &candidate : g_dialogSlots) {
    bool expected = false;
    if (candidate.inUse.compare_exchange_strong(expected, true,
                                                std::memory_order_acquire)) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    log_message(LogLevel::Warning, "platform",
                "native file dialog refused: every dialog slot is held by "
                "a dialog that has not closed");
    return false;
  }

  slot->callback = callback;
  slot->userData = userData;
  for (int i = 0; i < filterCount; ++i) {
    slot->filters[static_cast<std::size_t>(i)] =
        SDL_DialogFileFilter{filters[i].name, filters[i].pattern};
  }
  const SDL_DialogFileFilter *sdlFilters =
      (filterCount > 0) ? slot->filters.data() : nullptr;

  if (kind == FileDialogKind::Save) {
    SDL_ShowSaveFileDialog(&dialog_trampoline, slot, g_window, sdlFilters,
                           filterCount, defaultLocation);
  } else {
    SDL_ShowOpenFileDialog(&dialog_trampoline, slot, g_window, sdlFilters,
                           filterCount, defaultLocation, false);
  }
  return true;
}


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
  char tempPath[kPlatformPathMax] = {};
  const char *candidates[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
  for (const char *candidate : candidates) {
    if (non_empty_env(candidate, tempPath, sizeof(tempPath))) {
      return copy_normalized_path(tempPath, outBuffer, bufferCapacity);
    }
  }
  return copy_normalized_path("/tmp", outBuffer, bufferCapacity);
#endif
}

} // namespace engine::core
