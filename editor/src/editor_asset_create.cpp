// Implements creating a new folder, material, scene or Lua script from the
// Assets panel: the name check, the jail and never-replace checks, the
// write through each kind's own writer, and the sidecar identity that
// makes a new file referenceable, rolled back if it cannot be given.

#include "editor_asset_create.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "engine/content/asset_sidecar.h"
#include "engine/core/atomic_file.h"
#include "engine/core/diagnostic.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace engine::editor {

namespace {

namespace fs = std::filesystem;

constexpr const char *kLogChannel = "editor";

/// A new script: the lifecycle hooks the engine calls, empty.
constexpr const char kNewScript[] =
    "-- An entity script: attach it to an entity's Script component.\n"
    "-- 'self' is an opaque, generation-checked entity handle.\n"
    "local M = {}\n"
    "\n"
    "-- Called once when Play is pressed, or when the entity is created\n"
    "-- during play.\n"
    "function M.on_begin_play(_self)\n"
    "end\n"
    "\n"
    "-- Called once per fixed step with the fixed delta; input read here is\n"
    "-- seen once per step.\n"
    "function M.on_fixed_tick(_self, _dt)\n"
    "end\n"
    "\n"
    "-- Called once per rendered frame that advanced simulation.\n"
    "function M.on_tick(_self, _dt)\n"
    "end\n"
    "\n"
    "return M\n";

/// True when `path` names something on disk, a dangling link included.
bool occupied(const fs::path &path) noexcept {
  std::error_code ec{};
  return fs::symlink_status(path, ec).type() != fs::file_type::not_found;
}

/// True when `folder` is the asset root or a folder inside it.
bool inside_asset_root(const fs::path &folder) noexcept {
  std::error_code ec{};
  const fs::path root =
      fs::weakly_canonical(fs::path(active_config().editorAssetRoot), ec);
  if (ec) {
    return false;
  }
  const fs::path resolved = fs::weakly_canonical(folder, ec);
  if (ec || !fs::is_directory(resolved, ec) || ec) {
    return false;
  }
  const fs::path relative = resolved.lexically_relative(root);
  return !relative.empty() && (*relative.begin() != "..");
}

/// True when `name` ends with `suffix`.
bool ends_with(const char *name, const char *suffix) noexcept {
  const std::size_t nameLength = std::strlen(name);
  const std::size_t suffixLength = std::strlen(suffix);
  return (suffixLength > 0U) && (nameLength > suffixLength) &&
         (std::strcmp(name + (nameLength - suffixLength), suffix) == 0);
}

/// Writes the file for `kind` at `path`, whose VFS form is `virtualPath`.
bool write_new_file(NewAssetKind kind, const char *path,
                    const char *virtualPath) noexcept {
  switch (kind) {
  case NewAssetKind::Material:
    return runtime::editor_create_material(virtualPath);
  case NewAssetKind::Scene: {
    std::unique_ptr<runtime::World> empty(new (std::nothrow) runtime::World());
    return (empty != nullptr) && runtime::save_scene(*empty, path);
  }
  case NewAssetKind::LuaScript:
    return core::atomic_write_file(path, kNewScript, sizeof(kNewScript) - 1U);
  case NewAssetKind::Folder:
  default:
    return false;
  }
}

NewAssetResult refuse(NewAssetFailure failure, const char *name) noexcept {
  char message[256] = {};
  std::snprintf(message, sizeof(message), "could not create '%s': %s",
                (name != nullptr) ? name : "", new_asset_failure_text(failure));
  core::log_message(core::LogLevel::Error, kLogChannel, message);
  NewAssetResult result{};
  result.failure = failure;
  return result;
}

} // namespace

const char *new_asset_label(NewAssetKind kind) noexcept {
  switch (kind) {
  case NewAssetKind::Folder:
    return "Folder";
  case NewAssetKind::Material:
    return "Material";
  case NewAssetKind::Scene:
    return "Scene";
  case NewAssetKind::LuaScript:
  default:
    return "Lua Script";
  }
}

const char *new_asset_default_name(NewAssetKind kind) noexcept {
  switch (kind) {
  case NewAssetKind::Folder:
    return "New Folder";
  case NewAssetKind::Material:
    return "New Material";
  case NewAssetKind::Scene:
    return "New Scene";
  case NewAssetKind::LuaScript:
  default:
    // A script is named as Lua modules are, so require() can name it.
    return "new_script";
  }
}

