// Regression for #241's editor half: the quit-request bridge hook must
// route a live play session through the Stop flow (on_end_play dispatch
// and authored-world restore then follow the ordinary Stop machinery in
// the pipeline) before the unsaved-change check runs — before the fix,
// window-close during play left the session Playing and skipped the flow.

#include "editor_session.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

namespace engine::editor {
// Production bridge hook under test (registered into the runtime bridge by
// editor.cpp's static initializer; declared here because the hook is
// reached through the bridge struct, not a public header).
bool editor_handle_quit_request() noexcept;
} // namespace engine::editor

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

} // namespace

/// Runs this executable or test program.
int main() {
  using namespace engine::editor;

  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  CHECK(world != nullptr, "world allocation");

  editor_set_world(world.get());
  editor_session().initialized = true;

  start_play_mode();
  CHECK(editor_session().playState == PlayState::Playing,
        "session is playing before quit");

  // The production quit hook: clean document, so it must proceed — but
  // only after routing through the Stop flow.
  CHECK(editor_handle_quit_request(), "clean document proceeds with quit");
  CHECK(editor_session().playState == PlayState::Stopped,
        "quit request stopped the play session");
  CHECK(!editor_session().worldRestoreFailed,
        "the authored world restored on the way out");

  editor_session().initialized = false;
  editor_set_world(nullptr);

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("editor_quit_play_test passed");
  return 0;
}
