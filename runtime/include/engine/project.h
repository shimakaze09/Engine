// Declares the project entry points the editor, the player and tools
// share. open_project turns a project on disk (its directory, or its
// .project document) into the EngineConfig that runs it: the document is
// read and validated, its content root must exist, and so must the
// startup scene and main script it names, so a project that cannot run is
// refused here with a reason rather than half-starting.
// configure_without_project points a config at no project, as the project
// hub runs. create_project makes a new project from a template, whole or
// not at all. The project-switch handoff carries the choice of the next
// project (or the hub) out of a run to the executable's loop, which shuts
// the engine down and bootstraps it again, as Godot's project manager
// relaunches its editor.

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
  /// Each package's mount ("packages/<name>") and directory, absolute, in
  /// the document's order, and the table the config's `packages` points
  /// at. The table points into this storage; open_project sets it, so a
  /// copy of the storage must not be used in its place.
  char packageMounts[content::kMaxProjectPackages]
                    [content::kProjectPackageNameCapacity + 16U] = {};
  char packageRoots[content::kMaxProjectPackages][kProjectOsPathCapacity] = {};
  ContentMount packages[content::kMaxProjectPackages] = {};
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
  /// A package the document depends on has no directory.
  PackageMissing,
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
/// the .project file, the editor's asset root and startup scene, the main
/// script, the script limits, the packages, and the project GUID that
/// names its per-user data. Every other config field is left as the caller set
/// it. On failure logs an Error naming the path and the reason, and leaves
/// `*storage` and `*config` untouched. Cold path: resolves paths through
/// std::filesystem.
std::expected<void, ProjectOpenFailure>
open_project(const char *path, ProjectStorage *storage,
             EngineConfig *config) noexcept;

/// The Lua limits a project runs under.
struct ScriptLimits final {
  /// Instructions all scripts share per frame; 0 is unlimited.
  int instructionLimit = scripting::kDefaultInstructionLimit;
  /// Bytes the Lua allocator may hold; 0 is unlimited.
  std::size_t memoryLimitBytes = scripting::kDefaultMemoryLimit;
};

/// The limits a project's document asks for: each limit it sets, and the
/// engine's default for each it leaves unset.
ScriptLimits
project_script_limits(const content::ProjectScriptLimits &limits) noexcept;

/// A short English description of `kind`, for a tool's error line.
const char *project_open_failure_text(ProjectOpenFailureKind kind) noexcept;

/// Points `*config` at no project: empties the asset root, the .project
/// file, the editor's asset root and startup scene and the main script,
/// restores the default script limits, drops the packages, and clears the
/// project GUID, so
/// bootstrap mounts only the engine's content. Every other field is left as the
/// caller set it.
void configure_without_project(EngineConfig *config) noexcept;

/// Why a project was not created.
enum class ProjectCreateFailureKind : std::uint8_t {
  /// A null or empty argument, or no room for the result.
  InvalidArgument,
  /// The name is not one a .project document accepts (empty, too long, a
  /// path separator or a character some filesystem refuses).
  InvalidName,
  /// The location is not a directory.
  LocationMissing,
  /// Something already exists where the project would go.
  AlreadyExists,
  /// The template is not a directory, holds no content root, or holds a
  /// file that cannot be read.
  TemplateUnusable,
  /// A path does not fit kProjectOsPathCapacity.
  PathTooLong,
  /// Writing the staged project, or moving it into place, failed.
  WriteFailed,
};

/// Longest template file create_project copies; a template holds a
/// starting scene and script, far below it.
inline constexpr std::size_t kMaxProjectTemplateFileBytes = 1024U * 1024U;

/// Creates the project `name` in the directory `location`: the directory
/// `<location>/<name>` holding a copy of `templateDirectory` (its content
/// root is "assets", with the startup scene "assets/main.scene" and, when
/// the template has it, the main script "assets/main.lua") and a new
/// "<name>.project" with a fresh GUID. Everything is written into the
/// hidden sibling "<location>/.<name>.creating", each file durably, and
/// renamed into place only when complete, so a failure (logged with the
/// reason) leaves nothing at `<location>/<name>`; a stage left by an
/// interrupted earlier attempt is replaced. On success writes the new
/// .project file's path to `outProjectFile`, ready for open_project.
/// Cold path: std::filesystem, one heap buffer per call.
std::expected<void, ProjectCreateFailureKind>
create_project(const char *location, const char *name,
               const char *templateDirectory, char *outProjectFile,
               std::size_t capacity) noexcept;

/// A short English description of `kind`, for a dialog or a tool.
const char *project_create_failure_text(ProjectCreateFailureKind kind) noexcept;

/// Asks the executable running the engine to switch projects once this
/// run ends: to the project at `path` (as open_project takes it), or, for
/// an empty path, back to the project hub. Also asks the platform to quit,
/// so the run ends at the end of this frame. False, with nothing
/// requested, for a null path or one longer than kProjectOsPathCapacity
/// allows. A later request replaces an earlier one. Main thread.
bool request_project_switch(const char *path) noexcept;

/// Takes the pending switch, if any, clearing it: true with the project's
/// path in `out` (empty for the hub) and `*toHub` set when one was
/// requested; false, `out` emptied, when none was, which means the run
/// ended to quit, or when the path does not fit `capacity` (which leaves
/// it pending; kProjectOsPathCapacity always fits).
bool take_project_switch(char *out, std::size_t capacity, bool *toHub) noexcept;

/// Writes the sample project the build puts beside the executables
/// (`<app dir>/samples/island`) into `out`, the project the editor and the
/// player open when none is named. False, with `out` emptied, when the
/// executable's directory is unknown, the path does not fit `capacity`
/// whole, or no such directory exists (an installed or web build).
bool find_bundled_sample_project(char *out, std::size_t capacity) noexcept;

} // namespace engine
