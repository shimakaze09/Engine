// The window surface the editor now reaches through the platform instead of
// holding SDL_Window itself (#312 items 2-3): title, display scale, and the
// refusal paths of the native file dialog.
//
// A real dialog cannot open in CI -- there is no portal or desktop to show
// it -- so the dialog cases here are the refusals, which are the ones a
// caller has to get right: a refused dialog never calls back, and the
// editor turns that into a cancel so nothing waits on it forever.

#include "engine/core/logging.h"
#include "engine/core/platform.h"

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
