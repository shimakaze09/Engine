#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/command_buffer.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

using AssetId = std::uint32_t;
inline constexpr AssetId kInvalidAssetId = 0U;

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
};

AssetId make_asset_id_from_path(const char *path) noexcept;
bool register_mesh_asset(AssetDatabase *database,
                         AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept;
AssetState mesh_asset_state(const AssetDatabase *database, AssetId id) noexcept;
bool set_mesh_asset_state(AssetDatabase *database,
                          AssetId id,
                          AssetState state,
                          MeshHandle runtimeMesh) noexcept;
bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   AssetId id) noexcept;
MeshHandle resolve_mesh_asset(const AssetDatabase *database,
                              AssetId id) noexcept;
bool retain_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
bool release_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
void clear_asset_database(AssetDatabase *database) noexcept;

// Texture asset management.
bool register_texture_asset(AssetDatabase *database,
                            AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept;
AssetState texture_asset_state(const AssetDatabase *database,
                               AssetId id) noexcept;
bool set_texture_asset_state(AssetDatabase *database,
                             AssetId id,
                             AssetState state,
                             TextureHandle runtimeTexture) noexcept;
TextureHandle resolve_texture_asset(const AssetDatabase *database,
                                    AssetId id) noexcept;
bool retain_texture_asset(AssetDatabase *database, AssetId id) noexcept;
bool release_texture_asset(AssetDatabase *database, AssetId id) noexcept;

} // namespace engine::renderer
