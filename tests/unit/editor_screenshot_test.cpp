// Verifies the editor's Game view screenshot pieces on a headless ImGui
// frame: the file name (timestamped, suffixed -2 to -99 on a collision,
// refused whole when it does not fit or every name is taken), the Game
// view's rectangle in back-buffer pixels at a HiDPI scale, and the Take
// Screenshot action: run by F9 (also while the game has the keyboard) and
// by the `screenshot` console command, taken the frame after the request,
// never in the frame of the request itself; a request made while the Game
// view is a hidden tab brings it to the front and is taken on its second
// shown frame, and one whose view never shows is dropped with a warning.

#include "editor_screenshot.h"
#include "editor_session.h"
#include "editor_shortcuts.h"

#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"

#include "../test_harness.h"

#include <cstring>
#include <ctime>

namespace {

using namespace engine::editor;

std::size_t g_warnings = 0U;

void count_warnings(engine::core::LogLevel level, const char *channel,
                    const char *, void *) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (channel != nullptr) &&
      (std::strcmp(channel, "editor") == 0)) {
    ++g_warnings;
  }
}

/// A stand-in file system: the first `taken` names exist.
struct TakenNames final {
  int taken = 0;
  int asked = 0;
};

bool first_names_taken(const char *, void *userData) noexcept {
  auto *names = static_cast<TakenNames *>(userData);
  return names->asked++ < names->taken;
}

std::tm sample_time() noexcept {
  std::tm time{};
  time.tm_year = 2026 - 1900;
  time.tm_mon = 8; // September
  time.tm_mday = 28;
  time.tm_hour = 9;
  time.tm_min = 5;
  time.tm_sec = 7;
  return time;
}

void check_names(engine::tests::TestContext &t) noexcept {
  const std::tm time = sample_time();
  char path[128] = {};
  TakenNames none{};
  t.check(next_screenshot_path("shots", time, &first_names_taken, &none, path,
                               sizeof(path)) &&
              (std::strcmp(path, "shots/Screenshot-20260928-090507.png") == 0),
          "the name is the local time, zero-padded");

  TakenNames two{2, 0};
  t.check(next_screenshot_path("shots", time, &first_names_taken, &two, path,
                               sizeof(path)) &&
              (std::strcmp(path, "shots/Screenshot-20260928-090507-3.png") ==
               0),
          "names taken in the same second go on to -2, then -3");

  TakenNames all{99, 0};
  t.check(!next_screenshot_path("shots", time, &first_names_taken, &all, path,
                                sizeof(path)) &&
              (path[0] == '\0') && (all.asked == 99),
          "with all 99 names taken none is chosen");

  // "shots/Screenshot-20260928-090507.png" is 36 characters.
  TakenNames fits{};
  char exact[37] = {};
  t.check(next_screenshot_path("shots", time, &first_names_taken, &fits, exact,
                               sizeof(exact)),
          "a name that fits exactly, terminator included, is written");
  char shortBuffer[36] = {};
  TakenNames overlong{};
  t.check(!next_screenshot_path("shots", time, &first_names_taken, &overlong,
                                shortBuffer, sizeof(shortBuffer)) &&
              (shortBuffer[0] == '\0'),
          "one character short, the name is refused whole, not truncated");
  TakenNames suffixed{1, 0};
  t.check(!next_screenshot_path("shots", time, &first_names_taken, &suffixed,
                                exact, sizeof(exact)) &&
              (exact[0] == '\0'),
          "a suffixed name that no longer fits is refused whole");
}

void check_region(engine::tests::TestContext &t) noexcept {
  // At a framebuffer scale of 2 a point is two pixels; the size truncates
  // as the Game view's render target does (region_pixels).
  const engine::renderer::ScreenshotRegion region =
      game_view_screenshot_region(ImVec2(110.0F, 60.5F), ImVec2(320.75F, 200.25F),
                                  ImVec2(10.0F, 20.0F), ImVec2(2.0F, 2.0F));
  t.check((region.x == 200) && (region.y == 81) && (region.width == 641) &&
              (region.height == 400),
          "the region is the view's rectangle in pixels, viewport-relative");
  const engine::renderer::ScreenshotRegion unscaled =
      game_view_screenshot_region(ImVec2(0.0F, 30.0F), ImVec2(640.0F, 360.0F),
                                  ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
  t.check((unscaled.x == 0) && (unscaled.y == 30) &&
              (unscaled.width == 640) && (unscaled.height == 360),
          "at scale 1 pixels are points");
}

void run_frame() noexcept {
  ImGui::NewFrame();
  dispatch_editor_shortcuts();
  ImGui::Render();
}

/// Presses and releases F9 over two frames with the Game view shown, as
/// the editor draws it each frame: whether either frame took a screenshot.
bool tap_f9() noexcept {
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F9, true);
  run_frame();
  bool taken = begin_game_view_screenshot_frame(true);
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F9, false);
  run_frame();
  taken = begin_game_view_screenshot_frame(true) || taken;
  return taken;
}

