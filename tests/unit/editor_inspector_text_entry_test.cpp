// Pins that a typed Inspector field reaches the World once, when the
// edit ends. Typing "0.3" into a collider's half extent passes through
// "0" and "0." on the way; each keystroke used to be applied at once, so
// the World refused the zero extent with an Error every frame and every
// accepted intermediate value was a live mutation. Here the generic
// field drawer runs under a headless ImGui context fed keyboard events
// frame by frame, wired to the production staging and commit calls the
// Inspector panel makes: the value must not change while typing, must
// change once on Enter (and once on a click elsewhere), must land as one
// undo step, and Escape must discard the draft. No Error may be logged.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "editor_commands.h"
#include "editor_panels_inspector_generic.h"
#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/world.h"

namespace {

using engine::editor::ComponentEditSnapshot;
using engine::editor::ComponentEditType;
using engine::runtime::Entity;
using engine::runtime::World;

engine::tests::TestContext g_tests;
int g_rejections = 0;

void count_rejections(engine::core::LogLevel level, const char *,
                      const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Error) && (message != nullptr) &&
      (std::strstr(message, "add_collider rejected") != nullptr)) {
    ++g_rejections;
  }
}

/// One ImGui frame of the Half Extents field drawn the way the Inspector
/// draws it: a fresh snapshot of the component, the field, and the
/// staging and commit calls on a change. Returns what the drawer
/// reported.
bool frame(World &world, Entity entity, bool focus) noexcept {
  ImGui::NewFrame();
  ImGui::Begin("Probe");
  engine::runtime::Collider snapshot{};
  static_cast<void>(world.get_collider(entity, &snapshot));
  const engine::runtime::Collider before = snapshot;
  if (focus) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool modified = engine::editor::draw_reflected_field(
      "engine::runtime::Collider", "halfExtents", &snapshot, nullptr, false);
  if (modified) {
    ComponentEditSnapshot beforeSnapshot{};
    beforeSnapshot.collider = before;
    ComponentEditSnapshot afterSnapshot{};
    afterSnapshot.collider = snapshot;
    static_cast<void>(engine::editor::inspector_stage_component_edit(
        entity, ComponentEditType::Collider, beforeSnapshot, afterSnapshot));
  }
  if (engine::editor::inspector_has_pending_edit() &&
      !ImGui::IsAnyItemActive()) {
    engine::editor::inspector_commit_pending_edit();
  }
  ImGui::End();
  ImGui::Render();
  return modified;
}

bool type_text(World &world, Entity entity, const char *text) noexcept {
  bool anyModified = false;
  for (const char *c = text; *c != '\0'; ++c) {
    ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(*c));
    anyModified = frame(world, entity, false) || anyModified;
  }
  return anyModified;
}

bool press(World &world, Entity entity, ImGuiKey key) noexcept {
  ImGui::GetIO().AddKeyEvent(key, true);
  const bool modified = frame(world, entity, false);
  ImGui::GetIO().AddKeyEvent(key, false);
  return modified;
}

float extent_x(World &world, Entity entity) noexcept {
  engine::runtime::Collider collider{};
  static_cast<void>(world.get_collider(entity, &collider));
  return collider.halfExtents.x;
}

bool same(float a, float b) noexcept { return std::fabs(a - b) < 1.0e-6F; }

void run(World &world, Entity entity) noexcept {
  engine::editor::CommandHistory &history =
      engine::editor::editor_session().commandHistory;

  // Focus the X field; typing replaces the selected text.
  static_cast<void>(frame(world, entity, true));
  const bool typedModified = type_text(world, entity, "0.3");
  g_tests.check(!typedModified, "typing reports no change before commit");
  g_tests.check(same(extent_x(world, entity), 0.5F),
                "the World keeps the old extent while text is typed");
  g_tests.check(g_rejections == 0, "no half-typed value reached the World");

  const bool enterModified = press(world, entity, ImGuiKey_Enter);
  g_tests.check(enterModified, "Enter commits the typed value");
  g_tests.check(same(extent_x(world, entity), 0.3F),
                "the World holds 0.3 after Enter");
  static_cast<void>(frame(world, entity, false));
  g_tests.check(history.can_undo() && !history.can_redo(),
                "the commit is undoable");
  const auto tokenAfterFirst = history.current_token();

  // A second edit ended by a click elsewhere commits the same way.
  static_cast<void>(frame(world, entity, true));
  static_cast<void>(type_text(world, entity, "0.4"));
  g_tests.check(same(extent_x(world, entity), 0.3F),
                "the World keeps 0.3 while 0.4 is typed");
  ImGui::GetIO().AddMousePosEvent(1200.0F, 700.0F);
  ImGui::GetIO().AddMouseButtonEvent(0, true);
  const bool clickModified = frame(world, entity, false);
  ImGui::GetIO().AddMouseButtonEvent(0, false);
  static_cast<void>(frame(world, entity, false));
  g_tests.check(clickModified, "a click elsewhere commits the typed value");
  g_tests.check(same(extent_x(world, entity), 0.4F),
                "the World holds 0.4 after the click");
  g_tests.check(history.current_token() != tokenAfterFirst,
                "the second commit is its own history step");
  const auto tokenAfterSecond = history.current_token();

  // Escape discards the draft.
  static_cast<void>(frame(world, entity, true));
  static_cast<void>(type_text(world, entity, "9"));
  const bool escapeModified = press(world, entity, ImGuiKey_Escape);
  static_cast<void>(frame(world, entity, false));
  g_tests.check(!escapeModified, "Escape reports no change");
  g_tests.check(same(extent_x(world, entity), 0.4F),
                "the World keeps 0.4 after Escape");
  g_tests.check(history.current_token() == tokenAfterSecond,
                "Escape records no history step");

  // Undo steps back through exactly the two commits.
  engine::editor::editor_history_undo();
  g_tests.check(same(extent_x(world, entity), 0.3F), "undo restores 0.3");
  engine::editor::editor_history_undo();
  g_tests.check(same(extent_x(world, entity), 0.5F), "undo restores 0.5");
  g_tests.check(!history.can_undo(), "two commits were two undo steps");
  g_tests.check(g_rejections == 0, "no Error was logged by the whole edit");
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::runtime::ensure_runtime_reflection_registered();
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_rejections, nullptr)) {
    std::fprintf(stderr, "FAIL: logging\n");
    return 1;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  const Entity entity = world->create_scene_object();
  engine::runtime::Collider collider{};
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world->add_collider(entity, collider)) {
    return 3;
  }
  engine::editor::editor_session().world = world.get();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  // No renderer backend: build the atlas the legacy way so NewFrame has
  // a font.
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  run(*world, entity);

  engine::editor::inspector_abandon_pending_edit();
  engine::editor::editor_session().commandHistory.clear();
  engine::editor::editor_session().world = nullptr;
  ImGui::DestroyContext();
  engine::core::log_unregister_sink(&count_rejections, nullptr);
  engine::core::shutdown_logging();
  return g_tests.finish("inspector text entry tests");
}
