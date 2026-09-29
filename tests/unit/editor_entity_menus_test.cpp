// Verifies the right-click menus of the windows that edit entities, and
// the actions they add, through the production Entities panel on headless
// ImGui frames. A right-click on a row selects it and opens that entity's
// menu (its edits, Rename, Frame Selected, creating a child), a
// right-click on the panel's empty space opens the creation menu, and
// neither lists the other's items. Create Empty Child creates under the
// right-clicked row and Create Empty a root, each the new selection and
// one undo step. A creation's placement parents it at its parent's origin
// or puts it at a given point, lifted by a primitive's rest height, and a
// dead parent creates nothing. Rename edits the row in place: Enter
// commits one undoable edit, Escape leaves the name, an empty or unchanged
// name changes nothing, an unnamed entity gains a name, and switching
// worlds ends a rename. Camera, on the empty-space menu, creates an active
// "Main Camera" (then "Camera") up and back from its target, facing it, in
// one undo step.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "../test_harness.h"
#include "editor_commands.h"
#include "editor_entity_menus.h"
#include "editor_entity_rename.h"
#include "editor_panels_main.h"
#include "editor_session.h"
#include "editor_shortcuts.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/math/quat.h"
#include "engine/runtime/world.h"

namespace {

using namespace engine::editor;
using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::NameComponent;
using engine::runtime::Transform;
using engine::runtime::World;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

/// Draws one frame of the Entities panel and returns the text it and its
/// menus rendered.
std::string entities_frame() noexcept {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(360.0F, 520.0F));
  draw_entities_panel();
  const std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

/// Presses and releases `button` at `at`, a frame per step, then draws
/// the frame a new popup takes to size itself; returns that frame's text.
std::string click(ImVec2 at, ImGuiMouseButton button) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  static_cast<void>(entities_frame());
  io.AddMouseButtonEvent(button, true);
  static_cast<void>(entities_frame());
  io.AddMouseButtonEvent(button, false);
  static_cast<void>(entities_frame());
  return entities_frame();
}

/// The first row's centre, from where the panel last began its content.
ImVec2 first_row() noexcept {
  const ImGuiWindow *const window = ImGui::FindWindowByName(kEntitiesWindow);
  if (window == nullptr) {
    return ImVec2(-1.0F, -1.0F);
  }
  return ImVec2(window->DC.CursorStartPos.x + 60.0F,
                window->DC.CursorStartPos.y + (ImGui::GetFontSize() * 0.5F));
}

/// A point in the panel's empty space, below its rows and buttons.
ImVec2 empty_space() noexcept {
  const ImGuiWindow *const window = ImGui::FindWindowByName(kEntitiesWindow);
  if (window == nullptr) {
    return ImVec2(-1.0F, -1.0F);
  }
  return ImVec2(window->Pos.x + (window->Size.x * 0.5F),
                window->Pos.y + window->Size.y - 30.0F);
}

/// The open menu's rectangle; empty when no menu is open.
ImRect open_menu() noexcept {
  const ImGuiContext &g = *GImGui;
  if (g.OpenPopupStack.empty() || (g.OpenPopupStack.back().Window == nullptr)) {
    return ImRect();
  }
  return g.OpenPopupStack.back().Window->Rect();
}

/// The centre of the open menu's `row`th item, counted from its top
/// (`fromTop`) or its bottom, for a menu with no separator between.
ImVec2 menu_row(const ImRect &menu, int row, bool fromTop) noexcept {
  const ImGuiStyle &style = ImGui::GetStyle();
  const float step = ImGui::GetFontSize() + style.ItemSpacing.y;
  const float half = ImGui::GetFontSize() * 0.5F;
  const float y = fromTop ? (menu.Min.y + style.WindowPadding.y + half +
                             (step * static_cast<float>(row)))
                          : (menu.Max.y - style.WindowPadding.y - half -
                             (step * static_cast<float>(row)));
  return ImVec2(menu.Min.x + (menu.GetWidth() * 0.3F), y);
}

Entity add_named(World &world, const char *name) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }
  NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return world.add_name_component(entity, component) ? entity : kInvalidEntity;
}

std::string name_of(const World &world, Entity entity) noexcept {
  NameComponent name{};
  return world.get_name_component(entity, &name) ? std::string(name.name)
                                                 : std::string("<none>");
}