/// One headless frame with the Game view `shown` or not: whether it takes
/// the screenshot.
bool game_view_frame(bool shown) noexcept {
  run_frame();
  return begin_game_view_screenshot_frame(shown);
}

void check_action(engine::tests::TestContext &t) noexcept {
  EditorSession &session = editor_session();
  t.check(!game_view_frame(true) && !game_view_frame(true),
          "nothing is taken unasked");
  t.check(std::strcmp(editor_shortcut_text(EditorAction::Screenshot), "F9") ==
              0,
          "Take Screenshot is on F9");
  run_frame();
  t.check(run_editor_action(EditorAction::Screenshot), "the action runs");
  t.check(!begin_game_view_screenshot_frame(true) &&
              !game_view_screenshot_capturing(),
          "not in the frame it was asked for, where a menu may still show");
  run_frame();
  t.check(begin_game_view_screenshot_frame(true) &&
              game_view_screenshot_capturing(),
          "taken on the next frame, which draws nothing over the view");
  t.check(begin_game_view_screenshot_frame(true),
          "the capture frame answers the same when asked again");
  run_frame();
  t.check(!begin_game_view_screenshot_frame(true) &&
              !game_view_screenshot_capturing(),
          "one request, one capture");

  // F9 is live while the game has the keyboard, as Unreal's is in play.
  session.playState = PlayState::Playing;
  session.gameViewFocused = true;
  t.check(tap_f9(), "F9 takes one while the game has the keyboard");
  session.gameViewFocused = false;
  session.playState = PlayState::Stopped;
  run_frame();

  // The Game view is a tab behind the Scene view: the request brings it
  // to the front, and waits for the frame after it first shows, when it
  // has rendered at its size.
  t.check(!game_view_frame(false), "hidden");
  t.check(run_editor_action(EditorAction::Screenshot),
          "the action runs while the Game view is hidden");
  session.pendingViewFocus = nullptr;
  t.check(!game_view_frame(false) &&
              (session.pendingViewFocus != nullptr) &&
              (std::strcmp(session.pendingViewFocus, kGameViewWindow) == 0),
          "a hidden Game view is brought to the front");
  session.pendingViewFocus = nullptr;
  t.check(!game_view_frame(true), "not on its first shown frame");
  t.check(game_view_frame(true), "taken on its second");
  t.check(!game_view_frame(true), "once");

  g_warnings = 0U;
  t.check(run_editor_action(EditorAction::Screenshot), "asked again");
  bool taken = false;
  for (int frame = 0; frame < 12; ++frame) {
    taken = taken || game_view_frame(false);
  }
  t.check(!taken && (g_warnings == 1U),
          "a request whose view never shows is dropped, warned once");
  t.check(!game_view_frame(true) && !game_view_frame(true),
          "and not taken later");
  session.pendingViewFocus = nullptr;
}

void check_console_command(engine::tests::TestContext &t) noexcept {
  t.check(register_screenshot_command() && !register_screenshot_command(),
          "the command registers, once");
  t.check(!game_view_frame(true), "shown");
  t.check(engine::core::console_execute("screenshot"), "the command runs");
  t.check(game_view_frame(true), "it takes the Game view on the next frame");
  t.check(!game_view_frame(true), "once");
}

} // namespace

int main() {
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_warnings, nullptr) ||
      !engine::core::initialize_cvars() ||
      !engine::core::initialize_console()) {
    return 1;
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 0.05F;
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  engine::tests::TestContext t;
  check_names(t);
  check_region(t);
  check_action(t);
  check_console_command(t);

  ImGui::DestroyContext();
  engine::core::shutdown_console();
  engine::core::shutdown_cvars();
  engine::core::log_unregister_sink(&count_warnings, nullptr);
  engine::core::shutdown_logging();
  return t.finish("editor_screenshot");
}
