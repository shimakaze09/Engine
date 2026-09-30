// Verifies the Log panel's command line. The history keeps what was
// entered, skipping an empty line and an immediate repeat, and drops the
// oldest past its capacity; Up and Down step through it and back to a
// fresh line. Tab completes a command name, or a cvar name after `set` and
// `get`, whole when one name matches and to the shared prefix when several
// do. Driven through the real Log panel under a headless ImGui context:
// typing `help` and Enter runs the console command, and the echoed line
// and its output land in the Log; Tab completes the typed word in place;
// Up recalls the last command.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../test_harness.h"
#include "editor_console_capture.h"
#include "editor_console_commands.h"
#include "editor_panels_console.h"
#include "editor_session.h"
#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"

namespace {

using engine::editor::complete_console_line;
using engine::editor::ConsoleHistory;
using engine::editor::kConsoleCommandCapacity;
using engine::editor::kConsoleHistoryCapacity;

engine::tests::TestContext g_tests;

bool same(const char *a, const char *b) noexcept {
  return (a != nullptr) && (b != nullptr) && (std::strcmp(a, b) == 0);
}

void check_history() noexcept {
  ConsoleHistory history{};
  g_tests.check((history.size() == 0U) && (history.older() == nullptr) &&
                    (history.newer() == nullptr),
                "an empty history recalls nothing");

  history.push("help");
  g_tests.check((history.size() == 1U) && same(history.older(), "help"),
                "one command is recalled");
  g_tests.check(same(history.older(), "help"),
                "Up stays on the oldest command");
  g_tests.check(same(history.newer(), ""),
                "Down past the newest returns a fresh line");
  g_tests.check(history.newer() == nullptr, "and then stops browsing");

  history.push("");
  history.push("help");
  g_tests.check(history.size() == 1U,
                "an empty line and an immediate repeat are not recorded");

  history.push("get r_bloom");
  history.push("set r_bloom false");
  g_tests.check(same(history.older(), "set r_bloom false") &&
                    same(history.older(), "get r_bloom") &&
                    same(history.older(), "help") &&
                    same(history.newer(), "get r_bloom"),
                "Up and Down walk the commands newest first");

  const std::string overlong(kConsoleCommandCapacity, 'x');
  history.push(overlong.c_str());
  g_tests.check(history.size() == 3U,
                "a line longer than the command line holds is not recorded");

  ConsoleHistory full{};
  char line[16] = {};
  for (std::size_t i = 0U; i <= kConsoleHistoryCapacity; ++i) {
    std::snprintf(line, sizeof(line), "cmd%zu", i);
    full.push(line);
  }
  g_tests.check(full.size() == kConsoleHistoryCapacity,
                "the history holds its capacity");
  g_tests.check(same(full.at(0U), "cmd1") &&
                    same(full.at(kConsoleHistoryCapacity - 1U), "cmd32"),
                "one past capacity drops the oldest command");
}

void check_completion() noexcept {
  char out[kConsoleCommandCapacity] = {};
  char candidates[256] = {};

  g_tests.check((complete_console_line("hel", out, sizeof(out), candidates,
                                       sizeof(candidates)) == 1U) &&
                    same(out, "help "),
                "one matching command completes whole");

  static_cast<void>(engine::core::console_register_command(
      "spawn_test_a", [](const char *const *, int, void *) noexcept {}, nullptr,
      "test"));
  static_cast<void>(engine::core::console_register_command(
      "spawn_test_b", [](const char *const *, int, void *) noexcept {}, nullptr,
      "test"));
  g_tests.check((complete_console_line("spawn_t", out, sizeof(out), candidates,
                                       sizeof(candidates)) == 2U) &&
                    same(out, "spawn_test_") &&
                    same(candidates, "spawn_test_a  spawn_test_b"),
                "several matches complete to their shared prefix and list");

  g_tests.check((complete_console_line("zzz", out, sizeof(out), candidates,
                                       sizeof(candidates)) == 0U) &&
                    same(out, "zzz"),
                "no match leaves the line as it was");

  g_tests.check((complete_console_line("set console.test.bl", out, sizeof(out),
                                       candidates, sizeof(candidates)) == 1U) &&
                    same(out, "set console.test.bloom "),
                "after set, a cvar name completes");
  g_tests.check((complete_console_line("get console.test.bl", out, sizeof(out),
                                       candidates, sizeof(candidates)) == 1U) &&
                    same(out, "get console.test.bloom "),
                "after get, a cvar name completes");
  g_tests.check((complete_console_line("help con", out, sizeof(out), candidates,
                                       sizeof(candidates)) == 0U) &&
                    same(out, "help con"),
                "only set and get take a cvar name");

  char tiny[4] = {};
  g_tests.check((complete_console_line("hel", tiny, sizeof(tiny), candidates,
                                       sizeof(candidates)) == 0U) &&
                    same(tiny, "hel"),
                "a completion that does not fit is refused whole");
}

/// One frame of the Log panel, the keyboard in its command line when
/// `focus` is set.
void panel_frame(bool focus = false) noexcept {
  engine::editor::editor_session().console.focusCommandLine = focus;
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(900.0F, 500.0F));
  engine::editor::draw_console_panel();
  ImGui::Render();
}

