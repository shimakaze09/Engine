// Declares the editor's recovery copy: when a run ends fatally, the
// unsaved scene is written to the project's Recovery/ folder so the work
// survives the crash, as Unity keeps open scenes in a _Recovery folder and
// Unreal restores its autosaves.

#pragma once

#include <cstddef>

namespace engine::editor {

/// Writes a recovery copy of the open scene when it has unsaved changes,
/// to `<project data>/Recovery/<scene>-YYYYMMDD-HHMMSS.scene` through the
/// staged atomic write, and its path into `outPath`. During Play the copy
/// is the scene as it was before Play (the Play snapshot), not the play
/// world. False, with `outPath` emptied, when there is no editor World,
/// nothing is unsaved, or the copy cannot be written (logged). Runs on the
/// way out of a fatal: it forces a World left mid-frame back to its Input
/// phase so the scene can be read.
bool write_recovery_copy(char *outPath, std::size_t capacity) noexcept;

} // namespace engine::editor
