// Verifies the Scene view's camera drags (editor_view_drag.h) against the
// real platform capture, on a headless window: a flythrough holds the
// mouse once it moves past the click slop and keeps turning on relative
// motion however far it goes, lets go on release with the cursor back
// where it began, and a right click within the slop captures nothing and
// opens the menu. Orbit and pan hold the mouse from their press. Escape,
// a lost window focus and Play starting end a drag at once, and a button
// still held after a cancel starts nothing until it is pressed again.

#include "editor_view_drag.h"

#include <SDL3/SDL.h>

#include "../test_harness.h"
#include "engine/core/platform.h"
#include "engine/core/platform_event.h"

namespace {

using engine::editor::cancel_view_drag;
using engine::editor::step_view_drag;
using engine::editor::ViewDragInput;
using engine::editor::ViewDragKind;
using engine::editor::ViewDragState;
using engine::editor::ViewDragStep;

engine::tests::TestContext g_tests;

constexpr float kPressX = 300.0F;
constexpr float kPressY = 200.0F;

/// A frame over the Scene view's image, with nothing pressed or moved.
ViewDragInput over_view() noexcept {
  ViewDragInput input{};
  input.canStart = true;
  input.overImage = true;
  input.mouseX = kPressX;
  input.mouseY = kPressY;
  return input;
}

ViewDragInput right_press() noexcept {
  ViewDragInput input = over_view();
  input.rightPressed = true;
  input.rightDown = true;
  return input;
}

ViewDragInput right_held(float dx, float dy) noexcept {
  ViewDragInput input = over_view();
  input.rightDown = true;
  input.deltaX = dx;
  input.deltaY = dy;
  return input;
}

/// Where SDL's record of the cursor is, in window units.
void cursor(float *x, float *y) noexcept {
  static_cast<void>(SDL_GetMouseState(x, y));
}

void check_right_click_opens_the_menu() noexcept {
  ViewDragState state{};
  ViewDragStep step = step_view_drag(state, right_press());
  g_tests.check(step.active == ViewDragKind::Fly,
                "a right press over the view starts a flythrough");
  step = step_view_drag(state, right_held(2.0F, 2.0F));
  g_tests.check(!engine::core::platform_mouse_captured(),
                "motion within the click slop does not capture the mouse");
  step = step_view_drag(state, over_view());
  g_tests.check(step.openMenu && (step.menuX == kPressX) &&
                    (step.menuY == kPressY),
                "a release within the slop opens the menu where it was "
                "pressed");
  g_tests.check((state.kind == ViewDragKind::None) &&
                    !engine::core::platform_mouse_captured(),
                "and nothing is held after it");
}

void check_flythrough_captures_and_restores() noexcept {
  ViewDragState state{};
  static_cast<void>(step_view_drag(state, right_press()));
  ViewDragStep step = step_view_drag(state, right_held(6.0F, 0.0F));
  g_tests.check(engine::core::platform_mouse_captured(),
                "a flythrough past the slop captures the mouse");
  g_tests.check((step.active == ViewDragKind::Fly) && (step.deltaX == 6.0F),
                "and the motion that took it there turns the camera");

  // Far more than any screen is wide: the held cursor never reaches an
  // edge, so every frame's motion still turns the camera.
  float turned = 0.0F;
  for (int frame = 0; frame < 100; ++frame) {
    step = step_view_drag(state, right_held(50.0F, -1.0F));
    turned += step.deltaX;
  }
  g_tests.check(turned == 5000.0F,
                "5000 pixels of relative motion all reach the camera");
  g_tests.check(engine::core::platform_mouse_captured(),
                "the mouse stays held while the button is");

  step = step_view_drag(state, over_view());
  g_tests.check(!step.openMenu, "a flight's release opens no menu");
  g_tests.check((state.kind == ViewDragKind::None) &&
                    !engine::core::platform_mouse_captured(),
                "releasing the button lets the mouse go");
  float x = 0.0F;
  float y = 0.0F;
  cursor(&x, &y);
  g_tests.check((x == kPressX) && (y == kPressY),
                "the cursor is back where the flythrough began");
}

void check_orbit_and_pan_capture_at_once() noexcept {
  ViewDragState state{};
  ViewDragInput press = over_view();
  press.altHeld = true;
  press.leftPressed = true;
  press.leftDown = true;
  ViewDragStep step = step_view_drag(state, press);
  g_tests.check((step.active == ViewDragKind::Orbit) &&
                    engine::core::platform_mouse_captured(),
                "Alt+left orbits and holds the mouse from its press");
  ViewDragInput held = over_view();
  held.leftDown = true; // Alt may be let go mid-orbit
  held.deltaX = 3.0F;
  step = step_view_drag(state, held);
  g_tests.check((step.active == ViewDragKind::Orbit) && (step.deltaX == 3.0F),
                "the orbit follows the button, not Alt");
  static_cast<void>(step_view_drag(state, over_view()));
  g_tests.check(!engine::core::platform_mouse_captured(),
                "releasing it lets the mouse go");

  press = over_view();
  press.altHeld = true;
  press.middlePressed = true;
  press.middleDown = true;
  step = step_view_drag(state, press);
  g_tests.check((step.active == ViewDragKind::Pan) &&
                    engine::core::platform_mouse_captured(),
                "Alt+middle pans and holds the mouse from its press");
  static_cast<void>(step_view_drag(state, over_view()));
  g_tests.check(!engine::core::platform_mouse_captured(),
                "and lets it go on release");

  ViewDragInput plainLeft = over_view();
  plainLeft.leftPressed = true;
  plainLeft.leftDown = true;
  step = step_view_drag(state, plainLeft);
  g_tests.check((step.active == ViewDragKind::None) &&
                    !engine::core::platform_mouse_captured(),
                "a left press without Alt is a pick, not a camera drag");
}

void check_cancels() noexcept {
  // Escape (or Play starting, which cancels the same way) mid-flight.
  ViewDragState state{};
  static_cast<void>(step_view_drag(state, right_press()));
  static_cast<void>(step_view_drag(state, right_held(20.0F, 0.0F)));
  ViewDragInput escape = right_held(1.0F, 0.0F);
  escape.cancel = true;
  ViewDragStep step = step_view_drag(state, escape);
  g_tests.check((step.active == ViewDragKind::None) &&
                    !engine::core::platform_mouse_captured(),
                "a cancel ends the flight and lets the mouse go");
  step = step_view_drag(state, right_held(20.0F, 0.0F));
  g_tests.check(step.active == ViewDragKind::None,
                "the button still held after a cancel flies no more");
  static_cast<void>(step_view_drag(state, over_view()));
  step = step_view_drag(state, right_press());
  g_tests.check(step.active == ViewDragKind::Fly,
                "once it is up, the next press flies again");
  static_cast<void>(step_view_drag(state, over_view()));

  // A lost window focus: the platform lets go on its own, and the drag
  // ends when the editor hands the loss on as a cancel.
  static_cast<void>(step_view_drag(state, right_press()));
  static_cast<void>(step_view_drag(state, right_held(20.0F, 0.0F)));
  g_tests.check(engine::core::platform_mouse_captured(), "flying, held");
  SDL_Event focusLost{};
  focusLost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  static_cast<void>(SDL_PushEvent(&focusLost));
  engine::core::PlatformEvent polled{};
  bool lost = false;
  while (engine::core::platform_poll_event(&polled)) {
    lost = lost ||
           (polled.kind == engine::core::PlatformEventKind::WindowFocusLost);
  }
  g_tests.check(lost && !engine::core::platform_mouse_captured(),
                "a lost focus lets the mouse go at once");
  ViewDragInput afterLoss = right_held(0.0F, 0.0F);
  afterLoss.cancel = lost;
  step = step_view_drag(state, afterLoss);
  g_tests.check((step.active == ViewDragKind::None) &&
                    (state.kind == ViewDragKind::None),
                "and the flight ends with it");
  static_cast<void>(step_view_drag(state, over_view()));

  // The Scene view hidden, Play starting or the editor shutting down.
  static_cast<void>(step_view_drag(state, right_press()));
  static_cast<void>(step_view_drag(state, right_held(20.0F, 0.0F)));
  cancel_view_drag(state);
  g_tests.check((state.kind == ViewDragKind::None) &&
                    !engine::core::platform_mouse_captured(),
                "cancel_view_drag ends the drag and lets the mouse go");
  step = step_view_drag(state, right_held(5.0F, 0.0F));
  g_tests.check(step.active == ViewDragKind::None,
                "and the button still held starts nothing");

  // A press the view may not take (not hovered, or a gizmo drag).
  state = ViewDragState{};
  ViewDragInput elsewhere = right_press();
  elsewhere.canStart = false;
  step = step_view_drag(state, elsewhere);
  g_tests.check(step.active == ViewDragKind::None,
                "a press away from the view starts no drag");
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::core::PlatformConfig config{};
  config.headless = true;
  config.title = "editor view drag test";
  if (!engine::core::initialize_platform(config)) {
    g_tests.fail("initialize a headless platform");
    return g_tests.finish("editor view drag");
  }
  check_right_click_opens_the_menu();
  check_flythrough_captures_and_restores();
  check_orbit_and_pan_capture_at_once();
  check_cancels();
  engine::core::shutdown_platform();
  return g_tests.finish("editor view drag");
}
