// Declares the Project Settings window (Edit > Project Settings..., as in
// Unity; Godot's Project > Project Settings): the settings a project
// carries in its .project document, for everyone who opens it, where
// Preferences hold one user's editor settings. Its Scripting section edits
// the Lua sandbox limits: the instructions every script shares per frame
// and the memory the Lua allocator may hold. Its Physics section names the
// collision layers and sets which layers collide (Unity's Layer Collision
// Matrix). Each section's Apply saves the document through its staged
// writer and only then applies the change to the running engine.

#pragma once

#include <cstddef>

#include "engine/content/project_document.h"

namespace engine::runtime {
class World;
} // namespace engine::runtime

namespace engine::editor {

/// The limits as the window edits them: the values in force, 0 meaning
/// unlimited.
struct ProjectSettingsDraft final {
  int instructionLimit = 0;
  int memoryLimitMiB = 0;
};

/// The limits `limits` puts in force: each it sets, the engine's default
/// for each it leaves unset.
ProjectSettingsDraft
project_settings_draft(const content::ProjectScriptLimits &limits) noexcept;

/// The engine's default limits.
ProjectSettingsDraft default_project_settings_draft() noexcept;

/// Why `draft` cannot be applied, or nullptr when it can: a limit outside
/// the document's range, or a memory limit at or below the `memoryUsed`
/// bytes the running scripts already hold.
const char *project_settings_problem(const ProjectSettingsDraft &draft,
                                     std::size_t memoryUsed) noexcept;

/// Saves `draft` into the document at `projectFile` and, once it is
/// written, applies it to the running scripts. The document is read
/// afresh so nothing else in it is lost, and a limit equal to the engine's
/// default is left unset, so a project that never changes one writes no
/// "scripting" section. A refused draft, a document that will not read or
/// a failed write leaves the file and the running limits as they were,
/// and logs why.
bool save_project_settings(const char *projectFile,
                           const ProjectSettingsDraft &draft) noexcept;

/// Writes why `layers` cannot be applied into `out` and returns true, or
/// returns false when they can: a name that is not a name token or repeats
/// another ignoring case, or a one-sided matrix.
bool project_physics_problem(const content::ProjectCollisionLayers &layers,
                             char *out, std::size_t capacity) noexcept;

/// Saves `layers` into the document at `projectFile` and, once it is
/// written, makes them the project's layers for Lua and the Inspector and
/// installs their matrix on `world` (when not null). The document is read
/// afresh so nothing else in it is lost; the default layers write no
/// "physics" section. A refused set, a document that will not read or a
/// failed write leaves the file, the project's layers and the world as
/// they were, and logs why.
bool save_project_physics(const char *projectFile,
                          const content::ProjectCollisionLayers &layers,
                          runtime::World *world) noexcept;

/// Registers the cvar that shows the window.
void register_project_settings() noexcept;

/// Draws the window for the open project while
/// editor.show_project_settings is set.
void draw_project_settings_panel() noexcept;

/// Draws the window for the document at `projectFile` (the panel passes
/// the open project's); for tests.
void draw_project_settings_window(const char *projectFile) noexcept;

} // namespace engine::editor
