// Verifies the right-click menus of the Log, the Inspector and the Game
// view through their production panels on headless ImGui frames, each
// offering its own window's actions and none of another's. Every
// right-click in the Log offers Clear, Copy All and its Collapse,
// Autoscroll and Pause toggles, a line's menu with its own Copy Message
// above them, and Clear empties the log. The Inspector's empty space
// offers Add Component, by category, and Paste Component As New, enabled
// only while the component clipboard holds a component the entity lacks;
// pasting adds it as one undo step. The Game view's toolbar row offers
// Take Screenshot, the recording items and the Stats overlay toggle,
// while a right-click on its image, which belongs to the game, opens
// nothing. The Log's toolbar and the Inspector no longer repeat what the
// menus and the Delete action offer: no Copy All button, no Delete Entity.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <memory>
#include <new>
#include <string>

#include "../test_harness.h"
#include "editor_component_ops.h"
#include "editor_console_capture.h"
#include "editor_panels_console.h"
#include "editor_panels_diagnostics.h"
#include "editor_panels_inspector.h"
#include "editor_panels_viewport.h"
#include "editor_session.h"
#include "engine/core/console.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

namespace {

using namespace engine::editor;
using engine::runtime::Entity;
using engine::runtime::World;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

using PanelFn = void (*)();

/// Draws one frame of `panel` at a fixed place and returns the text it
/// and its menus rendered.
std::string frame(PanelFn panel) noexcept {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(700.0F, 710.0F));
  panel();
  const std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

void click(PanelFn panel, ImVec2 at, ImGuiMouseButton button) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  static_cast<void>(frame(panel));
  io.AddMouseButtonEvent(button, true);
  static_cast<void>(frame(panel));
  io.AddMouseButtonEvent(button, false);
  static_cast<void>(frame(panel));
}

/// Right-clicks `at` and returns the frame after, once a menu has sized.
std::string right_click(PanelFn panel, ImVec2 at) noexcept {
  click(panel, at, ImGuiMouseButton_Right);
  return frame(panel);
}

void settle(PanelFn panel) noexcept {
  ImGui::GetIO().AddMousePosEvent(-100.0F, -100.0F);
  ImGui::ClosePopupsExceptModals();
  static_cast<void>(frame(panel));
  static_cast<void>(frame(panel));
}

bool menu_open() noexcept { return !GImGui->OpenPopupStack.empty(); }

ImRect top_menu() noexcept {
  const ImGuiContext &g = *GImGui;
  if (g.OpenPopupStack.empty() || (g.OpenPopupStack.back().Window == nullptr)) {
    return ImRect();
  }
  return g.OpenPopupStack.back().Window->Rect();
}

/// The centre of the `row`th item from the top of `menu`, counting a
/// separator as `separators` extra spacing.
ImVec2 menu_row(const ImRect &menu, int row, int separators = 0) noexcept {
  const ImGuiStyle &style = ImGui::GetStyle();
  const float step = ImGui::GetFontSize() + style.ItemSpacing.y;
  return ImVec2(menu.Min.x + (menu.GetWidth() * 0.3F),
                menu.Min.y + style.WindowPadding.y +
                    (ImGui::GetFontSize() * 0.5F) +
                    (step * static_cast<float>(row)) +
                    (style.ItemSpacing.y * static_cast<float>(separators)));
}

ImVec2 bottom_of(const char *windowName) noexcept {
  const ImGuiWindow *window = ImGui::FindWindowByName(windowName);
  if (window == nullptr) {
    return ImVec2(-1.0F, -1.0F);
  }
  return ImVec2(window->Pos.x + (window->Size.x * 0.5F),
                window->Pos.y + window->Size.y - 60.0F);
}

/// True when the item under `at` is enabled: ImGui records a hover over a
/// disabled item as disabled.
bool enabled_at(PanelFn panel, ImVec2 at) noexcept {
  ImGui::GetIO().AddMousePosEvent(at.x, at.y);
  static_cast<void>(frame(panel));
  static_cast<void>(frame(panel));
  return (GImGui->HoveredId != 0U) && !GImGui->HoveredIdIsDisabled;
}

