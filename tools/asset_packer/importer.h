// Declares the importer contract the packer cooks every source through: an
// importer claims one asset type, names the settings that change its
// cooked bytes, finds the files its source pulls in, and cooks the source
// into outputs the packer then certifies with one cook stamp.
//
// Unity's ScriptedImporter and Godot's EditorImportPlugin share this shape:
// an importer is registered for the types it recognizes, carries a version
// that enters the artifact key, and turns one source plus its settings
// into outputs. Here the type comes from the asset type table rather than
// a list of suffixes, so an importer and the catalog can never disagree
// about what a file is.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/content/asset_type_table.h"
#include "engine/content/import_settings_resolve.h"

#include "packer_shared.h"

namespace engine::tools {

/// Everything one cook of one source knows before it starts.
struct CookRequest final {
  const char *inputPath = nullptr;
  const char *outputPath = nullptr;
  /// The settings that apply to the source (import_settings_resolve.h).
  const content::ResolvedImportSettings *settings = nullptr;
  /// The source's content hash.
  std::uint64_t sourceHash = 0ULL;
  /// Every file the source depends on, sorted by path.
  const std::vector<DependencyDigest> *dependencies = nullptr;
  /// The cook key the stamp records: the settings hash with the
  /// importer's logic revision folded in.
  std::uint64_t settingsKey = 0ULL;
};

/// What a cook produced.
struct CookResult final {
  /// Zero on success, else the packer's exit code for the failure, which
  /// the cook has already explained on stderr.
  int exitCode = 0;
  /// Every file the cook committed, in the order the stamp lists them.
  std::vector<std::string> outputs{};
  /// One line for stdout once the outputs are certified.
  std::string summary{};
};

/// One importer.
struct Importer final {
  /// Shown in diagnostics.
  const char *name = "";
  /// The asset type whose source files this importer cooks.
  content::AssetTypeTag claims = content::AssetTypeTag::Unknown;
  /// Folded into the cook key, so a change in what the importer does to
  /// the same source and settings recooks every output it made. Bump on
  /// any such change; kCookToolVersion covers output format changes.
  const char *logicRevision = "";
  /// Hashes the settings fields that change the cooked bytes, and only
  /// those: a field that does not change them must not recook.
  std::uint64_t (*hash_settings)(
      const content::ResolvedImportSettings &settings) = nullptr;
  /// Appends the files the source pulls in (a glTF's buffers and images)
  /// to `outDependencies`, each with its content hash, so a change to one
  /// recooks; may be null. A source that will not parse appends nothing,
  /// and the cook reports it.
  void (*discover_dependencies)(
      const char *inputPath, std::uint64_t assetId,
      std::vector<DependencyDigest> *outDependencies) = nullptr;
  /// Cooks the source.
  CookResult (*cook)(const CookRequest &request) = nullptr;
  /// Whether the outputs are certified by a cook stamp beside the output,
  /// which also lets an unchanged source skip its cook. A texture's
  /// thumbnail carries its own checksum and has no stamp.
  bool stamped = true;
};

/// The importer that claims `tag`, or nullptr when no type of that tag is
/// cooked: those are read by the engine as authored.
const Importer *find_importer(content::AssetTypeTag tag) noexcept;

/// The glTF mesh importer (gltf_importer.cpp).
extern const Importer kGltfMeshImporter;

} // namespace engine::tools
