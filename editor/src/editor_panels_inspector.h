// Declares the editor inspector panel and its reflected field editors.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#pragma once

#include "editor_component_registry.h"
#include "engine/core/engine_stats.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Draws the selected entity's component inspector.
void draw_inspector_panel() noexcept;

/// Draws the play-mode live-edit row for one component section, once the
/// component has been live-edited this session: Apply to authored value
/// (queues the running value for Stop to replay as an ordinary undoable
/// edit), Revert runtime edit (writes the play-session baseline back and
/// withdraws a queued apply) and, while an apply is queued, Cancel queued
/// apply. A full queue disables Apply for a new pair and says how to free
/// a slot.
void draw_live_edit_row(runtime::Entity entity,
                        ComponentEditType type) noexcept;

} // namespace engine::editor