bool parented_at_origin(const World &world, Entity child,
                        Entity parent) noexcept {
  Transform transform{};
  return world.get_transform(child, &transform) &&
         (transform.parentId == world.persistent_id(parent)) &&
         (transform.position.x == 0.0F) && (transform.position.y == 0.0F) &&
         (transform.position.z == 0.0F);
}

/// Parks the mouse off every window and closes any menu.
void settle() noexcept {
  ImGui::GetIO().AddMousePosEvent(-100.0F, -100.0F);
  ImGui::ClosePopupsExceptModals();
  static_cast<void>(entities_frame());
  static_cast<void>(entities_frame());
}

/// A right-click on a row opens that entity's menu and selects the row;
/// one on empty space opens the creation menu; each lists only its own.
void check_menus(World &world, Entity alpha, Entity beta) noexcept {
  select_entity(beta, false);
  settle();
  const std::string rowMenu = click(first_row(), ImGuiMouseButton_Right);
  check(selected_entity() == alpha, "a right-click selects the row it is on");
  for (const char *item :
       {"Copy", "Paste As Child", "Duplicate", "Delete", "Rename",
        "Frame Selected", "Create Empty Child", "3D Object Child"}) {
    char what[96] = {};
    std::snprintf(what, sizeof(what), "the row's menu lists %s", item);
    check(rowMenu.find(item) != std::string::npos, what);
  }
  check(rowMenu.find("F2") != std::string::npos,
        "the row's menu shows Rename's chord from the action table");

  // Create Empty Child is the item above the last, 3D Object Child.
  const std::size_t before = world.alive_entity_count();
  static_cast<void>(
      click(menu_row(open_menu(), 1, false), ImGuiMouseButton_Left));
  const Entity child = selected_entity();
  check((world.alive_entity_count() == before + 1U) && (child != alpha) &&
            parented_at_origin(world, child, alpha),
        "Create Empty Child creates under the right-clicked row, selected");
  auto &history = editor_session().commandHistory;
  check(history.undo() && (world.alive_entity_count() == before),
        "one undo removes the child");

  // 3D Object Child, the last item, opens the primitives; Cube, the
  // first, spawns under the right-clicked row even with another selected.
  select_entity(beta, false);
  settle();
  static_cast<void>(click(first_row(), ImGuiMouseButton_Right));
  const ImVec2 objectChild = menu_row(open_menu(), 0, false);
  ImGui::GetIO().AddMousePosEvent(objectChild.x, objectChild.y);
  for (int i = 0; i < 4; ++i) {
    static_cast<void>(entities_frame());
  }
  const ImGuiContext &g = *GImGui;
  check(g.OpenPopupStack.Size == 2, "3D Object Child opens the primitives");
  const ImVec2 cubeRow = menu_row(open_menu(), 0, true);
  // Across to the submenu first, along the row, so the pointer never
  // crosses another item of the parent menu.
  ImGui::GetIO().AddMousePosEvent(cubeRow.x, objectChild.y);
  static_cast<void>(entities_frame());
  static_cast<void>(click(cubeRow, ImGuiMouseButton_Left));
  const Entity cube = selected_entity();
  NameComponent cubeName{};
  check((world.alive_entity_count() == before + 1U) &&
            world.get_name_component(cube, &cubeName) &&
            (std::strcmp(cubeName.name, "Cube") == 0) &&
            parented_at_origin(world, cube, alpha),
        "3D Object Child > Cube spawns under the right-clicked row, selected");
  check(history.undo() && (world.alive_entity_count() == before),
        "one undo removes the cube");

  settle();
  const std::string spaceMenu = click(empty_space(), ImGuiMouseButton_Right);
  check((spaceMenu.find("Create Empty") != std::string::npos) &&
            (spaceMenu.find("3D Object") != std::string::npos) &&
            (spaceMenu.find("Camera") != std::string::npos) &&
            (spaceMenu.find("Paste") != std::string::npos),
        "the empty-space menu lists Create Empty, 3D Object, Camera and "
        "Paste");
  for (const char *item :
       {"Copy", "Duplicate", "Delete", "Rename", "Frame Selected", "Child"}) {
    char what[96] = {};
    std::snprintf(what, sizeof(what), "the empty-space menu leaves out %s",
                  item);
    check(spaceMenu.find(item) == std::string::npos, what);
  }

  // Create Empty is its first item, and makes a root.
  static_cast<void>(
      click(menu_row(open_menu(), 0, true), ImGuiMouseButton_Left));
  const Entity root = selected_entity();
  Transform transform{};
  check((world.alive_entity_count() == before + 1U) && (root != alpha) &&
            (root != beta) && world.get_transform(root, &transform) &&
            (transform.parentId == engine::runtime::kInvalidPersistentId),
        "Create Empty from empty space creates a root, selected");
  check(history.undo() && (world.alive_entity_count() == before),
        "one undo removes it");
  settle();
}

