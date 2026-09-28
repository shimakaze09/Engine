// Declares open_project: turns a project on disk (its directory, or its
// .project document) into the EngineConfig that runs it. The document is
// read and validated, its content root must exist, and so must the
// startup scene and main script it names, so a project that cannot run is
// refused here with a reason rather than half-starting. The same entry
// point serves the editor, the player and tools.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "engine/content/project_document.h"
#include "engine/engine.h"

namespace engine {

/// Longest OS path open_project stores; one more than the longest string
/// the engine adopts from its configuration, so every path it hands to
/// bootstrap is one bootstrap accepts.
inline constexpr std::size_t kProjectOsPathCapacity = 260U;

/// What open_project resolves, owned by the caller. The EngineConfig it
/// fills points into this storage, so the storage must outlive the
/// bootstrap that reads the config (bootstrap copies what it keeps).
struct ProjectStorage final {
  content::ProjectDocument document{};
  /// The .project file, absolute.
  char projectFile[kProjectOsPathCapacity] = {};
  /// The directory holding it, absolute.
  char projectDirectory[kProjectOsPathCapacity] = {};
  /// The content root, absolute; mounted at content::kProjectContentMount.
  char contentRoot[kProjectOsPathCapacity] = {};
};

/// Why a project was not opened.
enum class ProjectOpenFailureKind : std::uint8_t {
  /// The path is null or empty.
  InvalidPath,
  /// Nothing at the path, a directory holding no .project document, or a
  /// file that is not one.
  NotFound,
  /// A directory holding more than one .project document; which one was
  /// meant would be a guess.
  Ambiguous,
  /// The document could not be read or was refused; `document` says why.
  DocumentRefused,
  /// The document's content root is not a directory.
  ContentRootMissing,
  /// The startup scene the document names is not a file.
  StartupSceneMissing,
  /// The document names a main script that is not a file.
  MainScriptMissing,
  /// A resolved path does not fit kProjectOsPathCapacity.
  PathTooLong,
};

struct ProjectOpenFailure final {
  ProjectOpenFailureKind kind = ProjectOpenFailureKind::NotFound;
  /// The document's own refusal, for DocumentRefused.
  content::ProjectReadFailure document{};
};

/// Opens the project at `path`: a directory holding exactly one .project
/// document, or the document itself. On success fills `*storage` and
/// points `*config`'s content fields at it: the assets mount and its root,
/// the editor's asset root and startup scene, the main script, and the
/// project GUID that names its per-user data. Every other config field is
/// left as the caller set it. On failure logs an Error naming the path and
/// the reason, and leaves `*storage` and `*config` untouched. Cold path:
/// resolves paths through std::filesystem.
std::expected<void, ProjectOpenFailure>
open_project(const char *path, ProjectStorage *storage,
             EngineConfig *config) noexcept;

/// A short English description of `kind`, for a tool's error line.
const char *project_open_failure_text(ProjectOpenFailureKind kind) noexcept;

} // namespace engine
