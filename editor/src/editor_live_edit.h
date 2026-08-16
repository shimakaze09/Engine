// Declares play-mode live-edit support (issue #159): an explicit opt-in
// path for transient edits to a running World, kept out of undo history,
// plus the "Apply to authored value" queue Stop replays as an ordinary
// undoable edit once the authored scene is restored.

#pragma once

#include <cstddef>

#include "editor_component_registry.h"
#include "editor_session.h"

namespace engine::editor {

/// True when the attached world is Playing or Paused, its current phase
/// legally accepts component mutation (WorldPhase::Input -- true between
/// fixed steps and while paused; see runtime/include/engine/runtime/
/// world.h), and the author has opted in via EditorSession::liveEditEnabled.
/// Every function below no-ops (returns false) when this is false.
bool live_edit_available() noexcept;

/// Writes `after` straight onto the running world's component, bypassing
/// command history entirely (never undoable, never touches scene dirty
/// state); the first live touch of an (entity, type) pair this play
/// session captures the pre-edit value as its baseline so
/// revert_live_component_edit has something to restore. False when live
/// editing is unavailable or the world rejected the value (the world is
/// left unchanged either way).
bool apply_live_component_edit(runtime::Entity entity, ComponentEditType type,
                               const ComponentEditSnapshot &after) noexcept;

/// True when `entity`'s component of `type` has been live-edited at least
/// once this play session and not yet reverted -- drives the Inspector's
/// "edited live" badge.
bool has_live_component_edit(runtime::Entity entity,
                             ComponentEditType type) noexcept;

/// Restores `entity`'s component of `type` to its play-session baseline
/// and drops the tracking entry; false when there is no baseline to
/// restore (nothing was live-edited) or the world rejected the write.
bool revert_live_component_edit(runtime::Entity entity,
                                ComponentEditType type) noexcept;

/// Captures the entity's current running value of `type` and queues it to
/// be written onto the authored scene the next time Stop restores it, as
/// one ordinary undoable command executed after the restore (so it
/// integrates with the normal undo/redo/dirty-state contract instead of
/// bypassing it). Replaces any previously queued entry for the same
/// (entity, type). False when live editing is unavailable, the component
/// is absent, or the queue is full.
bool queue_apply_to_authored(runtime::Entity entity,
                             ComponentEditType type) noexcept;

/// True when `entity`'s component of `type` has a pending apply-to-authored
/// entry queued this play session.
bool has_pending_apply_to_authored(runtime::Entity entity,
                                   ComponentEditType type) noexcept;

/// Drops a queued apply-to-authored entry without ever applying it.
void cancel_apply_to_authored(runtime::Entity entity,
                              ComponentEditType type) noexcept;

/// Clears every live-edit baseline and queued apply-to-authored entry;
/// called on Play start (a fresh session must never see a prior session's
/// tracking) and again once Stop has consumed the queue.
void reset_live_edit_state() noexcept;

/// Number of entries currently queued for apply-to-authored (test hook and
/// the Inspector's "N pending" affordance).
std::size_t pending_apply_to_authored_count() noexcept;

/// Replays every queued apply-to-authored entry as an ordinary undoable
/// ComponentEditCommand against the just-restored authored world, then
/// clears all live-edit tracking; called by stop_play_mode() after a
/// successful restore. Entries whose persistent id no longer resolves
/// (e.g. an entity that only ever existed during Play) are skipped and
/// logged rather than treated as fatal. Returns the number of entries
/// successfully applied.
std::size_t replay_pending_authored_applies() noexcept;

} // namespace engine::editor