/// The centre of the panel's "+ Create" button, the last thing it draws:
/// its line ends where the window's cursor stood after it.
ImVec2 create_button() noexcept {
  const ImGuiWindow *const window = ImGui::FindWindowByName(kEntitiesWindow);
  if (window == nullptr) {
    return ImVec2(-1.0F, -1.0F);
  }
  const ImGuiStyle &style = ImGui::GetStyle();
  return ImVec2(window->DC.CursorPosPrevLine.x - style.FramePadding.x -
                    (ImGui::CalcTextSize("+ Create").x * 0.5F),
                window->DC.CursorPosPrevLine.y +
                    (ImGui::GetFrameHeight() * 0.5F));
}

/// The panel's one creation button opens the menu its empty space does,
/// so creation is found without knowing to right-click, and its Create
/// Empty makes a selected root in one undo step.
void check_create_button(World &world, Entity alpha, Entity beta) noexcept {
  settle();
  const std::string panel = entities_frame();
  check(panel.find("+ Create") != std::string::npos,
        "the panel shows its + Create button");
  check((panel.find("Create Entity") == std::string::npos) &&
            (panel.find("Add Primitive") == std::string::npos),
        "the two buttons the menus replace are gone");
  const std::string menu = click(create_button(), ImGuiMouseButton_Left);
  check((menu.find("Create Empty") != std::string::npos) &&
            (menu.find("3D Object") != std::string::npos) &&
            (menu.find("Paste") != std::string::npos) &&
            (menu.find("Duplicate") == std::string::npos),
        "+ Create opens the empty-space menu, and only it");
  const std::size_t before = world.alive_entity_count();
  static_cast<void>(
      click(menu_row(open_menu(), 0, true), ImGuiMouseButton_Left));
  const Entity root = selected_entity();
  Transform transform{};
  check((world.alive_entity_count() == before + 1U) && (root != alpha) &&
            (root != beta) && world.get_transform(root, &transform) &&
            (transform.parentId == engine::runtime::kInvalidPersistentId),
        "+ Create > Create Empty creates a root, selected");
  check(editor_session().commandHistory.undo() &&
            (world.alive_entity_count() == before),
        "one undo removes it");
  settle();
}

/// A placement parents a creation at its parent's origin, or puts it at a
/// point (a primitive lifted by its rest height); a dead parent creates
/// nothing and records no undo step.
void check_placement(World &world, Entity alpha) noexcept {
  auto &history = editor_session().commandHistory;
  history.clear();
  const std::size_t before = world.alive_entity_count();

  EntitySpawnPlacement under{};
  under.parent = alpha;
  const Entity child = execute_entity_create(under);
  check(parented_at_origin(world, child, alpha),
        "an empty entity is created at its parent's origin");
  const Entity cube = execute_primitive_spawn(EditorPrimitive::Cube, under);
  check(parented_at_origin(world, cube, alpha),
        "a primitive child sits at its parent's origin");

  EntitySpawnPlacement at{};
  at.hasPosition = true;
  at.position = engine::math::Vec3(3.0F, 0.0F, -2.0F);
  Transform transform{};
  const Entity placed = execute_entity_create(at);
  check(world.get_transform(placed, &transform) &&
            (transform.position.x == 3.0F) && (transform.position.y == 0.0F) &&
            (transform.position.z == -2.0F) &&
            (transform.parentId == engine::runtime::kInvalidPersistentId),
        "an empty entity is created at the given point");
  const Entity capsule = execute_primitive_spawn(EditorPrimitive::Capsule, at);
  check(world.get_transform(capsule, &transform) &&
            (transform.position.x == 3.0F) && (transform.position.y == 1.0F) &&
            (transform.position.z == -2.0F),
        "a primitive at a point rests on it, lifted by its height");
  check(world.alive_entity_count() == before + 4U, "four creations");
  for (int i = 0; i < 4; ++i) {
    static_cast<void>(history.undo());
  }
  check(world.alive_entity_count() == before && !history.can_undo(),
        "each creation is one undo step");

  const Entity doomed = add_named(world, "Doomed");
  static_cast<void>(world.destroy_entity(doomed));
  EntitySpawnPlacement dead{};
  dead.parent = doomed;
  check((execute_entity_create(dead) == kInvalidEntity) &&
            (execute_primitive_spawn(EditorPrimitive::Cube, dead) ==
             kInvalidEntity) &&
            (world.alive_entity_count() == before) && !history.can_undo(),
        "a dead parent creates nothing and records no undo step");
}

