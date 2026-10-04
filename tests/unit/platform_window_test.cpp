// The window surface the editor now reaches through the platform instead of
// holding SDL_Window itself (#312 items 2-3): title, display scale, the
// window geometry a layout restores, when a hidden window is first shown
// (#741), the file-dialog handoff, the mouse capture a camera drag holds
// -- plus the event translation that replaced SDL_Event above the
// platform layer (#312 item 1).
//
// A real dialog cannot open in CI -- there is no portal or desktop to show
// it. The dialog cases here are the refusals, and the handoff driven by
// scripted dialogs, which answer through the same delivery a native
// dialog uses: an answer from another thread is only ever seen by the
// requester on its own thread, once, and an abandoned or spent ticket
// never yields a result.

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/platform_event.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Asks for a scripted dialog; scripted mode is on for the caller.
engine::core::FileDialogTicket request_scripted() noexcept {
  using namespace engine::core;
  static const FileDialogFilter kFilter{"Scene", "scene"};
  return platform_request_file_dialog(FileDialogKind::Open, &kFilter, 1,
                                      nullptr);
}

/// The dialog handoff, end to end, with no window.
void check_dialog_handoff() {
  using namespace engine::core;
  platform_set_scripted_file_dialogs(true);

  // An answer from another thread is held until the requester takes it.
  const FileDialogTicket ticket = request_scripted();
  CHECK(ticket != kNoFileDialog, "a scripted dialog is granted a ticket");
  FileDialogResult result{};
  CHECK(platform_take_file_dialog_result(ticket, &result) ==
            FileDialogPoll::Pending,
        "an unanswered dialog is pending");
  bool answered = false;
  std::thread answerer([&answered, ticket]() {
    answered = platform_answer_scripted_file_dialog(ticket, "/tmp/a.scene");
  });
  answerer.join();
  CHECK(answered, "the answer from another thread is accepted");
  CHECK(platform_take_file_dialog_result(ticket, &result) ==
            FileDialogPoll::Ready,
        "the requester takes the answer on its own thread");
  CHECK((result.ticket == ticket) &&
            (result.outcome == FileDialogOutcome::Chosen) &&
            (std::strcmp(result.path, "/tmp/a.scene") == 0),
        "the taken result is the answer");
  CHECK(platform_take_file_dialog_result(ticket, &result) ==
            FileDialogPoll::Unknown,
        "a taken ticket is spent");
  CHECK(!platform_answer_scripted_file_dialog(ticket, "/tmp/b.scene"),
        "a spent ticket takes no second answer");

  // A cancel is a result too, with no path.
  const FileDialogTicket cancelled = request_scripted();
  CHECK(platform_answer_scripted_file_dialog(cancelled, nullptr),
        "a cancel is accepted");
  CHECK((platform_take_file_dialog_result(cancelled, &result) ==
         FileDialogPoll::Ready) &&
            (result.outcome == FileDialogOutcome::Cancelled) &&
            (result.path[0] == '\0'),
        "a cancel arrives as Cancelled with an empty path");

  // A folder dialog answers with a directory the same way.
  const FileDialogTicket folder =
      platform_request_file_dialog(FileDialogKind::Folder, nullptr, 0, nullptr);
  CHECK((folder != kNoFileDialog) &&
            platform_answer_scripted_file_dialog(folder, "/tmp/projects") &&
            (platform_take_file_dialog_result(folder, &result) ==
             FileDialogPoll::Ready) &&
            (result.outcome == FileDialogOutcome::Chosen) &&
            (std::strcmp(result.path, "/tmp/projects") == 0),
        "a folder dialog with no filters answers with the folder chosen");

  // A Windows answer arrives in the engine's one spelling, so it names the
  // same file as a path the engine built itself (#1217).
  const FileDialogTicket native = request_scripted();
  CHECK((native != kNoFileDialog) &&
            platform_answer_scripted_file_dialog(
                native, "C:\\dev\\game\\assets\\scenes\\main.scene") &&
            (platform_take_file_dialog_result(native, &result) ==
             FileDialogPoll::Ready) &&
            (std::strcmp(result.path, "C:/dev/game/assets/scenes/main.scene") ==
             0),
        "a backslash answer is delivered with '/' separators");
  const FileDialogTicket nativeFolder =
      platform_request_file_dialog(FileDialogKind::Folder, nullptr, 0, nullptr);
  CHECK((nativeFolder != kNoFileDialog) &&
            platform_answer_scripted_file_dialog(nativeFolder,
                                                 "D:\\dev\\projects\\") &&
            (platform_take_file_dialog_result(nativeFolder, &result) ==
             FileDialogPoll::Ready) &&
            (std::strcmp(result.path, "D:/dev/projects") == 0),
        "a folder answer loses its trailing separator");

  // A path that does not fit is refused, not cut.
  static char longPath[kMaxFileDialogPathLength + 1U] = {};
  std::memset(longPath, 'a', kMaxFileDialogPathLength);
  longPath[0] = '/';
  const FileDialogTicket overlong = request_scripted();
  CHECK(platform_answer_scripted_file_dialog(overlong, longPath),
        "an overlong answer is still an answer");
  CHECK((platform_take_file_dialog_result(overlong, &result) ==
         FileDialogPoll::Ready) &&
            (result.outcome == FileDialogOutcome::PathTooLong) &&
            (result.path[0] == '\0'),
        "an overlong path arrives as PathTooLong with no prefix of it");

  // Every slot held: refused. Abandoning keeps a slot until the dialog
  // answers, since the OS dialog is still open; the answer frees it.
  FileDialogTicket held[kMaxPendingFileDialogs] = {};
  for (FileDialogTicket &slot : held) {
    slot = request_scripted();
    CHECK(slot != kNoFileDialog, "a free slot grants a ticket");
  }
  CHECK(request_scripted() == kNoFileDialog,
        "with every slot held, a dialog is refused");
  platform_abandon_file_dialog(held[0]);
  platform_abandon_file_dialog(held[0]);
  CHECK(request_scripted() == kNoFileDialog,
        "an abandoned dialog that has not answered keeps its slot");
  CHECK(platform_take_file_dialog_result(held[0], &result) ==
            FileDialogPoll::Unknown,
        "an abandoned ticket yields no result");
  CHECK(platform_answer_scripted_file_dialog(held[0], "/tmp/late.scene"),
        "the abandoned dialog can still answer");
  CHECK(platform_take_file_dialog_result(held[0], &result) ==
            FileDialogPoll::Unknown,
        "an abandoned dialog's answer is dropped");
  const FileDialogTicket reused = request_scripted();
  CHECK((reused != kNoFileDialog) && (reused != held[0]),
        "the answer frees the slot, under a new ticket");
  CHECK(!platform_answer_scripted_file_dialog(held[0], "/tmp/stale.scene"),
        "a stale ticket cannot answer the slot's new request");

  // Abandoning an answered dialog frees its slot at once.
  CHECK(platform_answer_scripted_file_dialog(held[1], "/tmp/c.scene"),
        "a held dialog answers");
  platform_abandon_file_dialog(held[1]);
  const FileDialogTicket afterAbandon = request_scripted();
  CHECK(afterAbandon != kNoFileDialog,
        "abandoning an answered dialog frees its slot");

  for (const FileDialogTicket open : {reused, afterAbandon, held[2], held[3]}) {
    platform_abandon_file_dialog(open);
    static_cast<void>(platform_answer_scripted_file_dialog(open, nullptr));
  }
  platform_set_scripted_file_dialogs(false);
  CHECK(!platform_answer_scripted_file_dialog(kNoFileDialog, nullptr),
        "the null ticket takes no answer");
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
  CHECK(!platform_caps().hasWindow,
        "the capabilities report no window before the platform exists");
  CHECK(platform_content_scale() == 1.0F,
        "content scale is 1.0 with no window");
  {
    WindowGeometry none{};
    CHECK(!platform_window_geometry(&none), "no geometry without a window");
    CHECK(!platform_apply_window_geometry(WindowGeometry{1600, 900, false}),
          "no geometry is applied without a window");
  }

  // A stored geometry fitted to the display it reopens on (#593): kept as
  // stored when it fits, capped at the usable area of a smaller display,
  // raised to the minimum when it was saved tiny, and unbounded above on a
  // side the display could not report.
  {
    const WindowGeometry kept =
        fit_window_geometry(WindowGeometry{1600, 900, true}, 3840, 2100);
    CHECK((kept.width == 1600) && (kept.height == 900) && kept.maximized,
          "a geometry that fits is restored as stored");
    const WindowGeometry capped =
        fit_window_geometry(WindowGeometry{3000, 2000, false}, 1920, 1040);
    CHECK((capped.width == 1920) && (capped.height == 1040),
          "a geometry from a larger display is capped to this one");
    const WindowGeometry raised =
        fit_window_geometry(WindowGeometry{100, 50, false}, 1920, 1040);
    CHECK((raised.width == kMinRestoredWindowWidth) &&
              (raised.height == kMinRestoredWindowHeight),
          "a geometry saved tiny opens at the minimum");
    const WindowGeometry unbounded =
        fit_window_geometry(WindowGeometry{2500, 1500, false}, 0, -1);
    CHECK((unbounded.width == 2500) && (unbounded.height == 1500),
          "a display that reports no usable area bounds nothing");
  }

  // Content scale per platform shape: the pixel density the renderer
  // already applies is divided out of the display scale.
  CHECK(content_scale_for(2.0F, 2.0F) == 1.0F,
        "Retina: two pixels per unit, no extra UI scale");
  CHECK(content_scale_for(1.5F, 1.0F) == 1.5F,
        "Windows at 150%: window units are pixels, scale 1.5");
  CHECK(content_scale_for(4.0F, 2.0F) == 2.0F,
        "Retina plus a 200% content scale keeps the 2");
  CHECK(content_scale_for(1.0F, 1.0F) == 1.0F, "reference density");
  CHECK((content_scale_for(0.0F, 2.0F) == 0.5F) &&
            (content_scale_for(2.0F, 0.0F) == 2.0F) &&
            (content_scale_for(-1.0F, -1.0F) == 1.0F),
        "a factor the platform could not report counts as 1");
  CHECK(platform_request_file_dialog(FileDialogKind::Open, &filter, 1,
                                     nullptr) == kNoFileDialog,
        "a dialog with no window to parent it is refused");
  check_dialog_handoff();

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

  CHECK(!platform_caps().hasWindow,
        "a headless window is not one a GPU backend can present to");

  // The window stays hidden until its first real frames are on it (#741):
  // shown on the second present, once, and never hidden again by later
  // presents.
  platform_note_frame_presented();
  CHECK(!platform_window_revealed(), "one present does not show the window");
  platform_note_frame_presented();
  CHECK(platform_window_revealed(), "the second present shows the window");
  platform_note_frame_presented();
  CHECK(platform_window_revealed(), "later presents keep it shown");
  const float scale = platform_display_scale();
  CHECK(scale > 0.0F, "a window reports a positive display scale");

  // Mouse capture for camera drags: held until let go, let go by a lost
  // window focus whoever held it, and never held past shutdown.
  CHECK(!platform_mouse_captured(), "the mouse starts free");
  CHECK(platform_begin_mouse_capture(), "a window captures the mouse");
  CHECK(platform_mouse_captured(), "the capture is held");
  CHECK(platform_begin_mouse_capture(), "a second capture is the same one");
  platform_end_mouse_capture(10.0F, 20.0F);
  CHECK(!platform_mouse_captured(), "ending the capture lets the mouse go");
  platform_end_mouse_capture(10.0F, 20.0F);
  CHECK(!platform_mouse_captured(), "ending it twice is harmless");
  CHECK(platform_begin_mouse_capture(), "the mouse is captured again");
  {
    SDL_Event focusLost{};
    focusLost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    CHECK(SDL_PushEvent(&focusLost), "a focus loss is queued");
    PlatformEvent polled{};
    bool sawFocusLost = false;
    while (platform_poll_event(&polled)) {
      sawFocusLost =
          sawFocusLost || (polled.kind == PlatformEventKind::WindowFocusLost);
    }
    CHECK(sawFocusLost, "the focus loss is delivered");
    CHECK(!platform_mouse_captured(), "a lost focus lets the mouse go");
  }
  CHECK(platform_begin_mouse_capture(), "captured before shutdown");
  CHECK(platform_set_window_title("platform window test - renamed"),
        "a window accepts a title");
  CHECK(!platform_set_window_title(nullptr), "a null title is refused");

  // The geometry round trip a layout file makes: what is applied is what
  // is read back, and a geometry with no size is refused.
  {
    WindowGeometry opened{};
    CHECK(platform_window_geometry(&opened) && (opened.width > 0) &&
              (opened.height > 0),
          "a window reports its geometry");
    CHECK(platform_apply_window_geometry(WindowGeometry{1000, 700, false}),
          "a stored geometry is applied");
    WindowGeometry restored{};
    CHECK(platform_window_geometry(&restored) && (restored.width == 1000) &&
              (restored.height == 700) && !restored.maximized,
          "the applied size is the one read back");
    CHECK(!platform_apply_window_geometry(WindowGeometry{0, 700, false}),
          "a geometry with no size is refused");
  }

  // Refusals that do not depend on a dialog backend.
  CHECK(platform_request_file_dialog(FileDialogKind::Save, nullptr, 1,
                                     nullptr) == kNoFileDialog,
        "a filter count with no filter list is refused");
  CHECK(platform_request_file_dialog(FileDialogKind::Save, &filter,
                                     kMaxFileDialogFilters + 1,
                                     nullptr) == kNoFileDialog,
        "more filters than a dialog carries is refused");
  CHECK(platform_request_file_dialog(FileDialogKind::Save, &filter, -1,
                                     nullptr) == kNoFileDialog,
        "a negative filter count is refused");
  CHECK(platform_request_file_dialog(FileDialogKind::Folder, &filter, 1,
                                     nullptr) == kNoFileDialog,
        "a folder dialog with a filter is refused");

  shutdown_platform();
  CHECK(!platform_mouse_captured(), "shutdown lets the mouse go");
  CHECK(!platform_begin_mouse_capture(), "no window, no capture");
  CHECK(!platform_window_revealed(), "a fresh platform starts hidden");
  platform_note_frame_presented();
  platform_note_frame_presented();
  CHECK(!platform_window_revealed(), "no window, nothing is shown");
  CHECK(!platform_set_window_title("after shutdown"),
        "the window is gone after shutdown");
  {
    WindowGeometry after{};
    CHECK(!platform_window_geometry(&after), "no geometry after shutdown");
  }
  shutdown_logging();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("platform_window_test passed");
  return 0;
}
