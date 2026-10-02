// The SDL-facing half of the platform layer: SDL and window lifecycle, the
// event pump and its translation to engine events, display scale, mouse
// capture, window geometry, gamepads, capability and native-handle
// reporting, and the application and save directories. The file dialogs and
// the per-OS services live in platform_file_dialogs.cpp and platform_os_*.cpp.

#include "engine/core/platform.h"
#include "engine/core/platform_event.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(ENGINE_PLATFORM_WEB)
#include <emscripten.h>
#endif

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/thread_affinity.h"
#include "platform_internal.h"

namespace engine::core {

namespace platform_detail {

namespace {

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

} // namespace

bool validate_path_output(char *outBuffer,
                          std::size_t bufferCapacity) noexcept {
  if ((outBuffer == nullptr) || (bufferCapacity == 0U)) {
    return false;
  }
  outBuffer[0] = '\0';
  return true;
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
  const bool needsSeparator =
      (baseLength > 0U) && (base[baseLength - 1U] != '/');
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

} // namespace platform_detail

using platform_detail::append_path_segment;
using platform_detail::build_save_base;
using platform_detail::copy_normalized_path;
using platform_detail::g_window;
using platform_detail::kPlatformPathMax;
using platform_detail::log_sdl_error;
using platform_detail::validate_path_output;

namespace {

bool g_platformRunning = false;
/// The window's un-maximized size, followed through resize events while
/// the window is neither maximized nor fullscreen, so a geometry read
/// while maximized reports the size it restores to.
int g_restoredWidth = 0;
int g_restoredHeight = 0;
bool g_headless = false;
int g_presentedFrames = 0;
bool g_windowRevealed = false;
bool g_gamepadSubsystem = false;
/// platform_begin_mouse_capture holds the mouse; whether its first refusal
/// has been logged.
bool g_mouseCaptured = false;
bool g_mouseCaptureRefusalLogged = false;

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
constexpr char kDefaultOrganizationName[] = "Engine";
constexpr char kDefaultApplicationName[] = "Engine";

/// Lets a held mouse go. With `restore`, the cursor is first put at
/// (x, y): SDL records a warp made in relative mode and moves the cursor
/// there as relative mode ends, so it reappears once, in place.
void release_mouse_capture(bool restore, float x, float y) noexcept {
  if (!g_mouseCaptured) {
    return;
  }
  g_mouseCaptured = false;
  if (g_window == nullptr) {
    return;
  }
  // Headless the warp still moves SDL's own record of the cursor, which
  // is how a test sees where a drag put it back.
  if (restore) {
    SDL_WarpMouseInWindow(g_window, x, y);
  }
  if (g_headless) {
    return;
  }
  if (!SDL_SetWindowRelativeMouseMode(g_window, false)) {
    log_sdl_error("failed to release the mouse");
  }
}

/// Shuts down the owning system for platform resources.
void shutdown_platform_resources() noexcept {
  release_mouse_capture(false, 0.0F, 0.0F);
  g_mouseCaptureRefusalLogged = false;
  if (g_window != nullptr) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
  g_restoredWidth = 0;
  g_restoredHeight = 0;
  g_presentedFrames = 0;
  g_windowRevealed = false;
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
  // handles below, and vsync is applied by the backend at its reset. A
  // desktop window starts hidden and is shown by
  // platform_note_frame_presented once the first real frames are on it;
  // the web canvas is part of a page that is already showing, so it is
  // never hidden.
#if defined(__EMSCRIPTEN__)
  constexpr SDL_WindowFlags kStartHidden = 0U;
#else
  constexpr SDL_WindowFlags kStartHidden = SDL_WINDOW_HIDDEN;
#endif
  g_window = SDL_CreateWindow(title, width, height,
                              SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY | kStartHidden);
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
  static_cast<void>(
      SDL_GetWindowSize(g_window, &g_restoredWidth, &g_restoredHeight));
  g_platformRunning = true;
  return true;
}

// SDL numbers its scancodes by the same HID keyboard usage IDs the engine
// uses up to kMaxKeyCode, so the translation passes those through. Every
// named engine key is checked against SDL here, so a divergence fails to
// compile instead of silently rebinding a key.
static_assert(kKey_A == SDL_SCANCODE_A);
static_assert(kKey_B == SDL_SCANCODE_B);
static_assert(kKey_C == SDL_SCANCODE_C);
static_assert(kKey_D == SDL_SCANCODE_D);
static_assert(kKey_E == SDL_SCANCODE_E);
static_assert(kKey_F == SDL_SCANCODE_F);
static_assert(kKey_G == SDL_SCANCODE_G);
static_assert(kKey_H == SDL_SCANCODE_H);
static_assert(kKey_I == SDL_SCANCODE_I);
static_assert(kKey_J == SDL_SCANCODE_J);
static_assert(kKey_K == SDL_SCANCODE_K);
static_assert(kKey_L == SDL_SCANCODE_L);
static_assert(kKey_M == SDL_SCANCODE_M);
static_assert(kKey_N == SDL_SCANCODE_N);
static_assert(kKey_O == SDL_SCANCODE_O);
static_assert(kKey_P == SDL_SCANCODE_P);
static_assert(kKey_Q == SDL_SCANCODE_Q);
static_assert(kKey_R == SDL_SCANCODE_R);
static_assert(kKey_S == SDL_SCANCODE_S);
static_assert(kKey_T == SDL_SCANCODE_T);
static_assert(kKey_U == SDL_SCANCODE_U);
static_assert(kKey_V == SDL_SCANCODE_V);
static_assert(kKey_W == SDL_SCANCODE_W);
static_assert(kKey_X == SDL_SCANCODE_X);
static_assert(kKey_Y == SDL_SCANCODE_Y);
static_assert(kKey_Z == SDL_SCANCODE_Z);
static_assert(kKey_1 == SDL_SCANCODE_1);
static_assert(kKey_2 == SDL_SCANCODE_2);
static_assert(kKey_3 == SDL_SCANCODE_3);
static_assert(kKey_4 == SDL_SCANCODE_4);
static_assert(kKey_5 == SDL_SCANCODE_5);
static_assert(kKey_6 == SDL_SCANCODE_6);
static_assert(kKey_7 == SDL_SCANCODE_7);
static_assert(kKey_8 == SDL_SCANCODE_8);
static_assert(kKey_9 == SDL_SCANCODE_9);
static_assert(kKey_0 == SDL_SCANCODE_0);
static_assert(kKey_Return == SDL_SCANCODE_RETURN);
static_assert(kKey_Escape == SDL_SCANCODE_ESCAPE);
static_assert(kKey_Backspace == SDL_SCANCODE_BACKSPACE);
static_assert(kKey_Tab == SDL_SCANCODE_TAB);
static_assert(kKey_Space == SDL_SCANCODE_SPACE);
static_assert(kKey_F1 == SDL_SCANCODE_F1);
static_assert(kKey_F2 == SDL_SCANCODE_F2);
static_assert(kKey_F3 == SDL_SCANCODE_F3);
static_assert(kKey_F4 == SDL_SCANCODE_F4);
static_assert(kKey_F5 == SDL_SCANCODE_F5);
static_assert(kKey_F6 == SDL_SCANCODE_F6);
static_assert(kKey_F7 == SDL_SCANCODE_F7);
static_assert(kKey_F8 == SDL_SCANCODE_F8);
static_assert(kKey_F9 == SDL_SCANCODE_F9);
static_assert(kKey_F10 == SDL_SCANCODE_F10);
static_assert(kKey_F11 == SDL_SCANCODE_F11);
static_assert(kKey_F12 == SDL_SCANCODE_F12);
static_assert(kKey_Delete == SDL_SCANCODE_DELETE);
static_assert(kKey_Right == SDL_SCANCODE_RIGHT);
static_assert(kKey_Left == SDL_SCANCODE_LEFT);
static_assert(kKey_Down == SDL_SCANCODE_DOWN);
static_assert(kKey_Up == SDL_SCANCODE_UP);
static_assert(kKey_LCtrl == SDL_SCANCODE_LCTRL);
static_assert(kKey_LShift == SDL_SCANCODE_LSHIFT);
static_assert(kKey_LAlt == SDL_SCANCODE_LALT);

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
    // Past kMaxKeyCode SDL numbers keys of its own (media and mode keys);
    // they have no engine key, so they stay Other and reach only the
    // editor's ImGui backend through the native event.
    if (static_cast<int>(event.key.scancode) > kMaxKeyCode) {
      break;
    }
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
#if defined(ENGINE_PLATFORM_WEB)
namespace {

struct HostedLoop final {
  PlatformFrameFn frame = nullptr;
  PlatformLoopEndFn end = nullptr;
  void *context = nullptr;
};
HostedLoop g_hostedLoop{};

/// One requestAnimationFrame tick of the browser-owned loop.
void hosted_frame(void *arg) noexcept {
  auto *loop = static_cast<HostedLoop *>(arg);
  if (!loop->frame(loop->context)) {
    emscripten_cancel_main_loop();
    loop->end(loop->context);
  }
}

} // namespace
#endif

void platform_run_loop(PlatformFrameFn frame, PlatformLoopEndFn end,
                       void *context) noexcept {
#if defined(ENGINE_PLATFORM_WEB)
  // The browser owns the loop: hand the frame to requestAnimationFrame and
  // unwind (simulate_infinite unwinds through the JS event loop; frame
  // pacing collapses into the animation frame).
  g_hostedLoop = HostedLoop{frame, end, context};
  emscripten_set_main_loop_arg(&hosted_frame, &g_hostedLoop, 0, 1);
#else
  while (frame(context)) {
  }
  end(context);
#endif
}

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

bool platform_poll_event(PlatformEvent *outEvent) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if ((outEvent == nullptr) || !SDL_PollEvent(&g_polledEvent)) {
    return false;
  }
  if ((g_polledEvent.type == SDL_EVENT_WINDOW_RESIZED) &&
      (g_window != nullptr) &&
      ((SDL_GetWindowFlags(g_window) &
        (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0U)) {
    g_restoredWidth = static_cast<int>(g_polledEvent.window.data1);
    g_restoredHeight = static_cast<int>(g_polledEvent.window.data2);
  }
  *outEvent = translate_event(g_polledEvent);
  // A window that loses focus gives up the mouse at once, so no drag can
  // leave the cursor hidden and held while the author is elsewhere.
  if (outEvent->kind == PlatformEventKind::WindowFocusLost) {
    release_mouse_capture(false, 0.0F, 0.0F);
  }
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

float content_scale_for(float displayScale, float pixelDensity) noexcept {
  const float display = (displayScale > 0.0F) ? displayScale : 1.0F;
  const float density = (pixelDensity > 0.0F) ? pixelDensity : 1.0F;
  return display / density;
}

float platform_content_scale() noexcept {
  if (g_window == nullptr) {
    return 1.0F;
  }
  return content_scale_for(SDL_GetWindowDisplayScale(g_window),
                           SDL_GetWindowPixelDensity(g_window));
}

void platform_note_frame_presented() noexcept {
  if (g_window == nullptr) {
    return;
  }
  if (g_presentedFrames < kPresentsBeforeWindowShown) {
    ++g_presentedFrames;
  }
  if (g_windowRevealed || (g_presentedFrames < kPresentsBeforeWindowShown)) {
    return;
  }
  g_windowRevealed = true;
  if (!g_headless && !SDL_ShowWindow(g_window)) {
    log_sdl_error("failed to show the window");
  }
}

bool platform_window_revealed() noexcept { return g_windowRevealed; }

bool platform_begin_mouse_capture() noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if (g_mouseCaptured) {
    return true;
  }
  if (g_window == nullptr) {
    return false;
  }
  if (!g_headless && !SDL_SetWindowRelativeMouseMode(g_window, true)) {
    if (!g_mouseCaptureRefusalLogged) {
      g_mouseCaptureRefusalLogged = true;
      char buffer[256] = {};
      std::snprintf(buffer, sizeof(buffer),
                    "the mouse cannot be captured (%s); camera drags stop "
                    "at the screen edge",
                    SDL_GetError());
      log_message(LogLevel::Warning, "platform", buffer);
    }
    return false;
  }
  g_mouseCaptured = true;
  return true;
}

void platform_end_mouse_capture(float x, float y) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  release_mouse_capture(true, x, y);
}

bool platform_mouse_captured() noexcept { return g_mouseCaptured; }

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

WindowGeometry fit_window_geometry(const WindowGeometry &stored,
                                   int usableWidth, int usableHeight) noexcept {
  const auto fit = [](int side, int minimum, int usable) noexcept {
    int fitted = (side > minimum) ? side : minimum;
    if ((usable > 0) && (fitted > usable)) {
      fitted = usable;
    }
    return fitted;
  };
  WindowGeometry fitted = stored;
  fitted.width = fit(stored.width, kMinRestoredWindowWidth, usableWidth);
  fitted.height = fit(stored.height, kMinRestoredWindowHeight, usableHeight);
  return fitted;
}

bool platform_window_geometry(WindowGeometry *outGeometry) noexcept {
  if ((g_window == nullptr) || (outGeometry == nullptr)) {
    return false;
  }
  WindowGeometry geometry{};
  geometry.maximized =
      (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) != 0U;
  if (!geometry.maximized) {
    static_cast<void>(
        SDL_GetWindowSize(g_window, &g_restoredWidth, &g_restoredHeight));
  }
  geometry.width = g_restoredWidth;
  geometry.height = g_restoredHeight;
  *outGeometry = geometry;
  return true;
}

bool platform_apply_window_geometry(const WindowGeometry &geometry) noexcept {
  if ((g_window == nullptr) || (geometry.width <= 0) ||
      (geometry.height <= 0)) {
    return false;
  }
  SDL_Rect usable{};
  const SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
  if ((display == 0U) || !SDL_GetDisplayUsableBounds(display, &usable)) {
    usable = SDL_Rect{};
  }
  const WindowGeometry fitted =
      fit_window_geometry(geometry, usable.w, usable.h);
  if (!SDL_SetWindowSize(g_window, fitted.width, fitted.height)) {
    log_sdl_error("failed to restore the window size");
    return false;
  }
  g_restoredWidth = fitted.width;
  g_restoredHeight = fitted.height;
  static_cast<void>(SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED));
  if (fitted.maximized && !SDL_MaximizeWindow(g_window)) {
    log_sdl_error("failed to restore the maximized window");
  }
  return true;
}

std::uint64_t platform_ticks_ns() noexcept {
  return static_cast<std::uint64_t>(SDL_GetTicksNS());
}

PlatformCaps platform_caps_for(PlatformId id) noexcept {
  PlatformCaps caps{};
  caps.id = id;
  switch (id) {
  case PlatformId::Web:
    // The browser schedules frames and forbids blocking its thread, and
    // audio waits for a user gesture. Threads come from the page's
    // prewarmed pthread pool, whose size the build fixes.
    caps.ownsMainLoop = true;
    caps.needsAudioUnlock = true;
    caps.mainThreadMayBlock = false;
    break;
  case PlatformId::IOS:
    // The display link drives frames, and a main thread blocked too long
    // is killed by the system watchdog.
    caps.ownsMainLoop = true;
    caps.mainThreadMayBlock = false;
    caps.touchPrimary = true;
    break;
  case PlatformId::Android:
    // SDL runs the game on its own thread, not the UI thread, so the
    // engine's loop may block it.
    caps.touchPrimary = true;
    break;
  case PlatformId::Windows:
  case PlatformId::Linux:
  case PlatformId::MacOS:
  default:
    break;
  }
  return caps;
}

PlatformCaps platform_caps() noexcept {
#if defined(ENGINE_PLATFORM_WEB)
  constexpr PlatformId kId = PlatformId::Web;
#elif defined(_WIN32)
  constexpr PlatformId kId = PlatformId::Windows;
#elif defined(SDL_PLATFORM_IOS)
  constexpr PlatformId kId = PlatformId::IOS;
#elif defined(SDL_PLATFORM_ANDROID)
  constexpr PlatformId kId = PlatformId::Android;
#elif defined(__APPLE__)
  constexpr PlatformId kId = PlatformId::MacOS;
#else
  constexpr PlatformId kId = PlatformId::Linux;
#endif
  PlatformCaps caps = platform_caps_for(kId);
  caps.hasWindow = platform_native_window().kind != NativeWindowKind::None;
  return caps;
}

NativeWindow platform_native_window() noexcept {
  NativeWindow native{};
  if ((g_window == nullptr) || g_headless) {
    return native;
  }
#if defined(ENGINE_PLATFORM_WEB)
  // bgfx takes the canvas's CSS selector as the window.
  native.kind = NativeWindowKind::WebCanvas;
  native.window = const_cast<char *>("#canvas");
#else
  const SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
#if defined(_WIN32)
  native.kind = NativeWindowKind::Win32;
  native.window = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_IOS)
  native.kind = NativeWindowKind::UIKit;
  native.window = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_ANDROID)
  native.kind = NativeWindowKind::Android;
  native.window = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#elif defined(__APPLE__)
  native.kind = NativeWindowKind::Cocoa;
  native.window = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
  const char *driver = SDL_GetCurrentVideoDriver();
  if ((driver != nullptr) && (SDL_strcmp(driver, "wayland") == 0)) {
    native.kind = NativeWindowKind::Wayland;
    native.window = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    native.display = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  } else {
    native.kind = NativeWindowKind::X11;
    native.x11Window = static_cast<std::uint64_t>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    native.display = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  }
#endif
#endif
  // A window whose handle the OS would not give is no window to present to.
  if ((native.window == nullptr) && (native.x11Window == 0U)) {
    return NativeWindow{};
  }
  return native;
}

void platform_show_error_box(const char *title, const char *message) noexcept {
  static_cast<void>(SDL_ShowSimpleMessageBox(
      SDL_MESSAGEBOX_ERROR, (title != nullptr) ? title : "Error",
      (message != nullptr) ? message : "", nullptr));
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

} // namespace engine::core