/// True when `camera` is an active Camera at `position`, looking along
/// `forward`. The pose is built from exact inputs; 1e-5 bounds only the
/// float rounding of the rotation's basis and its re-application.
bool camera_posed(const World &world, Entity camera,
                  const engine::math::Vec3 &position,
                  const engine::math::Vec3 &forward) noexcept {
  engine::runtime::CameraComponent component{};
  Transform transform{};
  if (!world.get_camera_component(camera, &component) || !component.active ||
      !world.get_transform(camera, &transform)) {
    return false;
  }
  const engine::math::Vec3 look = engine::math::rotate_vector(
      engine::math::Vec3(0.0F, 0.0F, -1.0F), transform.rotation);
  const engine::math::Vec3 want = engine::math::normalize(forward);
  constexpr float kBound = 1.0e-5F;
  return (transform.position.x == position.x) &&
         (transform.position.y == position.y) &&
         (transform.position.z == position.z) &&
         (std::fabs(look.x - want.x) <= kBound) &&
         (std::fabs(look.y - want.y) <= kBound) &&
         (std::fabs(look.z - want.z) <= kBound);
}

/// Camera on the empty-space menu creates an active "Main Camera" two up
/// and five back from the origin, facing it, selected, in one undo step;
/// a second is "Camera"; a placement moves its target; a dead parent
/// creates nothing.
void check_camera_create(World &world) noexcept {
  auto &history = editor_session().commandHistory;
  history.clear();
  settle();
  const std::size_t before = world.alive_entity_count();
  static_cast<void>(click(empty_space(), ImGuiMouseButton_Right));
  // Create Empty, 3D Object, Camera: the third row, with no separator
  // above it.
  static_cast<void>(
      click(menu_row(open_menu(), 2, true), ImGuiMouseButton_Left));
  const Entity main = selected_entity();
  check((world.alive_entity_count() == before + 1U) &&
            (name_of(world, main) == "Main Camera") &&
            camera_posed(world, main, engine::math::Vec3(0.0F, 2.0F, 5.0F),
                         engine::math::Vec3(0.0F, -2.0F, -5.0F)),
        "Camera creates an active Main Camera facing the origin, selected");

  EntitySpawnPlacement at{};
  at.hasPosition = true;
  at.position = engine::math::Vec3(3.0F, 0.0F, -2.0F);
  const Entity second = execute_camera_create(at);
  check((name_of(world, second) == "Camera") &&
            camera_posed(world, second, engine::math::Vec3(3.0F, 2.0F, 3.0F),
                         engine::math::Vec3(0.0F, -2.0F, -5.0F)),
        "a second camera is \"Camera\", framing the placement's point");
  check(history.undo() && history.undo() &&
            (world.alive_entity_count() == before) && !history.can_undo(),
        "each camera is one undo step");

  const Entity doomed = add_named(world, "Doomed");
  static_cast<void>(world.destroy_entity(doomed));
  EntitySpawnPlacement dead{};
  dead.parent = doomed;
  check((execute_camera_create(dead) == kInvalidEntity) &&
            (world.alive_entity_count() == before) && !history.can_undo(),
        "a dead parent creates no camera and records no undo step");
  settle();
}

/// Types `text` into the focused field and presses `key`, a frame each.
void type_and_press(const char *text, ImGuiKey key) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddInputCharactersUTF8(text);
  static_cast<void>(entities_frame());
  io.AddKeyEvent(key, true);
  static_cast<void>(entities_frame());
  io.AddKeyEvent(key, false);
  static_cast<void>(entities_frame());
}

