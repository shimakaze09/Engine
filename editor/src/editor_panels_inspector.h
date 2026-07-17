// Declares the editor inspector panel and its reflected field editors.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "engine/core/engine_stats.h"

namespace engine::editor {

/// Draws the selected entity's component inspector.
void draw_inspector_panel() noexcept;

} // namespace engine::editor
