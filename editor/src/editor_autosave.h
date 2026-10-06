// Declares the editor's autosave and restart recovery, on Unreal's model:
// while the scene has unsaved changes, a copy is written every few minutes
// into the project's per-user data directory, never over the authored
// file; a session marker there says an editor is running; and the next
// launch after a run that did not end cleanly offers the last copy to
// recover, inspect or discard before the startup scene opens.
//
// Files, under <project data>/Autosave/:
//  - session.lock: written when a session with a project starts, removed
//    when it ends cleanly. Present at the next start means the previous
//    run crashed, was killed or stopped on a fatal frame.
//  - autosave.json: the manifest naming the last copy (relative to the
//    project data directory), the scene it was taken from, and when.
//  - scene-a.scene, scene-b.scene: the copies. Each autosave writes the
//    slot the manifest does not name and then commits the manifest, so an
//    interrupted autosave leaves the previous copy and manifest intact.
// A fatal frame's recovery copy (editor_recovery.h) is recorded in the
// same manifest, so it is offered the same way.
//
// Two editors open on one project share these files: the second sees the
// first's marker and offers its copy. Neither copy is lost by that.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::editor {

/// Minutes between autosaves by default: between Blender's 2 and
/// Unreal's 10.
constexpr int kDefaultAutosaveMinutes = 5;
/// The longest interval the preference accepts; 0 turns autosave off.
constexpr int kMaxAutosaveMinutes = 60;

/// The copy a manifest names.
struct AutosaveRecord {
  /// The copy, relative to the project data directory
  /// ("Autosave/scene-a.scene", "Recovery/level-20261005-101500.scene").
  char file[256] = {};
  /// The authored scene the copy was taken from, or "" for an untitled
  /// scene.
  char scenePath[512] = {};
  /// The scene's name as the editor showed it.
  char sceneName[128] = {};
  /// Local time the copy was written, "YYYY-MM-DD HH:MM:SS".
  char savedAt[32] = {};
  /// True when the copy is the scene as it was before Play.
  bool beforePlay = false;
};

/// What the author chose for a pending recovery.
enum class RecoveryChoice : std::uint8_t {
  /// Loads the copy as the authored scene, unsaved: Save writes it over
  /// the authored file, and until then that file is untouched.
  Recover,
  /// Loads the copy as an untitled scene, to look at or Save As
  /// elsewhere; the authored file and the copy are both kept.
  Inspect,
  /// Forgets the copy; the startup scene opens as usual. The copy's file
  /// stays until a later autosave replaces it.
  Discard,
};

/// Starts the autosave session for the open project, once per editor
/// session: reads what the previous run left, arms the recovery prompt
/// when that run did not end cleanly and its copy is still there, and
/// writes the session marker. False, logged, when no project data
/// directory is available; autosave is then off for the session.
bool autosave_begin_session() noexcept;

/// Ends the session. After a clean end the marker and the manifest are
/// removed: the author was asked about unsaved changes on the way out.
/// After autosave_note_fatal_exit both stay, for the next launch.
void autosave_end_session() noexcept;

/// Records that this run is ending on a fatal, so its end is not clean.
void autosave_note_fatal_exit() noexcept;

/// Writes a copy when one is due: the scene is unsaved, autosave is on and
/// its interval has passed since the scene became unsaved or since the
/// last copy. `nowNs` is core::platform_ticks_ns. True when a copy was
/// written this call.
bool autosave_tick(std::uint64_t nowNs) noexcept;

/// Writes a copy of the unsaved scene now, into the next slot, and
/// commits the manifest. During Play the copy is the scene as it was
/// before Play. False, logged, when the session has not begun, nothing is
/// unsaved, the scene cannot be read now, or a write fails; the previous
/// copy and manifest are then intact.
bool autosave_write_now() noexcept;

/// Records `path`, a copy already written under the project data
/// directory, as the one the next launch offers. False, logged, when the
/// path is not under that directory or the manifest cannot be written.
bool autosave_record_copy(const char *path, bool beforePlay) noexcept;

/// True while a recovery waits for the author's choice; the startup scene
/// does not open until it is made.
bool autosave_recovery_pending() noexcept;

/// The copy a pending recovery offers, or nullptr.
const AutosaveRecord *autosave_pending_recovery() noexcept;

/// Applies the author's choice for the pending recovery. False, with the
/// recovery still pending and the prompt showing why, when the copy
/// cannot be loaded now.
bool autosave_choose_recovery(RecoveryChoice choice) noexcept;

/// Why the last choice failed, or "".
const char *autosave_recovery_error() noexcept;

/// The interval preference, in minutes; 0 is off.
int autosave_minutes() noexcept;
/// Sets the interval, clamped to [0, kMaxAutosaveMinutes].
void set_autosave_minutes(int minutes) noexcept;

/// Forgets the session's autosave state (the interval preference stays):
/// run with the rest of the editor's session residue.
void autosave_reset() noexcept;

} // namespace engine::editor
