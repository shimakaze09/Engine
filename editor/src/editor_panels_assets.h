// Declares the editor asset browser panel, asset tree, and import inspector.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "engine/core/engine_stats.h"

namespace engine::editor {

/// Draws the asset browser with thumbnails and import settings.
void draw_asset_browser_panel() noexcept;

} // namespace engine::editor
