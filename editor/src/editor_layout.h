// Declares the editor's dock/window layout persistence (issue #313): the
// engine owns the layout file instead of ImGui's default writer, staging
// it atomically into the per-user save directory rather than truncating
// an imgui.ini in whatever directory the editor was launched from.

#pragma once

#include <cstddef>

namespace engine::editor {

/// Resolves the layout file's absolute path under the per-user save
/// directory (or the test override); false when that directory is
/// unavailable or the path would truncate.
bool editor_layout_path(char *out, std::size_t capacity) noexcept;

/// Disables ImGui's own ini writer on the current context and loads any
/// stored layout. Must run before the first frame, while the context
/// exists. False when there is no context; a fresh profile with no
/// stored layout still returns true, since that is not a failure.
bool editor_layout_initialize() noexcept;

/// Loads the stored layout into the current ImGui context. False when
/// none is stored or it is unreadable — both leave the context on
/// ImGui's defaults, which the dockspace builder then fills in.
bool editor_layout_load() noexcept;

/// Stages the current ImGui layout through the atomic writer. False when
/// there is no context, ImGui yields no settings, or the staged write
/// failed; every false path leaves an already-stored layout byte-for-byte
/// intact rather than replacing it with a shorter or empty document.
bool editor_layout_save() noexcept;

/// Saves only when ImGui has flagged its settings dirty, clearing the
/// flag; runs once per frame in place of ImGui's own periodic writer.
void editor_layout_save_if_dirty() noexcept;

/// Test-only override for the layout directory; an empty string restores
/// the real per-user save directory. Exists so tests never read or write
/// the real user's save directory.
void editor_layout_set_directory_override_for_tests(
    const char *directory) noexcept;

} // namespace engine::editor
