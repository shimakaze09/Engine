// Declares the bootstrap content surface the pipeline initializes with:
// the built-in primitive mesh ids and the default-scene builders.

#pragma once

#include "engine/renderer/asset_database.h"

namespace engine {

namespace runtime {
class World;
}
namespace renderer {
struct AssetManager;
struct GpuMeshRegistry;
}

/// Asset ids of the built-in procedural meshes registered at bootstrap.
struct BootstrapMeshIds final {
  renderer::AssetId bootstrap = renderer::kInvalidAssetId;
  renderer::AssetId plane = renderer::kInvalidAssetId;
  renderer::AssetId cube = renderer::kInvalidAssetId;
  renderer::AssetId sphere = renderer::kInvalidAssetId;
  renderer::AssetId cylinder = renderer::kInvalidAssetId;
  renderer::AssetId capsule = renderer::kInvalidAssetId;
  renderer::AssetId pyramid = renderer::kInvalidAssetId;
  renderer::AssetId grass = renderer::kInvalidAssetId;
};

/// Loads the sample mesh asset and registers every built-in primitive.
bool load_bootstrap_meshes(renderer::AssetManager *assetManager,
                           renderer::AssetDatabase *assetDatabase,
                           renderer::GpuMeshRegistry *meshRegistry,
                           BootstrapMeshIds *out) noexcept;

/// Creates the default editor scene from the bootstrap meshes.
void create_bootstrap_scene(runtime::World *world,
                            const BootstrapMeshIds &meshIds) noexcept;

} // namespace engine