bool has(const std::string &text, const char *item) noexcept {
  return text.find(item) != std::string::npos;
}

void check_log() noexcept {
  engine::core::log_message(engine::core::LogLevel::Info, "test",
                            "a line for the log");
  const PanelFn log = [] { draw_console_panel(); };
  settle(log);
  check(console_capture_entry_count() > 0U, "the log holds a line");
  const std::string toolbar = frame(log);
  check(has(toolbar, "Clear") && !has(toolbar, "Copy All"),
        "the Log's toolbar keeps Clear and leaves Copy All to its menus");
  // The log's scroll region fills the panel above its command line.
  const ImGuiWindow *logWindow = ImGui::FindWindowByName(kLogWindow);
  if (logWindow == nullptr) {
    g_tests.fail("the Log draws");
    return;
  }
  const ImVec2 inside(logWindow->Pos.x + (logWindow->Size.x * 0.5F),
                      logWindow->Pos.y + logWindow->Size.y - 70.0F);
  const std::string text = right_click(log, inside);
  check(menu_open() && has(text, "Clear") && has(text, "Copy All") &&
            has(text, "Collapse") && has(text, "Autoscroll") &&
            has(text, "Pause"),
        "the log's empty space offers Clear, Copy All and its toggles");
  check(!has(text, "Add Component") && !has(text, "Create Empty") &&
            !has(text, "Screenshot"),
        "and nothing of another window's");

  const bool collapseBefore = editor_session().console.collapseView;
  // Collapse is the third item, under a separator.
  const ImRect menuRect = top_menu();
  click(log, menu_row(menuRect, 2, 1), ImGuiMouseButton_Left);
  check(editor_session().console.collapseView != collapseBefore,
        "Collapse in the menu toggles the Log's collapse");

  // A line's own menu ends with the same items, so they are reachable
  // however full the log is.
  settle(log);
  const ImGuiWindow *scroll = logWindow->DC.ChildWindows.empty()
                                  ? nullptr
                                  : logWindow->DC.ChildWindows[0];
  check(scroll != nullptr, "the log scrolls in a region of its own");
  if (scroll != nullptr) {
    const ImVec2 firstLine(scroll->Pos.x + (scroll->Size.x * 0.3F),
                           scroll->DC.CursorStartPos.y +
                               (ImGui::GetFontSize() * 0.5F));
    const std::string lineMenu = right_click(log, firstLine);
    check(menu_open() && has(lineMenu, "Copy Message") &&
              has(lineMenu, "Clear") && has(lineMenu, "Collapse"),
          "a line's menu offers Copy Message and the log's own items");
  }

  settle(log);
  static_cast<void>(right_click(log, inside));
  click(log, menu_row(top_menu(), 0), ImGuiMouseButton_Left);
  check(console_capture_entry_count() == 0U, "Clear empties the log");
  editor_session().console.collapseView = collapseBefore;
  settle(log);
}

