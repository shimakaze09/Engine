// Declares asset database types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_metadata.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/material.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

// AssetId and kInvalidAssetId are defined in asset_metadata.h (included above).

enum class AssetState : std::uint8_t { Unloaded, Loading, Ready, Failed };

/// One mesh slot: id, GPU handle, source path, refcount, residency.
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

/// One texture slot: id, GPU handle, source path, refcount, state.
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

/// One material slot: id, source path, and the fully resolved parameters
/// (parent-chain overrides are baked at load time so render prep reads a
/// flat record).
struct MaterialAssetRecord final {
  AssetId id = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
  Material params{};
  AssetState state = AssetState::Unloaded;
};

/// Fixed-slot asset tables (meshes with tombstones, textures, materials,
/// metadata).
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

  static constexpr std::size_t kMaxMaterialAssets = 1024U;
  std::array<MaterialAssetRecord, kMaxMaterialAssets> materialAssets{};
  std::array<bool, kMaxMaterialAssets> materialOccupied{};

  static constexpr std::size_t kMaxMetadata = 4096U;
  std::array<AssetMetadata, kMaxMetadata> metadata{};
  std::array<bool, kMaxMetadata> metadataOccupied{};

  std::uint64_t currentFrame = 0ULL;
};

/// Bumps the frame counter used for last-access stamps.
void advance_asset_database_frame(AssetDatabase *database) noexcept;

/// 64-bit FNV-1a id from the canonicalized path.
AssetId make_asset_id_from_path(const char *path) noexcept;
/// 64-bit content-hash id from the file bytes; 0 on read failure.
AssetId make_asset_id_from_file(const char *path) noexcept;
/// Inserts or updates a mesh record; false when the table is full.
bool register_mesh_asset(AssetDatabase *database, AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept;
/// Marks a mesh asset as requested and loading without queuing a sync load.
bool request_mesh_asset_streaming_load(AssetDatabase *database, AssetId id,
                                       const char *sourcePath) noexcept;
/// Lifecycle state for the id (Unloaded when unknown).
AssetState mesh_asset_state(const AssetDatabase *database, AssetId id) noexcept;
/// Sets the requested value for mesh asset state.
bool set_mesh_asset_state(AssetDatabase *database, AssetId id, AssetState state,
                          MeshHandle runtimeMesh) noexcept;
/// True when a streaming load was requested for the id.
bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   AssetId id) noexcept;
/// GPU handle for the id (touches last-access); invalid unless Ready.
MeshHandle resolve_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Increments the refcount; false when the id is unknown.
bool retain_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Decrements the refcount; false when unknown or already zero.
bool release_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Resets every table to empty.
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

// Material asset management. Materials are CPU parameter blocks; records
// hold parent-resolved values, so lookups are flat and mutation-free
// (safe from parallel render-prep jobs).
/// Inserts or updates a material record; false when the table is full.
bool register_material_asset(AssetDatabase *database, AssetId id,
                             const char *sourcePath,
                             const Material &params) noexcept;
/// Resolved parameters for the id, or nullptr when absent (no access
/// stamps are touched — safe to call from parallel jobs).
const Material *find_material_params(const AssetDatabase *database,
                                     AssetId id) noexcept;
/// Lifecycle state for the material id (Unloaded when unknown).
AssetState material_asset_state(const AssetDatabase *database,
                                AssetId id) noexcept;

// Texture asset management.
bool register_texture_asset(AssetDatabase *database, AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept;
/// Lifecycle state for the texture id (Unloaded when unknown).
AssetState texture_asset_state(const AssetDatabase *database,
                               AssetId id) noexcept;
/// Sets the requested value for texture asset state.
bool set_texture_asset_state(AssetDatabase *database, AssetId id,
                             AssetState state,
                             TextureHandle runtimeTexture) noexcept;
/// GPU texture handle for the id; invalid unless Ready.
TextureHandle resolve_texture_asset(AssetDatabase *database,
                                    AssetId id) noexcept;
/// Increments the texture refcount; false when unknown.
bool retain_texture_asset(AssetDatabase *database, AssetId id) noexcept;
/// Decrements the texture refcount; false when unknown or zero.
bool release_texture_asset(AssetDatabase *database, AssetId id) noexcept;

// Metadata management.
bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept;
/// Finds the matching object or resource for asset metadata.
const AssetMetadata *find_asset_metadata(const AssetDatabase *database,
                                         AssetId id) noexcept;
/// Adds a tag to the id's metadata; false when unknown or tags full.
bool add_asset_tag(AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
/// True when the id's metadata carries the tag.
bool asset_has_tag(const AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
/// Collects up to maxIds ids carrying the tag; returns the count.
std::size_t query_assets_by_tag(const AssetDatabase *database, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept;
/// Collects up to maxIds ids of the given type; returns the count.
std::size_t query_assets_by_type(const AssetDatabase *database,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept;

// Dependency queries.
std::size_t get_dependencies(const AssetDatabase *database, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept;

/// Records a directed dependency edge id -> depId; false when full.
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
