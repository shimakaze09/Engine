// Verifies the two halves of the editor's first-launch layout that a test
// can reach without a display: a row of panel buttons wraps to the next
// line when the panel is too narrow for it rather than clipping the second
// button (the Entities panel's Add Primitive), and the main window's
// geometry travels through the layout file's preferences section, so the
// window a session left is the one the next one opens.

#include "editor_panels_main.h"
#include "editor_preferences.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "imgui.h"

#include "../test_harness.h"

#include <cstring>

namespace {

/// Where "Add Primitive" landed after "Create Entity" in a window `width`
/// wide: on the first button's line, and wholly inside the content edge.
struct SecondButton final {
  bool sharesLine = false;
  bool whole = false;
};

SecondButton draw_button_row(float width) noexcept {
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(width, 200.0F));
  ImGui::Begin("Entities", nullptr, ImGuiWindowFlags_NoSavedSettings);
  ImGui::Button("Create Entity");
  const float firstTop = ImGui::GetItemRectMin().y;
  engine::editor::same_line_if_button_fits("Add Primitive");
  ImGui::Button("Add Primitive");
  SecondButton result{};
  result.sharesLine = (ImGui::GetItemRectMin().y == firstTop);
  const float contentRight =
      ImGui::GetWindowPos().x + width - ImGui::GetStyle().WindowPadding.x;
  result.whole = ImGui::GetItemRectMax().x <= contentRight + 0.5F;
  ImGui::End();
  ImGui::Render();
  return result;
}

} // namespace

int main() {
  engine::tests::TestContext t;
  static_cast<void>(engine::core::initialize_logging());

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

  // --- A wide panel keeps both buttons on one line; a panel narrower
  // than the pair (200 % scaling at the default dock width) wraps the
  // second to its own line instead of clipping it to "A".
  const SecondButton wide = draw_button_row(600.0F);
  t.check(wide.sharesLine && wide.whole, "a wide panel keeps one row");
  ImGui::GetStyle().ScaleAllSizes(2.0F);
  ImGui::GetStyle().FontScaleMain = 2.0F;
  const SecondButton narrow = draw_button_row(300.0F);
  t.check(!narrow.sharesLine && narrow.whole,
          "a narrow panel wraps Add Primitive, whole, to the next line");

  // --- Window geometry rides in the preferences section. Without a
  // platform window (this test) the geometry that was read is carried
  // back out unchanged, so a headless run never forgets it.
  t.check(engine::core::initialize_cvars(), "initialize cvars");
  engine::editor::register_editor_preferences();
  const char stored[] = "[EnginePreferences][Editor]\n"
                        "WindowSize=1500x900\n"
                        "WindowMaximized=1\n\n";
  ImGui::LoadIniSettingsFromMemory(stored, sizeof(stored) - 1U);
  char section[256] = {};
  const std::size_t written =
      engine::editor::editor_preferences_section(section, sizeof(section));
  t.check((written > 0U) &&
              (std::strstr(section, "WindowSize=1500x900") != nullptr) &&
              (std::strstr(section, "WindowMaximized=1") != nullptr),
          "the layout file carries the window geometry it read");
  // With no window to reopen, applying the stored geometry does nothing
  // and does not fail.
  engine::editor::apply_stored_window_geometry();

  const char malformed[] = "[EnginePreferences][Editor]\n"
                           "WindowSize=0x900\n\n";
  ImGui::LoadIniSettingsFromMemory(malformed, sizeof(malformed) - 1U);
  const std::size_t rewritten =
      engine::editor::editor_preferences_section(section, sizeof(section));
  t.check((rewritten > 0U) &&
              (std::strstr(section, "WindowSize=1500x900") != nullptr),
          "a geometry with no size is ignored, keeping the last good one");

  engine::core::shutdown_cvars();
  ImGui::DestroyContext();
  engine::core::shutdown_logging();
  return t.finish("editor_first_launch_layout");
}
