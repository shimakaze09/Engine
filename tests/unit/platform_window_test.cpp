// The window surface the editor now reaches through the platform instead of
// holding SDL_Window itself (#312 items 2-3): title, display scale, and the
// refusal paths of the native file dialog -- plus the event translation
// that replaced SDL_Event above the platform layer (#312 item 1).
//
// A real dialog cannot open in CI -- there is no portal or desktop to show
// it -- so the dialog cases here are the refusals, which are the ones a
// caller has to get right: a refused dialog never calls back, and the
// editor turns that into a cancel so nothing waits on it forever.

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/platform_event.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool g_called = false;

void record_call(void * /*userData*/, const char * /*path*/) noexcept {
  g_called = true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  using namespace engine::core;
  static_cast<void>(initialize_logging());

  const FileDialogFilter filter{"Scene", "scene"};

  // Before the platform exists there is no window: every call must say so
  // rather than dereference a null one.
  CHECK(platform_display_scale() == 1.0F,
        "display scale is 1.0 with no window, so callers can multiply by it");
  CHECK(!platform_set_window_title("no window"),
        "setting a title with no window is refused");
  CHECK(!platform_show_file_dialog(FileDialogKind::Open, &record_call,
                                   nullptr, &filter, 1, nullptr),
        "a dialog with no window to parent it is refused");
  CHECK(!g_called, "a refused dialog never calls back");

  // --- Event translation: the conversions that are easy to get wrong.
  CHECK(!platform_translate_native_event(nullptr, nullptr),
        "a null native event is refused");
  {
    // SDL numbers mouse buttons from 1, the engine from 0.
    SDL_Event native{};
    native.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    native.button.button = SDL_BUTTON_RIGHT;
    native.button.x = 12.5F;
    native.button.y = 7.0F;
    PlatformEvent event{};
    CHECK(platform_translate_native_event(&native, &event), "translates");
    CHECK(event.kind == PlatformEventKind::MouseButtonDown, "button kind");
    CHECK(event.mouseButton == 2, "the right button is engine button 2");
    CHECK((event.x == 12.5F) && (event.y == 7.0F),
          "the cursor rides on the button event");
  }
  {
    SDL_Event native{};
    native.type = SDL_EVENT_KEY_DOWN;
    native.key.scancode = SDL_SCANCODE_W;
    native.key.repeat = true;
    native.common.timestamp = 123456789U;
    PlatformEvent event{};
    CHECK(platform_translate_native_event(&native, &event), "translates");
    CHECK(event.kind == PlatformEventKind::KeyDown, "key kind");
    CHECK(event.scancode == static_cast<int>(SDL_SCANCODE_W),
          "scancodes keep their numbering");
    CHECK(event.repeat, "OS auto-repeat is carried, not mistaken for a press");
    CHECK(event.timestampNs == 123456789U, "the timestamp is carried");
  }
  {
    // The engine's key vocabulary is the HID keyboard page (#312 item 5).
    // A key SDL numbers past it -- its own media and mode keys -- has no
    // engine key, so it must not reach input as one: before, it arrived
    // as KeyDown with SDL's private number, which a bindings file then
    // persisted as if it meant something outside SDL.
    SDL_Event native{};
    native.type = SDL_EVENT_KEY_DOWN;
    native.key.scancode = SDL_SCANCODE_MEDIA_PLAY;
    PlatformEvent event{};
    CHECK(platform_translate_native_event(&native, &event), "translates");
    CHECK(event.kind == PlatformEventKind::Other,
          "a key outside the HID keyboard page is not an engine key");
    CHECK(event.native == &native, "it still reaches the ImGui backend");

    native.key.scancode = static_cast<SDL_Scancode>(kMaxKeyCode);
    CHECK(platform_translate_native_event(&native, &event), "translates");
    CHECK((event.kind == PlatformEventKind::KeyDown) &&
              (event.scancode == kMaxKeyCode),
          "the last usage on the page is still a key");
  }
  {
    // An event the engine does not model still arrives, as Other with its
    // native event attached: the editor's ImGui backend reads those, and
    // dropping them would lose hover, clipboard and IME there.
    SDL_Event native{};
    native.type = SDL_EVENT_WINDOW_MOUSE_ENTER;
    PlatformEvent event{};
    CHECK(platform_translate_native_event(&native, &event), "translates");
    CHECK(event.kind == PlatformEventKind::Other, "unmodelled is Other");
    CHECK(event.native == &native, "the native event rides along");
  }
  {
    SDL_Event native{};
    native.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    native.gaxis.which = 77;
    native.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    native.gaxis.value = -32768;
    PlatformEvent event{};
    CHECK(platform_translate_native_event(&native, &event), "translates");
    CHECK(event.kind == PlatformEventKind::GamepadAxis, "axis kind");
    CHECK(event.deviceId == 77U, "the device id is the instance id");
    CHECK(event.gamepadAxis == kGamepadAxis_RightTrigger,
          "axes are the engine's numbering");
    CHECK(event.axisValue == -32768, "the full negative range survives");
  }

  PlatformConfig config{};
  config.headless = true;
  config.title = "platform window test";
  if (!initialize_platform(config)) {
    std::fprintf(stderr, "FAIL: headless platform did not initialize\n");
    return 1;
  }

  const float scale = platform_display_scale();
  CHECK(scale > 0.0F, "a window reports a positive display scale");
  CHECK(platform_set_window_title("platform window test - renamed"),
        "a window accepts a title");
  CHECK(!platform_set_window_title(nullptr), "a null title is refused");

  // Refusals that do not depend on a dialog backend.
  CHECK(!platform_show_file_dialog(FileDialogKind::Open, nullptr, nullptr,
                                   &filter, 1, nullptr),
        "a dialog with no callback is refused");
  CHECK(!platform_show_file_dialog(FileDialogKind::Save, &record_call,
                                   nullptr, nullptr, 1, nullptr),
        "a filter count with no filter list is refused");
  CHECK(!platform_show_file_dialog(FileDialogKind::Save, &record_call,
                                   nullptr, &filter,
                                   kMaxFileDialogFilters + 1, nullptr),
        "more filters than a dialog carries is refused");
  CHECK(!platform_show_file_dialog(FileDialogKind::Save, &record_call,
                                   nullptr, &filter, -1, nullptr),
        "a negative filter count is refused");
  CHECK(!g_called, "no refused dialog called back");

  shutdown_platform();
  CHECK(!platform_set_window_title("after shutdown"),
        "the window is gone after shutdown");
  shutdown_logging();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("platform_window_test passed");
  return 0;
}