void check_inspector(World &world) noexcept {
  const Entity entity = world.create_scene_object();
  const Entity donor = world.create_scene_object();
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.25F, 0.5F, 0.75F);
  check((entity != engine::runtime::kInvalidEntity) &&
            world.add_collider(donor, collider),
        "an entity and a donor with a collider");
  select_entity(entity, false);
  component_clipboard_clear();
  const PanelFn inspector = [] { draw_inspector_panel(); };
  settle(inspector);
  check(!has(frame(inspector), "Delete Entity"),
        "the Inspector has no Delete button; deleting is the scene's edit");

  const ImVec2 empty = bottom_of(kInspectorWindow);
  std::string text = right_click(inspector, empty);
  check(menu_open() && has(text, "Add Component") &&
            has(text, "Paste Component As New"),
        "the Inspector's empty space offers Add Component and Paste As New");
  check(!has(text, "Clear") && !has(text, "Create Empty") &&
            !has(text, "Screenshot"),
        "and nothing of another window's");
  // Paste Component As New, the second item, is disabled with nothing
  // copied: clicking it adds nothing.
  check(!enabled_at(inspector, menu_row(top_menu(), 1)),
        "with nothing copied Paste Component As New is disabled");
  click(inspector, menu_row(top_menu(), 1), ImGuiMouseButton_Left);
  engine::runtime::Collider read{};
  check(!world.get_collider(entity, &read),
        "with nothing copied Paste Component As New does nothing");

  settle(inspector);
  check(component_clipboard_copy(donor, ComponentEditType::Collider),
        "copy the donor's collider");
  static_cast<void>(right_click(inspector, empty));
  check(enabled_at(inspector, menu_row(top_menu(), 1)),
        "with a collider copied it is enabled");
  click(inspector, menu_row(top_menu(), 1), ImGuiMouseButton_Left);
  check(world.get_collider(entity, &read) &&
            (read.halfExtents.z == collider.halfExtents.z),
        "Paste Component As New adds the copied collider");
  auto &history = editor_session().commandHistory;
  check(history.undo() && !world.get_collider(entity, &read),
        "one undo removes it");

  // An entity that already has the copied component cannot take another:
  // every entity has a Transform.
  settle(inspector);
  check(component_clipboard_copy(donor, ComponentEditType::Transform),
        "copy the donor's transform");
  static_cast<void>(right_click(inspector, empty));
  check(menu_open() && !enabled_at(inspector, menu_row(top_menu(), 1)),
        "on an entity that has the copied component it is disabled");

  settle(inspector);
  static_cast<void>(right_click(inspector, empty));
  const ImVec2 addRow = menu_row(top_menu(), 0);
  ImGui::GetIO().AddMousePosEvent(addRow.x, addRow.y);
  for (int i = 0; i < 4; ++i) {
    static_cast<void>(frame(inspector));
  }
  text = frame(inspector);
  check((GImGui->OpenPopupStack.Size == 2) && has(text, "Physics"),
        "Add Component lists the categories of what the entity lacks");
  settle(inspector);
  component_clipboard_clear();
  history.clear();
}

void check_game_view() noexcept {
  const PanelFn game = [] { draw_game_view_panel(); };
  settle(game);
  const ImGuiWindow *window = ImGui::FindWindowByName(kGameViewWindow);
  check(window != nullptr, "the Game view draws");
  if (window == nullptr) {
    return;
  }
  const float imageTop = editor_session().gameViewScreenPos.y;
  const ImVec2 onImage(window->Pos.x + (window->Size.x * 0.5F),
                       imageTop + 100.0F);
  static_cast<void>(right_click(game, onImage));
  check(!menu_open(), "a right-click on the game's image opens nothing");

  settle(game);
  const ImVec2 onToolbar(window->Pos.x + window->Size.x - 40.0F,
                         (window->Pos.y + imageTop) * 0.5F);
  const std::string text = right_click(game, onToolbar);
  check(menu_open() && has(text, "Take Screenshot") &&
            has(text, "Record Play") && has(text, "Stats"),
        "the toolbar row offers Take Screenshot, recording and Stats");
  check(!has(text, "Add Component") && !has(text, "Clear") &&
            !has(text, "Create Empty"),
        "and nothing of another window's");
  settle(game);
}

} // namespace

int main() {
  static_cast<void>(engine::core::initialize_logging());
  static_cast<void>(engine::core::initialize_cvars());
  static_cast<void>(engine::core::initialize_console());
  register_stats_cvars();
  console_capture_initialize();
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  editor_set_world(world.get());

  check_log();
  check_inspector(*world);
  check_game_view();

  editor_set_world(nullptr);
  ImGui::DestroyContext();
  console_capture_shutdown();
  engine::core::shutdown_console();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  return g_tests.finish("editor_panel_menus");
}
