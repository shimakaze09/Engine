// Implements the Scene view's camera-drag state machine and the mouse
// capture it holds (editor_view_drag.h).

#include "editor_view_drag.h"

#include "editor_scene_query.h"
#include "engine/core/platform.h"

namespace engine::editor {

namespace {

bool button_held(ViewDragKind kind, const ViewDragInput &input) noexcept {
  switch (kind) {
  case ViewDragKind::Fly:
    return input.rightDown;
  case ViewDragKind::Orbit:
    return input.leftDown;
  case ViewDragKind::Pan:
    return input.middleDown;
  case ViewDragKind::None:
    break;
  }
  return false;
}

/// Ends the drag, letting the mouse go with the cursor where it began.
void finish(ViewDragState &state) noexcept {
  if (state.captured) {
    core::platform_end_mouse_capture(state.pressX, state.pressY);
  }
  state.kind = ViewDragKind::None;
  state.captured = false;
}

void start(ViewDragState &state, ViewDragKind kind,
           const ViewDragInput &input) noexcept {
  state.kind = kind;
  state.captured = false;
  state.captureAsked = false;
  state.moved = false;
  state.pressInImage = input.overImage;
  state.pressX = input.mouseX;
  state.pressY = input.mouseY;
  state.travelX = 0.0F;
  state.travelY = 0.0F;
}

} // namespace

ViewDragStep step_view_drag(ViewDragState &state,
                            const ViewDragInput &input) noexcept {
  ViewDragStep step{};

  if (state.kind != ViewDragKind::None) {
    if (input.cancel) {
      finish(state);
      return step;
    }
    if (!button_held(state.kind, input)) {
      // A right press that never left the slop is a click, not a flight.
      const bool click = (state.kind == ViewDragKind::Fly) && !state.moved &&
                         state.pressInImage;
      finish(state);
      if (click) {
        step.openMenu = true;
        step.menuX = state.pressX;
        step.menuY = state.pressY;
      }
      return step;
    }
  } else {
    // Only a press starts a drag, so a button still held after a cancel
    // starts nothing until it is pressed again.
    if (input.cancel || !input.canStart) {
      return step;
    }
    if (input.rightPressed) {
      start(state, ViewDragKind::Fly, input);
    } else if (input.altHeld && input.leftPressed) {
      start(state, ViewDragKind::Orbit, input);
    } else if (input.altHeld && input.middlePressed) {
      start(state, ViewDragKind::Pan, input);
    } else {
      return step;
    }
    // An orbit or pan is a drag from its press; only a right press may
    // still be a click.
    state.moved = (state.kind != ViewDragKind::Fly);
  }

  state.travelX += input.deltaX;
  state.travelY += input.deltaY;
  if (!state.moved && !within_click_slop(state.travelX, state.travelY)) {
    state.moved = true;
  }
  if (state.moved && !state.captureAsked) {
    // A refusal leaves the drag working uncaptured, as before capture
    // existed; it is not asked again until the next drag.
    state.captureAsked = true;
    state.captured = core::platform_begin_mouse_capture();
  }
  step.active = state.kind;
  step.deltaX = input.deltaX;
  step.deltaY = input.deltaY;
  return step;
}

void cancel_view_drag(ViewDragState &state) noexcept {
  if (state.kind == ViewDragKind::None) {
    return;
  }
  finish(state);
}

} // namespace engine::editor
