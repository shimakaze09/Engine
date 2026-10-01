// Verifies that colour fields edit linear colours through their sRGB
// encoding. Material, light and mesh tint colours are linear, as the
// shaders read them; a picker showing the raw floats showed a swatch
// lighter than what the renderer draws. Here the Inspector's generic
// field drawer runs a Light's colour under a headless ImGui context: a
// stored linear 0.2158605 shows as 128 of 255 (read from the channel's
// text-input state), typing 128 into a channel stores 0.2158605, and the
// channels left alone keep their exact value.
// The transfer pair itself is checked across every 8-bit code and above 1.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "../test_harness.h"
#include "editor_panels_inspector_generic.h"
#include "engine/math/color_space.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/world.h"

namespace {

engine::tests::TestContext g_tests;

/// One frame of the Light colour field, focusing drag `focusChannel` of
/// it when not -1; `modified` gathers whether the drawer reported a change.
void frame(engine::runtime::LightComponent *light, int focusChannel,
           bool *modified) noexcept {
  ImGui::NewFrame();
  ImGui::Begin("Probe");
  if (focusChannel >= 0) {
    // The colour field is three drags, R, G and B, in that order.
    ImGui::SetKeyboardFocusHere(focusChannel);
  }
  *modified =
      engine::editor::draw_reflected_field("engine::runtime::LightComponent",
                                           "color", light, nullptr, false) ||
      *modified;
  ImGui::End();
  ImGui::Render();
}

void check_transfer_pair() noexcept {
  using engine::math::linear_to_srgb;
  using engine::math::srgb_to_linear;
  bool roundTrips = true;
  for (int code = 0; code <= 255; ++code) {
    const float encoded = static_cast<float>(code) / 255.0F;
    const float back = linear_to_srgb(srgb_to_linear(encoded));
    // The float round trip loses at most a few ulps of the 0..1 range.
    roundTrips = roundTrips && (std::fabs(back - encoded) <= 2.0e-7F);
  }
  g_tests.check(roundTrips, "every 8-bit code round-trips through linear");
  g_tests.check(std::fabs(srgb_to_linear(128.0F / 255.0F) - 0.2158605F) <=
                    1.0e-7F,
                "code 128 is linear 0.2158605");
  g_tests.check((srgb_to_linear(0.0F) == 0.0F) &&
                    (srgb_to_linear(1.0F) == 1.0F) &&
                    (linear_to_srgb(1.0F) == 1.0F),
                "black and white map to themselves");
  g_tests.check(
      (linear_to_srgb(4.0F) > linear_to_srgb(2.0F)) &&
          (linear_to_srgb(2.0F) > 1.0F) &&
          (std::fabs(srgb_to_linear(linear_to_srgb(4.0F)) - 4.0F) <= 4.0e-6F),
      "an HDR value above 1 continues the curve and round-trips");
}

/// The text of the drag being typed into, as ImGui's input state holds
/// it: the value the field shows for that channel.
std::string active_text() noexcept {
  const ImGuiInputTextState &state = GImGui->InputTextState;
  return std::string(state.TextA.Data, static_cast<std::size_t>(state.TextLen));
}

/// Focuses the field's B drag, the third of its R, G and B drags.
void focus_blue(engine::runtime::LightComponent *light,
                bool *modified) noexcept {
  frame(light, 2, modified);
  frame(light, -1, modified);
}

void press(engine::runtime::LightComponent *light, ImGuiKey key,
           bool *modified) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddKeyEvent(key, true);
  frame(light, -1, modified);
  io.AddKeyEvent(key, false);
  frame(light, -1, modified);
}

/// A fresh headless ImGui context, with no field active.
void begin_imgui() noexcept {
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
}

void check_shown(engine::runtime::LightComponent *light) noexcept {
  // What the field shows: a stored linear 0.2158605 reads as 128 of 255.
  light->color = engine::math::Vec3(1.0F, 0.0F, 0.2158605F);
  bool modified = false;
  focus_blue(light, &modified);
  g_tests.check(active_text() == "128",
                "a linear 0.2158605 shows as 128 of 255");
  press(light, ImGuiKey_Escape, &modified);
  g_tests.check(!modified && (light->color.z == 0.2158605F),
                "showing and leaving the field changes nothing");
}

void check_picked(engine::runtime::LightComponent *light) noexcept {
  // What picking stores: typing 128 into the channel stores 0.2158605.
  light->color = engine::math::Vec3(0.2158605F, 0.0F, 1.0F);
  bool modified = false;
  focus_blue(light, &modified);
  g_tests.check(active_text() == "255", "a linear 1 shows as 255");
  ImGuiIO &io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_A, true);
  frame(light, -1, &modified);
  io.AddKeyEvent(ImGuiKey_A, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  io.AddInputCharactersUTF8("128");
  frame(light, -1, &modified);
  press(light, ImGuiKey_Enter, &modified);

  g_tests.check(modified, "typing 128 reports a change");
  g_tests.check(std::fabs(light->color.z - 0.2158605F) <= 1.0e-7F,
                "typing 128 stores linear 0.2158605");
  g_tests.check((light->color.x == 0.2158605F) && (light->color.y == 0.0F),
                "the channels left alone keep their exact value");
}

} // namespace

int main() {
  engine::runtime::ensure_runtime_reflection_registered();
  check_transfer_pair();
  engine::runtime::LightComponent light{};
  begin_imgui();
  check_shown(&light);
  ImGui::DestroyContext();
  begin_imgui();
  check_picked(&light);
  ImGui::DestroyContext();
  return g_tests.finish("editor_linear_color");
}
