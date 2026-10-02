// Declares the project document: "<name>.project" at a project
// directory's root, the file that makes a directory a project (decision
// 0016). It owns the project's identity, its roots, its scene list, its
// startup scene, its main script, its script limits, its named collision
// layers and the packages it depends on; every other per-project table joins it
// as a schema field when the epic that needs it lands.
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

/// The virtual mount every package's content is under: package `name` is
/// mounted at "packages/<name>", disjoint from the content mount and the
/// engine's own.
inline constexpr const char *kProjectPackagesMount = "packages";

/// Most packages a project depends on; a list past it is refused.
inline constexpr std::size_t kMaxProjectPackages = 16U;

/// A package name's capacity, terminator included.
inline constexpr std::size_t kProjectPackageNameCapacity = 49U;

/// One package the project depends on (Unity's manifest dependencies,
/// Godot's addons). Its assets are addressed "packages/<name>/...", with
/// their identities in their own sidecars, exactly as the project's are.
struct ProjectPackage final {
  /// Lower-case letters, digits, '_' and '-', starting with a letter or a
  /// digit, so it is the same file name on every filesystem.
  char name[kProjectPackageNameCapacity] = {};
  /// Where the package comes from. The only kind this schema reads is an
  /// embedded package, the project directory's "packages/<name>".
  char source[kProjectRootCapacity] = {};
};

/// The script limits a project may set, bounded so a typo cannot stop the
/// project opening (a Lua VM that cannot allocate its own libraries fails
/// to start) or overflow a 32-bit size. 0 means unlimited for both.
inline constexpr std::uint32_t kProjectMinInstructionLimit = 100000U;
inline constexpr std::uint32_t kProjectMaxInstructionLimit = 1000000000U;
inline constexpr std::uint32_t kProjectMinMemoryLimitMiB = 16U;
inline constexpr std::uint32_t kProjectMaxMemoryLimitMiB = 2048U;

/// The Lua sandbox limits a project sets, each only when the author chose
/// one; an unset limit runs at the engine's default, so a project the
/// author never tuned follows the engine as its default moves. Written as
/// the optional "scripting" object, omitted when neither is set.
struct ProjectScriptLimits final {
  bool instructionLimitSet = false;
  /// Lua instructions all scripts share per frame; 0 is unlimited,
  /// otherwise within [kProjectMinInstructionLimit,
  /// kProjectMaxInstructionLimit].
  std::uint32_t instructionLimit = 0U;
  bool memoryLimitSet = false;
  /// MiB the Lua allocator may hold; 0 is unlimited, otherwise within
  /// [kProjectMinMemoryLimitMiB, kProjectMaxMemoryLimitMiB].
  std::uint32_t memoryLimitMiB = 0U;
};

/// The bound a project may set on one save slot, in MiB. The engine's
/// default applies while it is unset; the top of the range is the
/// ceiling every save read accepts, so a project can never set a bound
/// its own reads refuse.
inline constexpr std::uint32_t kProjectMinSaveSlotMiB = 1U;
inline constexpr std::uint32_t kProjectMaxSaveSlotMiB = 256U;

/// The game-save settings a project sets. Written as the optional
/// "saves" object, omitted while nothing is set.
struct ProjectSaveSettings final {
  bool maxSlotMiBSet = false;
  /// The largest save slot the game may write, in MiB, within
  /// [kProjectMinSaveSlotMiB, kProjectMaxSaveSlotMiB].
  std::uint32_t maxSlotMiB = 0U;
};

/// Physics collision layers: the 32 bits of Collider::collisionLayer and
/// collisionMask, as Godot's and Unity's are.
inline constexpr std::size_t kMaxCollisionLayers = 32U;

/// A layer name's capacity, terminator included.
inline constexpr std::size_t kCollisionLayerNameCapacity = 32U;

/// The project's collision layers: a name for each bit the author named
/// (Godot's layer_names/3d_physics) and which layers may collide (Unity's
/// Layer Collision Matrix). Names only label the bits a collider already
/// carries, so naming, renaming or clearing one changes no scene. Written
/// as the optional "physics" object, omitted while every name is empty and
/// every pair collides.
struct ProjectCollisionLayers final {
  /// The name of each bit, or empty when the author left it unnamed. A
  /// name is a name token ([A-Za-z0-9_.-], at most 31 characters) and no
  /// two names match ignoring case, so Lua and the editor can look a layer
  /// up by it.
  char names[kMaxCollisionLayers][kCollisionLayerNameCapacity] = {};
  /// Bit j of collides[i] is set when layer i may collide with layer j.
  /// Symmetric, so a pair reads the same either way round; all set by
  /// default, which is the engine's behaviour without a matrix.
  std::uint32_t collides[kMaxCollisionLayers] = {
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
      0xFFFFFFFFU, 0xFFFFFFFFU};
};

/// True when no layer is named and every pair collides.
bool collision_layers_are_default(
    const ProjectCollisionLayers &layers) noexcept;

/// The bit named `name` (matched ignoring case), or -1 when no layer has
/// that name.
int find_collision_layer(const ProjectCollisionLayers &layers,
                         const char *name) noexcept;

/// Sets whether layers `a` and `b` may collide, both ways round. Ignored
/// for a bit past kMaxCollisionLayers.
void set_collision_layer_pair(ProjectCollisionLayers *layers, std::uint32_t a,
                              std::uint32_t b, bool collide) noexcept;

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
  /// The script limits the project sets, if any.
  ProjectScriptLimits scriptLimits{};
  /// The project's collision layer names and matrix.
  ProjectCollisionLayers collisionLayers{};
  /// The project's game-save settings, if any.
  ProjectSaveSettings saveSettings{};
  /// The packages the project depends on, in the author's order; none is
  /// written as no "dependencies" key.
  ProjectPackage packages[kMaxProjectPackages] = {};
  std::size_t packageCount = 0U;
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

/// Checks collision layers against the document's rules: every name a
/// name token unique ignoring case, and a symmetric matrix. The failure
/// names the field as the document spells it ("physics.layers[3].name").
std::expected<void, ProjectReadFailure>
validate_collision_layers(const ProjectCollisionLayers &layers) noexcept;

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
