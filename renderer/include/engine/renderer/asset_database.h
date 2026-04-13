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

struct MeshAssetRecord final {
  AssetId id = kInvalidAssetId;
  MeshHandle runtimeMesh = kInvalidMeshHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  AssetState state = AssetState::Unloaded;
  bool requestedResident = false;
};

struct TextureAssetRecord final {
  AssetId id = kInvalidAssetId;
  TextureHandle runtimeTexture = kInvalidTextureHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  AssetState state = AssetState::Unloaded;
  bool requestedResident = false;
};

struct AssetDatabase final {
  static constexpr std::size_t kMaxMeshAssets = 4096U;
  std::array<MeshAssetRecord, kMaxMeshAssets> meshAssets{};
  std::array<bool, kMaxMeshAssets> occupied{};

  static constexpr std::size_t kMaxTextureAssets = 512U;
  std::array<TextureAssetRecord, kMaxTextureAssets> textureAssets{};
  std::array<bool, kMaxTextureAssets> textureOccupied{};

  static constexpr std::size_t kMaxMetadata = 4096U;
  std::array<AssetMetadata, kMaxMetadata> metadata{};
  std::array<bool, kMaxMetadata> metadataOccupied{};
};

AssetId make_asset_id_from_path(const char *path) noexcept;
AssetId make_asset_id_from_file(const char *path) noexcept;
bool register_mesh_asset(AssetDatabase *database, AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept;
AssetState mesh_asset_state(const AssetDatabase *database, AssetId id) noexcept;
bool set_mesh_asset_state(AssetDatabase *database, AssetId id, AssetState state,
                          MeshHandle runtimeMesh) noexcept;
bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   AssetId id) noexcept;
MeshHandle resolve_mesh_asset(const AssetDatabase *database,
                              AssetId id) noexcept;
bool retain_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
bool release_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
void clear_asset_database(AssetDatabase *database) noexcept;

// Texture asset management.
bool register_texture_asset(AssetDatabase *database, AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept;
AssetState texture_asset_state(const AssetDatabase *database,
                               AssetId id) noexcept;
bool set_texture_asset_state(AssetDatabase *database, AssetId id,
                             AssetState state,
                             TextureHandle runtimeTexture) noexcept;
TextureHandle resolve_texture_asset(const AssetDatabase *database,
                                    AssetId id) noexcept;
bool retain_texture_asset(AssetDatabase *database, AssetId id) noexcept;
bool release_texture_asset(AssetDatabase *database, AssetId id) noexcept;

// Metadata management.
bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept;
const AssetMetadata *find_asset_metadata(const AssetDatabase *database,
                                         AssetId id) noexcept;
bool add_asset_tag(AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
bool asset_has_tag(const AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
std::size_t query_assets_by_tag(const AssetDatabase *database, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept;
std::size_t query_assets_by_type(const AssetDatabase *database,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept;

} // namespace engine::renderer
