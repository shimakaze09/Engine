// Declares the project document: "<name>.project" at a project
// directory's root, the file that makes a directory a project (decision
// 0016). It owns the project's identity, its roots, its scene list, its
// startup scene and its main script; every other per-project table joins
// it as a schema field when the epic that needs it lands.
//
// The document is authored and committed. Reading refuses anything this
// schema does not describe exactly: an unknown version, an unknown or
// repeated key, a missing field, a wrong type, and a value that does not
// fit whole. Accepting any of them would let a resave silently drop or
// rewrite what an author (or a newer engine) wrote. Writing validates the
// same rules first and commits through a staged atomic replacement.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "engine/content/asset_identity.h"

namespace engine::content {

/// The one schema this build reads and writes. A reader refuses any
/// other, older or newer, rather than guessing what its fields mean.
inline constexpr std::uint32_t kProjectSchemaVersion = 1U;

/// The file extension that names a project document.
inline constexpr const char *kProjectFileExtension = ".project";

/// Longest document this reader accepts; a project document naming the
/// most scenes it can hold at their longest paths fits well inside it.
inline constexpr std::size_t kMaxProjectDocumentBytes = 64U * 1024U;

/// Most scenes a project lists. A limit the document enforces on read and
/// write, so a list past it is refused rather than cut short.
inline constexpr std::size_t kMaxProjectScenes = 64U;

/// Field capacities, terminator included. A value that does not fit is
/// refused, never truncated: a truncated name or path names something
/// else.
inline constexpr std::size_t kProjectNameCapacity = 64U;
inline constexpr std::size_t kProjectOrganisationCapacity = 128U;
inline constexpr std::size_t kProjectVersionCapacity = 32U;
inline constexpr std::size_t kProjectRootCapacity = 128U;
inline constexpr std::size_t kProjectPathCapacity = 256U;

/// The virtual mount every content path in the document is under. The
/// content root, wherever it is on disk, is mounted here.
inline constexpr const char *kProjectContentMount = "assets";

/// A project document's contents.
struct ProjectDocument final {
  /// The project's name, also its file name: "<name>.project". No path
  /// separators, no characters a filesystem refuses, no control
  /// characters.
  char name[kProjectNameCapacity] = {};
  /// Who makes it; may be empty. Display only.
  char organisation[kProjectOrganisationCapacity] = {};
  /// The project's own version text, e.g. "0.1.0". Display only.
  char version[kProjectVersionCapacity] = {};
  /// The project's persistent identity. Survives a rename or a move of the
  /// directory; per-project data (saves, rebound input) is keyed by it.
  AssetGuid guid{};
  /// Content root, relative to the project directory, mounted at
  /// kProjectContentMount.
  char contentRoot[kProjectRootCapacity] = {};
  /// Derived-data root, relative to the project directory, which version
  /// control ignores. Never equal to or nested with the content root.
  char cacheRoot[kProjectRootCapacity] = {};
  /// The project's scenes, as virtual paths under kProjectContentMount.
  /// Non-empty, no duplicates, in the author's order.
  char scenes[kMaxProjectScenes][kProjectPathCapacity] = {};
  std::size_t sceneCount = 0U;
  /// The scene the player starts in; one of `scenes`.
  char startupScene[kProjectPathCapacity] = {};
  /// The scene-level Lua module the startup scene's controller runs; empty
  /// when the project has none.
  char mainScript[kProjectPathCapacity] = {};
};

/// Why a project document was not read or not accepted.
enum class ProjectReadFailureKind : std::uint8_t {
  /// Nothing at the path.
  Absent,
  /// Present but could not be read.
  Unreadable,
  /// Larger than kMaxProjectDocumentBytes.
  TooLarge,
  /// Read, but not a document this schema describes.
  Malformed,
};

/// A refusal, naming the field it is about ("identity.name", "scenes[3]",
/// "schemaVersion"; empty when the whole document is at fault) and why.
struct ProjectReadFailure final {
  ProjectReadFailureKind kind = ProjectReadFailureKind::Malformed;
  char field[64] = {};
  /// A static, human-readable reason; never null.
  const char *reason = "";
};

/// Parses a document from `length` bytes of `text`. `*out` is written only
/// on success. Never call .value() on the result: with exceptions disabled
/// it aborts; check has_value() and use error().
std::expected<void, ProjectReadFailure>
parse_project_document(const char *text, std::size_t length,
                       ProjectDocument *out) noexcept;

/// Reads and parses the document at `osPath`, logging a refusal with the
/// path, the field and the reason. `*out` is written only on success.
std::expected<void, ProjectReadFailure>
read_project_document(const char *osPath, ProjectDocument *out) noexcept;

/// Checks `document` against every rule the reader enforces, so a writer
/// never produces a file its own reader would refuse.
std::expected<void, ProjectReadFailure>
validate_project_document(const ProjectDocument &document) noexcept;

/// Formats a valid document, one field per line, so two branches that
/// edit different fields merge per field. The same document always
/// formats to the same bytes. False, with `out` emptied, when the document
/// is invalid or does not fit `capacity`.
bool format_project_document(const ProjectDocument &document, char *out,
                             std::size_t capacity,
                             std::size_t *outLength) noexcept;

/// Validates, formats and writes `document` to `osPath` through a staged
/// atomic replacement: a refusal or a failed write leaves the previous
/// file exactly as it was. Logs why it refused.
bool write_project_document(const char *osPath,
                            const ProjectDocument &document) noexcept;

} // namespace engine::content
