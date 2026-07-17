// Declares asset database types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_metadata.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

// AssetId and kInvalidAssetId are defined in asset_metadata.h (included above).

enum class AssetState : std::uint8_t { Unloaded, Loading, Ready, Failed };

/// Stores mesh asset record data used by the engine.
struct MeshAssetRecord final {
  AssetId id = kInvalidAssetId;
  MeshHandle runtimeMesh = kInvalidMeshHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  std::uint64_t lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  AssetState state = AssetState::Unloaded;
  bool requestedResident = false;
};

/// Stores texture asset record data used by the engine.
struct TextureAssetRecord final {
  AssetId id = kInvalidAssetId;
  TextureHandle runtimeTexture = kInvalidTextureHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  std::uint64_t lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  AssetState state = AssetState::Unloaded;
  bool requestedResident = false;
};

/// Stores asset database data used by the engine.
struct AssetDatabase final {
  static constexpr std::size_t kMaxMeshAssets = 4096U;
  std::array<MeshAssetRecord, kMaxMeshAssets> meshAssets{};
  std::array<bool, kMaxMeshAssets> occupied{};
  // Slots freed by unregister_mesh_asset: not occupied (iteration skips them)
  // but probe chains continue through them; inserts reuse them.
  std::array<bool, kMaxMeshAssets> meshTombstoned{};

  static constexpr std::size_t kMaxTextureAssets = 512U;
  std::array<TextureAssetRecord, kMaxTextureAssets> textureAssets{};
  std::array<bool, kMaxTextureAssets> textureOccupied{};

  static constexpr std::size_t kMaxMetadata = 4096U;
  std::array<AssetMetadata, kMaxMetadata> metadata{};
  std::array<bool, kMaxMetadata> metadataOccupied{};

  std::uint64_t currentFrame = 0ULL;
};

/// Handles advance asset database frame.
void advance_asset_database_frame(AssetDatabase *database) noexcept;

/// Handles make asset id from path.
AssetId make_asset_id_from_path(const char *path) noexcept;
/// Handles make asset id from file.
AssetId make_asset_id_from_file(const char *path) noexcept;
/// Handles register mesh asset.
bool register_mesh_asset(AssetDatabase *database, AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept;
/// Marks a mesh asset as requested and loading without queuing a sync load.
bool request_mesh_asset_streaming_load(AssetDatabase *database, AssetId id,
                                       const char *sourcePath) noexcept;
/// Handles mesh asset state.
AssetState mesh_asset_state(const AssetDatabase *database, AssetId id) noexcept;
/// Sets the requested value for mesh asset state.
bool set_mesh_asset_state(AssetDatabase *database, AssetId id, AssetState state,
                          MeshHandle runtimeMesh) noexcept;
/// Handles mesh asset requested resident.
bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   AssetId id) noexcept;
/// Handles resolve mesh asset.
MeshHandle resolve_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Handles retain mesh asset.
bool retain_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Handles release mesh asset.
bool release_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Handles clear asset database.
void clear_asset_database(AssetDatabase *database) noexcept;

// Low-level mesh slot access shared by the database and the asset manager.
/// Returns the record slot for an id, or kMaxMeshAssets when absent.
std::size_t find_mesh_asset_record_slot(const AssetDatabase *database,
                                        AssetId id) noexcept;
/// Finds the id's slot or claims an empty/tombstoned one (occupied is set and
/// the id written for fresh claims). Returns kMaxMeshAssets when full.
std::size_t claim_mesh_asset_record_slot(AssetDatabase *database,
                                         AssetId id) noexcept;
/// Frees a mesh record slot for reuse. Requires refCount == 0 and no live
/// runtimeMesh (unload first); the slot becomes a tombstone so probe chains
/// stay intact. Fixes unbounded slot growth over long content-streaming
/// sessions.
bool unregister_mesh_asset(AssetDatabase *database, AssetId id) noexcept;

// Texture asset management.
bool register_texture_asset(AssetDatabase *database, AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept;
/// Handles texture asset state.
AssetState texture_asset_state(const AssetDatabase *database,
                               AssetId id) noexcept;
/// Sets the requested value for texture asset state.
bool set_texture_asset_state(AssetDatabase *database, AssetId id,
                             AssetState state,
                             TextureHandle runtimeTexture) noexcept;
/// Handles resolve texture asset.
TextureHandle resolve_texture_asset(AssetDatabase *database,
                                    AssetId id) noexcept;
/// Handles retain texture asset.
bool retain_texture_asset(AssetDatabase *database, AssetId id) noexcept;
/// Handles release texture asset.
bool release_texture_asset(AssetDatabase *database, AssetId id) noexcept;

// Metadata management.
bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept;
/// Finds the matching object or resource for asset metadata.
const AssetMetadata *find_asset_metadata(const AssetDatabase *database,
                                         AssetId id) noexcept;
/// Adds a value or component to the target system for asset tag.
bool add_asset_tag(AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
/// Handles asset has tag.
bool asset_has_tag(const AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
/// Handles query assets by tag.
std::size_t query_assets_by_tag(const AssetDatabase *database, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept;
/// Handles query assets by type.
std::size_t query_assets_by_type(const AssetDatabase *database,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept;

// Dependency queries.
std::size_t get_dependencies(const AssetDatabase *database, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept;

/// Adds a value or component to the target system for asset dependency.
bool add_asset_dependency(AssetDatabase *database, AssetId id,
                          AssetId depId) noexcept;

/// Load an asset and all its dependencies (depth-first, dependency-first).
/// Returns false if a cycle is detected or if any dependency fails to resolve.
/// The `loadCallback` is invoked for each asset that needs loading, in
/// dependency order. It receives the AssetId and should return true if the
/// load succeeds.
bool load_with_dependencies(AssetDatabase *database, AssetId rootId,
                            bool (*loadCallback)(AssetDatabase *db, AssetId id,
                                                 void *userData),
                            void *userData) noexcept;

} // namespace engine::renderer
