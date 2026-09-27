// Declares the camera frustum overlay: a camera's view volume drawn as
// debug lines in the editor's Scene view, for the selected camera.

#pragma once

#include "engine/renderer/camera.h"

namespace engine::editor {

/// Draws a camera's frustum as its 12 edges through the core debug_draw
/// queue, with the renderer's projection so orthographic cameras draw true.
void draw_camera_frustum_wireframe(const renderer::CameraState &camera,
                                   float aspectRatio) noexcept;

} // namespace engine::editor
