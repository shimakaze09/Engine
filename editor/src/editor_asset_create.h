// Declares creating a new asset from the Assets panel: a folder, a
// material, a scene or a Lua script, named by the author before anything
// is written, as Godot's FileSystem dock asks for the name of a New
// Folder, Scene or Script. Nothing that exists is ever replaced, and a new
// file gets its sidecar identity at once, so it can be referenced before
// the next restart; a creation that cannot finish leaves nothing behind.

#pragma once

#include <cstddef>
#include <cstdint>

#include "editor_asset_index.h"

namespace engine::editor {

/// What the Create menu makes.
enum class NewAssetKind : std::uint8_t { Folder, Material, Scene, LuaScript };

/// The longest name an author may give a new asset, extension included.
inline constexpr std::size_t kMaxNewAssetName = 128U;

/// The Create menu's label for `kind` ("Folder", "Material", ...).
const char *new_asset_label(NewAssetKind kind) noexcept;

/// The name the prompt starts with ("New Material", ...), as Unity's.
const char *new_asset_default_name(NewAssetKind kind) noexcept;

/// The file extension `kind` is written with ("" for a folder).
const char *new_asset_extension(NewAssetKind kind) noexcept;

/// Why a creation was refused.
enum class NewAssetFailure : std::uint8_t {
  None,
  /// The name is empty, "." or "..", holds a separator, a character a
  /// file name cannot hold on Windows or a control character, starts or
  /// ends with a space, ends with a dot, or is longer than
  /// kMaxNewAssetName.
  InvalidName,
  /// The folder is not the project's asset folder or one inside it.
  OutsideProject,
  /// Something already has that name there.
  AlreadyExists,
  /// The path does not fit whole.
  TooLong,
  /// The file or folder, or the new file's sidecar, could not be written.
  WriteFailed,
};

/// A human-readable reason for `failure`.
const char *new_asset_failure_text(NewAssetFailure failure) noexcept;

/// What a creation made, or why it made nothing.
struct NewAssetResult final {
  NewAssetFailure failure = NewAssetFailure::None;
  /// The created folder or file.
  char osPath[kMaxAssetIndexPath] = {};
};

/// True when `name` may name a new asset (see NewAssetFailure::InvalidName).
bool new_asset_name_valid(const char *name) noexcept;

/// Creates a `kind` named `name` in `folderOsPath` ("" is the asset
/// root), with the kind's extension added unless `name` already ends with
/// it: an empty folder, a material of the engine's defaults, an empty
/// scene, or a script with the empty lifecycle hooks. A file is given its
/// sidecar identity and catalogued; if that fails the file is removed. The
/// asset index is rebuilt on success. A refusal writes nothing.
NewAssetResult create_new_asset(NewAssetKind kind, const char *folderOsPath,
                                const char *name) noexcept;

} // namespace engine::editor