/// F2's Rename edits the row in place: Enter commits one undoable edit,
/// Escape leaves the name.
void check_rename_in_panel(World &world, Entity alpha) noexcept {
  auto &history = editor_session().commandHistory;
  history.clear();
  settle();
  select_entity(alpha, false);
  check(run_editor_action(EditorAction::Rename) &&
            entity_rename_active_for(alpha),
        "Rename starts on the selection");
  static_cast<void>(entities_frame());
  const std::string field = entities_frame();
  check(field.find("Alpha") != std::string::npos,
        "the field holds the current name");
  type_and_press("Gamma", ImGuiKey_Enter);
  check(name_of(world, alpha) == "Gamma" && !entity_rename_active_for(alpha),
        "Enter gives the typed name and ends the rename");
  check(history.undo() && (name_of(world, alpha) == "Alpha") &&
            !history.can_undo(),
        "the rename is one undo step");

  check(run_editor_action(EditorAction::Rename), "Rename again");
  static_cast<void>(entities_frame());
  static_cast<void>(entities_frame());
  type_and_press("Nope", ImGuiKey_Escape);
  check(name_of(world, alpha) == "Alpha" && !entity_rename_active_for(alpha) &&
            !history.can_undo(),
        "Escape leaves the name and records nothing");
  settle();
}

/// The rename's own contract, without the panel.
void check_rename_contract(World &world, Entity alpha) noexcept {
  auto &history = editor_session().commandHistory;
  history.clear();
  check(!begin_entity_rename(kInvalidEntity), "no entity, no rename");

  check(begin_entity_rename(alpha), "begin");
  entity_rename_state().buffer[0] = '\0';
  check(!commit_entity_rename() && (name_of(world, alpha) == "Alpha") &&
            !history.can_undo(),
        "an empty name changes nothing");
  check(begin_entity_rename(alpha) && !commit_entity_rename() &&
            !history.can_undo(),
        "an unchanged name changes nothing");

  const Entity unnamed = world.create_scene_object();
  check(begin_entity_rename(unnamed) &&
            (entity_rename_state().buffer[0] == '\0'),
        "an unnamed entity starts with an empty field");
  std::snprintf(entity_rename_state().buffer,
                sizeof(entity_rename_state().buffer), "%s", "Fresh");
  check(commit_entity_rename() && (name_of(world, unnamed) == "Fresh"),
        "an unnamed entity gains the typed name");
  check(history.undo() && (name_of(world, unnamed) == "<none>"),
        "undo takes the added name away");

  check(begin_entity_rename(alpha), "begin before a world switch");
  std::unique_ptr<World> other(new (std::nothrow) World());
  if (other == nullptr) {
    g_tests.fail("allocate the other world");
    return;
  }
  editor_set_world(other.get());
  editor_set_world(&world);
  check(!entity_rename_active_for(alpha), "switching worlds ends the rename");

  check(begin_entity_rename(alpha), "begin before the entity dies");
  static_cast<void>(world.destroy_entity(unnamed));
  const Entity doomed = add_named(world, "Doomed");
  check(begin_entity_rename(doomed), "begin on a doomed entity");
  static_cast<void>(world.destroy_entity(doomed));
  std::snprintf(entity_rename_state().buffer,
                sizeof(entity_rename_state().buffer), "%s", "Ghost");
  check(!commit_entity_rename() && !entity_rename_active_for(doomed),
        "a rename of an entity that died changes nothing");
}

/// Create Empty Child and Rename need a selection to act on.
void check_enabled_states(Entity alpha) noexcept {
  clear_entity_selection();
  check(!editor_action_enabled(EditorAction::CreateEmptyChild) &&
            !editor_action_enabled(EditorAction::Rename),
        "without a selection neither is enabled");
  select_entity(alpha, false);
  check(editor_action_enabled(EditorAction::CreateEmptyChild) &&
            editor_action_enabled(EditorAction::Rename),
        "with one both are");
}

} // namespace

int main() {
  static_cast<void>(engine::core::initialize_logging());
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
  const Entity alpha = add_named(*world, "Alpha");
  const Entity beta = add_named(*world, "Beta");
  check((alpha != kInvalidEntity) && (beta != kInvalidEntity),
        "two named roots");

  check_menus(*world, alpha, beta);
  check_create_button(*world, alpha, beta);
  check_placement(*world, alpha);
  check_camera_create(*world);
  check_rename_in_panel(*world, alpha);
  check_rename_contract(*world, alpha);
  check_enabled_states(alpha);

  editor_session().commandHistory.clear();
  editor_set_world(nullptr);
  ImGui::DestroyContext();
  engine::core::shutdown_logging();
  return g_tests.finish("editor_entity_menus");
}
