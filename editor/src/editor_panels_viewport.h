// Declares the editor scene viewport panel, gizmos, and collider overlay.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "engine/core/engine_stats.h"

namespace engine::editor {

/// Draws the scene viewport with ImGuizmo transform gizmos.
void draw_scene_viewport_panel() noexcept;

} // namespace engine::editor
