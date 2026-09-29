// Declares the project hub: the screen the editor shows when it runs with
// no project open, as Unity Hub and Godot's project manager do. It lists
// the projects opened recently (kept per user), opens one by its
// .project document, and creates one from the engine's empty-project
// template in a folder the user picks. Choosing a project hands it to the
// executable through engine::request_project_switch; the editor itself
// never swaps projects inside a run.

#pragma once

#include <cstddef>

#include "editor_recent_list.h"

namespace engine::editor {

/// The hub's window, which fills the editor's viewport.
inline constexpr const char *kProjectHubWindow = "Projects";

/// The projects opened recently, most recent first, kept per user.
RecentList &recent_projects() noexcept;

/// Writes the name a project is shown under: the .project document's stem,
/// or the directory's name for a directory. False, `out` emptied, when the
/// path names nothing or the name does not fit.
bool project_display_name(const char *path, char *out,
                          std::size_t capacity) noexcept;

/// Records the project the editor was started with at the front of the
/// recent list; nothing when no project is open. Called once at editor
/// startup.
void project_hub_note_open_project() noexcept;

/// With no recent projects at all, lists the sample beside the executable,
/// so a first start offers something to open.
void project_hub_seed_bundled_sample() noexcept;

/// Opens the project at `path` (a directory or a .project document):
/// checks it opens, puts it at the front of the recent list and asks for
/// the switch. False, with the reason shown in the hub, when it does not
/// open.
bool project_hub_open(const char *path) noexcept;

/// Creates the project `name` in `location` from the engine's
/// empty-project template and opens it. False, with the reason shown in
/// the New Project window, when it cannot be created.
bool project_hub_create(const char *location, const char *name) noexcept;

/// Shows the Open Project dialog (a .project document); the choice is
/// opened by project_hub_poll_dialogs.
void project_hub_request_open_dialog() noexcept;

/// Takes the answers of the hub's dialogs; called every editor frame, in
/// a project as in the hub, so File > Open Project works from both.
void project_hub_poll_dialogs() noexcept;

/// File > Close Project: ends play, then goes back to the hub once any
/// unsaved scene or material is saved or discarded.
void project_hub_close_project() noexcept;

/// The reason the last open or create failed; "" when none did.
const char *project_hub_error() noexcept;

/// Draws the hub over the whole viewport.
void draw_project_hub() noexcept;

/// Forgets the hub's per-session state (open dialogs, typed fields, the
/// last error); the recent list is re-read on next use.
void project_hub_reset() noexcept;

} // namespace engine::editor
