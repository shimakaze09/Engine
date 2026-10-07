// Declares the scene templates File > New Scene from Template offers, as
// Unity's New Scene dialog offers its scene templates: the engine's Basic
// scene (the empty-project template's startup scene, a camera and a
// light), then every scene in the open project's templates/ folder. A
// template opens as an untitled scene with nothing unsaved; the template's
// own file is never written.

#pragma once

#include <cstddef>

namespace engine::editor {

/// Most templates the menu lists; a project's folder holding more lists
/// the first ones by name.
inline constexpr std::size_t kMaxSceneTemplates = 32U;

/// Longest template label, terminator included; a longer file name is
/// cut short for display only.
inline constexpr std::size_t kSceneTemplateLabelCapacity = 64U;

/// Longest template path, terminator included; a template whose path
/// does not fit is not listed, since a cut path names another file.
inline constexpr std::size_t kSceneTemplatePathCapacity = 512U;

/// The project folder, below its content root, whose scenes are templates.
inline constexpr const char *kProjectSceneTemplatesFolder = "templates";

/// The engine's Basic template, below the engine content root.
inline constexpr const char *kBasicSceneTemplate =
    "templates~/empty_project/assets/main.scene";

struct SceneTemplate final {
  char label[kSceneTemplateLabelCapacity] = {};
  char path[kSceneTemplatePathCapacity] = {};
  /// True for the engine's own template; false for one of the project's.
  bool builtIn = false;
};

/// Fills `out` with the templates to offer: the engine's Basic template,
/// below `engineRoot`, when its file exists, then the .scene files
/// directly in `assetRoot`'s templates/ folder, by name. Returns how many
/// were written, at most `capacity`. An empty root is skipped. Cold path:
/// reads the directory.
std::size_t list_scene_templates_in(const char *engineRoot,
                                    const char *assetRoot, SceneTemplate *out,
                                    std::size_t capacity) noexcept;

/// list_scene_templates_in for the running engine's content root and the
/// open project's.
std::size_t list_scene_templates(SceneTemplate *out,
                                 std::size_t capacity) noexcept;

} // namespace engine::editor
