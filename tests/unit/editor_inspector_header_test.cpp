// Verifies the Inspector's component headers on headless ImGui frames. An
// entity shows a section only for the components it has: no "<none>" row
// for each component it lacks, and the raw entity index stays behind
// Advanced. Each header's options button is a square of the header's own
// height, flush with its right edge at every font scale (a fixed pixel
// offset used to push it off the header as the UI scaled), and opens the
// component menu whose last item, Remove Component, is reported to the
// caller.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "../test_harness.h"
#include "editor_component_ops.h"
#include "editor_panels_inspector.h"
#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/runtime/world.h"

namespace {

using engine::editor::ComponentEditType;
using engine::runtime::Entity;
using engine::runtime::World;

engine::tests::TestContext g_tests;

/// Draws one Inspector frame and returns the text it rendered.
std::string inspector_text() noexcept {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(420.0F, 700.0F));
  engine::editor::draw_inspector_panel();
  const std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

void check_only_present_components() noexcept {
  const std::string text = inspector_text();
  g_tests.check(text.find("Rigid Body") != std::string::npos,
                "the entity's Rigid Body has a section");
  g_tests.check(text.find("<none>") == std::string::npos,
                "no section is drawn for a component the entity lacks");
  g_tests.check(text.find("Collider") == std::string::npos,
                "an absent Collider is not listed");
  g_tests.check(text.find("Entity [") == std::string::npos,
                "the raw entity index is behind Advanced");
}

/// What one probe frame of a header and its options saw.
struct HeaderFrame final {
  ImVec2 headerMin{};
  ImVec2 headerMax{};
  ImVec2 buttonMin{};
  ImVec2 buttonMax{};
  bool removeChosen = false;
  bool menuOpen = false;
};

/// Draws a Rigid Body header the way the Inspector does, with its options.
HeaderFrame header_frame(Entity entity) noexcept {
  HeaderFrame frame{};
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(420.0F, 400.0F));
  ImGui::Begin("Probe");
  ImGui::PushID("Rigid Body");
  ImGui::CollapsingHeader("Rigid Body", ImGuiTreeNodeFlags_DefaultOpen |
                                            ImGuiTreeNodeFlags_AllowOverlap);
  frame.headerMin = ImGui::GetItemRectMin();
  frame.headerMax = ImGui::GetItemRectMax();
  const bool popupWasOpen = ImGui::IsPopupOpen("component_menu");
  frame.removeChosen = engine::editor::draw_component_header_menu(
      &entity, 1U, ComponentEditType::RigidBody, true, "Remove Component");
  if (!popupWasOpen) {
    frame.buttonMin = ImGui::GetItemRectMin();
    frame.buttonMax = ImGui::GetItemRectMax();
  }
  frame.menuOpen = ImGui::IsPopupOpen("component_menu");
  ImGui::PopID();
  ImGui::End();
  ImGui::Render();
  return frame;
}

void click(ImVec2 at, Entity entity, HeaderFrame *last) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  *last = header_frame(entity);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  *last = header_frame(entity);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  *last = header_frame(entity);
}

void check_header_options(Entity entity, float fontScale) noexcept {
  ImGui::GetStyle().FontScaleMain = fontScale;
  ImGui::GetIO().AddMousePosEvent(-1.0F, -1.0F);
  HeaderFrame frame = header_frame(entity);
  frame = header_frame(entity);
  const float headerHeight = frame.headerMax.y - frame.headerMin.y;
  g_tests.check(frame.buttonMax.x == frame.headerMax.x,
                "the options button is flush with the header's right edge");
  g_tests.check((frame.buttonMin.y == frame.headerMin.y) &&
                    (frame.buttonMax.y == frame.headerMax.y),
                "the options button spans the header's height");
  g_tests.check((frame.buttonMax.x - frame.buttonMin.x) == headerHeight,
                "the options button is square");
  g_tests.check(frame.buttonMin.x > frame.headerMin.x,
                "the options button sits inside the header");
  g_tests.check(!frame.menuOpen, "the menu starts closed");

  const ImVec2 buttonCenter((frame.buttonMin.x + frame.buttonMax.x) * 0.5F,
                            (frame.buttonMin.y + frame.buttonMax.y) * 0.5F);
  click(buttonCenter, entity, &frame);
  g_tests.check(frame.menuOpen, "clicking the options button opens the menu");

  // Remove Component is the menu's last item: click inside its row, once
  // the menu has had the frame a new popup takes to size itself.
  frame = header_frame(entity);
  ImGuiContext &g = *GImGui;
  g_tests.check(!g.OpenPopupStack.empty() &&
                    (g.OpenPopupStack.back().Window != nullptr),
                "the menu has a window");
  if (g.OpenPopupStack.empty() || (g.OpenPopupStack.back().Window == nullptr)) {
    return;
  }
  const ImRect menu = g.OpenPopupStack.back().Window->Rect();
  const ImVec2 removeRow(menu.Min.x + (menu.GetWidth() * 0.5F),
                         menu.Max.y - g.Style.WindowPadding.y -
                             (ImGui::GetFontSize() * 0.5F));
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(removeRow.x, removeRow.y);
  frame = header_frame(entity);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  frame = header_frame(entity);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  frame = header_frame(entity);
  g_tests.check(frame.removeChosen, "Remove Component is reported");
  frame = header_frame(entity);
  g_tests.check(!frame.menuOpen && !frame.removeChosen,
                "choosing Remove closes the menu and reports once");
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
  engine::editor::editor_set_world(world.get());
  const Entity entity = world->create_scene_object();
  engine::runtime::RigidBody body{};
  g_tests.check(world->add_rigid_body(entity, body), "add a rigid body");
  engine::editor::select_entity(entity, false);

  check_only_present_components();
  check_header_options(entity, 1.0F);
  check_header_options(entity, 2.5F);

  engine::editor::editor_set_world(nullptr);
  ImGui::DestroyContext();
  engine::core::shutdown_logging();
  return g_tests.finish("editor_inspector_header");
}