const char *new_asset_extension(NewAssetKind kind) noexcept {
  switch (kind) {
  case NewAssetKind::Material:
    return ".mat";
  case NewAssetKind::Scene:
    return ".scene";
  case NewAssetKind::LuaScript:
    return ".lua";
  case NewAssetKind::Folder:
  default:
    return "";
  }
}

const char *new_asset_failure_text(NewAssetFailure failure) noexcept {
  switch (failure) {
  case NewAssetFailure::None:
    return "created";
  case NewAssetFailure::InvalidName:
    return "a name cannot be empty, \".\" or \"..\", hold / \\ : * ? \" < > | "
           "or a control character, start or end with a space, end with a "
           "dot, or be longer than 128 bytes";
  case NewAssetFailure::OutsideProject:
    return "the folder is not inside the project's assets";
  case NewAssetFailure::AlreadyExists:
    return "something with that name is already there";
  case NewAssetFailure::TooLong:
    return "the path would be too long";
  case NewAssetFailure::WriteFailed:
  default:
    return "it could not be written; see the log";
  }
}

bool new_asset_name_valid(const char *name) noexcept {
  if ((name == nullptr) || (name[0] == '\0') || (std::strcmp(name, ".") == 0) ||
      (std::strcmp(name, "..") == 0)) {
    return false;
  }
  const std::size_t length = std::strlen(name);
  if ((length > kMaxNewAssetName) || (name[0] == ' ') ||
      (name[length - 1U] == ' ') || (name[length - 1U] == '.')) {
    return false;
  }
  for (std::size_t i = 0U; i < length; ++i) {
    const auto c = static_cast<unsigned char>(name[i]);
    if ((c < 0x20U) || (c == 0x7FU) ||
        (std::strchr("/\\:*?\"<>|", c) != nullptr)) {
      return false;
    }
  }
  return true;
}

NewAssetResult create_new_asset(NewAssetKind kind, const char *folderOsPath,
                                const char *name) noexcept {
  if (!new_asset_name_valid(name)) {
    return refuse(NewAssetFailure::InvalidName, name);
  }
  const char *folderText =
      ((folderOsPath == nullptr) || (folderOsPath[0] == '\0'))
          ? active_config().editorAssetRoot
          : folderOsPath;
  const fs::path folder(folderText);
  if (!inside_asset_root(folder)) {
    return refuse(NewAssetFailure::OutsideProject, name);
  }

  const char *extension = new_asset_extension(kind);
  std::string fileName(name);
  if (!ends_with(name, extension)) {
    fileName += extension;
  }
  const std::string pathText = (folder / fileName).string();
  NewAssetResult result{};
  if (pathText.size() >= sizeof(result.osPath)) {
    return refuse(NewAssetFailure::TooLong, name);
  }
  std::memcpy(result.osPath, pathText.c_str(), pathText.size() + 1U);

  char sidecar[kMaxAssetIndexPath + 8U] = {};
  const bool isFile = kind != NewAssetKind::Folder;
  if (isFile &&
      !content::asset_sidecar_path(result.osPath, sidecar, sizeof(sidecar))) {
    return refuse(NewAssetFailure::TooLong, name);
  }
  // A sidecar left by an asset deleted outside the editor would hand the
  // new file the old one's identity, and with it every old reference.
  if (occupied(fs::path(result.osPath)) ||
      (isFile && occupied(fs::path(sidecar)))) {
    return refuse(NewAssetFailure::AlreadyExists, name);
  }

  if (!isFile) {
    std::error_code ec{};
    if (!fs::create_directory(fs::path(result.osPath), ec) || ec) {
      return refuse(NewAssetFailure::WriteFailed, name);
    }
  } else {
    char virtualPath[kMaxAssetIndexPath] = {};
    if (!asset_virtual_path(result.osPath, virtualPath, sizeof(virtualPath))) {
      return refuse(NewAssetFailure::TooLong, name);
    }
    if (!write_new_file(kind, result.osPath, virtualPath)) {
      return refuse(NewAssetFailure::WriteFailed, name);
    }
    if (runtime::editor_establish_asset_identity(result.osPath) !=
        runtime::EditorIdentityResult::Created) {
      std::error_code ec{};
      fs::remove(fs::path(result.osPath), ec);
      return refuse(NewAssetFailure::WriteFailed, name);
    }
  }

  static_cast<void>(rebuild_asset_index());
  core::log_path_diagnostic(core::LogLevel::Info, kLogChannel, result.osPath,
                            "created");
  return result;
}

} // namespace engine::editor
