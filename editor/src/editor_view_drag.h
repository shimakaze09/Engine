// Declares the Scene view's camera drags -- right-button flythrough,
// Alt+left orbit and Alt+middle pan -- as one state machine, with the
// mouse capture each holds. A captured drag hides the cursor and reads
// the platform's relative motion, so the camera keeps turning at the
// screen edge, and the cursor reappears where the drag began, as in
// Unreal's viewport and Godot's freelook. The machine drives the platform
// through platform_begin_mouse_capture / platform_end_mouse_capture and
// knows nothing of ImGui, so a test steps it frame by frame.

#pragma once

#include <cstdint>

namespace engine::editor {

enum class ViewDragKind : std::uint8_t { None, Fly, Orbit, Pan };

struct ViewDragState final {
  ViewDragKind kind = ViewDragKind::None;
  /// The platform holds the mouse for this drag, and whether this drag
  /// has asked for it (a refusal is not asked again until the next drag).
  bool captured = false;
  bool captureAsked = false;
  /// The drag has moved past the click slop. A flythrough captures only
  /// then: a right press released within the slop is a click that opens
  /// the Scene view's menu, and it never hides the cursor.
  bool moved = false;
  /// The press lay on the Scene view's image, so a click opens the menu.
  bool pressInImage = false;
  /// Where the cursor was when the drag began, and where it goes back to.
  float pressX = 0.0F;
  float pressY = 0.0F;
  /// Motion since the press, summed from the relative deltas, which keep
  /// counting while the cursor is held in place.
  float travelX = 0.0F;
  float travelY = 0.0F;
};

/// One frame of what the Scene view saw.
struct ViewDragInput final {
  /// The Scene view is hovered and no gizmo drag holds the mouse: a drag
  /// may start.
  bool canStart = false;
  /// The cursor lies on the Scene view's image.
  bool overImage = false;
  bool altHeld = false;
  /// Pressed this frame: the only way a drag starts.
  bool rightPressed = false;
  bool leftPressed = false;
  bool middlePressed = false;
  /// Held now.
  bool rightDown = false;
  bool leftDown = false;
  bool middleDown = false;
  /// The cursor, in window units.
  float mouseX = 0.0F;
  float mouseY = 0.0F;
  /// The platform's relative motion this frame, in window units.
  float deltaX = 0.0F;
  float deltaY = 0.0F;
  /// Escape, a lost window focus or Play starting: the drag ends now.
  bool cancel = false;
};

/// What the frame's drag does.
struct ViewDragStep final {
  /// The drag that moves the camera this frame, by (deltaX, deltaY).
  ViewDragKind active = ViewDragKind::None;
  float deltaX = 0.0F;
  float deltaY = 0.0F;
  /// A right click: open the Scene view's menu at (menuX, menuY).
  bool openMenu = false;
  float menuX = 0.0F;
  float menuY = 0.0F;
};

/// Advances the drag by one frame: starts one on a press the view may
/// take, captures the mouse once it moves (an orbit or pan at once),
/// ends it on its button's release or a cancel and puts the cursor back
/// where it began.
ViewDragStep step_view_drag(ViewDragState &state,
                            const ViewDragInput &input) noexcept;

/// Ends any drag at once and lets the mouse go, the cursor put back where
/// the drag began: the Scene view hidden, Play starting, the editor
/// shutting down. Only a press starts a drag, so a button still held
/// starts nothing.
void cancel_view_drag(ViewDragState &state) noexcept;

} // namespace engine::editor