void type_text(const char *text) noexcept {
  for (const char *c = text; *c != '\0'; ++c) {
    ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(*c));
    panel_frame();
  }
}

void press(ImGuiKey key) noexcept {
  ImGui::GetIO().AddKeyEvent(key, true);
  panel_frame();
  ImGui::GetIO().AddKeyEvent(key, false);
  panel_frame();
}

/// True when the Log holds an entry on the console channel reading `text`.
bool log_holds(const char *text) noexcept {
  const std::size_t count = engine::editor::console_capture_entry_count();
  for (std::size_t i = 0U; i < count; ++i) {
    engine::editor::ConsoleEntry entry{};
    if (engine::editor::console_capture_get_entry(i, &entry) &&
        same(entry.channel, "console") && same(entry.message, text)) {
      return true;
    }
  }
  return false;
}

void check_panel() noexcept {
  const char *line = engine::editor::editor_session().console.commandLine;

  // Focus lands on the frame after it is asked for.
  panel_frame(true);
  panel_frame();
  type_text("help");
  press(ImGuiKey_Enter);
  g_tests.check(log_holds("> help") && log_holds("Registered commands:"),
                "Enter runs the command and its output reaches the Log");
  g_tests.check(line[0] == '\0', "the command line clears after Enter");

  type_text("hel");
  press(ImGuiKey_Tab);
  g_tests.check(same(line, "help "), "Tab completes the typed command");

  // The completed line runs, and the field keeps the keyboard for the next.
  press(ImGuiKey_Enter);
  g_tests.check(line[0] == '\0', "the completed line runs");
  press(ImGuiKey_UpArrow);
  g_tests.check(same(line, "help "), "Up recalls the last command");
  press(ImGuiKey_UpArrow);
  g_tests.check(same(line, "help"), "Up again recalls the one before");
  press(ImGuiKey_DownArrow);
  press(ImGuiKey_DownArrow);
  g_tests.check(line[0] == '\0', "Down past the newest returns a fresh line");
}

int g_worldCommandRuns = 0;

/// A command that changes the World runs from the Log's command line only
/// in a play session (#1091): in Edit mode it would change the authored
/// scene outside undo and the unsaved-change prompt.
void check_world_commands() noexcept {
  engine::editor::EditorSession &session = engine::editor::editor_session();
  g_tests.check(engine::core::console_register_world_command(
                    "world_test",
                    [](const char *const *, int, void *) noexcept {
                      ++g_worldCommandRuns;
                    },
                    nullptr, "changes the world"),
                "a world command registers");
  g_tests.check(engine::core::console_line_changes_world("world_test 1 2") &&
                    !engine::core::console_line_changes_world("help"),
                "the console knows which commands change the world");

  session.playState = engine::editor::PlayState::Stopped;
  type_text("world_test");
  press(ImGuiKey_Enter);
  g_tests.check(g_worldCommandRuns == 0,
                "in Edit mode the Log refuses a command that changes the "
                "world");
  g_tests.check(log_holds("> world_test"),
                "the refused line is echoed so the author sees what ran");

  session.playState = engine::editor::PlayState::Playing;
  type_text("world_test");
  press(ImGuiKey_Enter);
  g_tests.check(g_worldCommandRuns == 1, "in Play the command runs");
  session.playState = engine::editor::PlayState::Stopped;
}

} // namespace

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  static_cast<void>(engine::core::initialize_logging());
  static_cast<void>(engine::core::initialize_cvars());
  static_cast<void>(engine::core::initialize_console());
  static_cast<void>(engine::core::cvar_register_bool("console.test.bloom", true,
                                                     "completion test cvar"));
  engine::editor::console_capture_initialize();

  check_history();
  check_completion();
  check_panel();
  check_world_commands();

  engine::editor::console_capture_shutdown();
  engine::core::shutdown_console();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  ImGui::DestroyContext();
  return g_tests.finish("editor_console_commands");
}
