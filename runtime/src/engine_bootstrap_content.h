// Declares the bootstrap content surface the pipeline initializes with:
// the built-in primitive mesh ids and the default-scene builders.

#pragma once

#include "engine/renderer/asset_database.h"

namespace engine {

namespace runtime {
class World;
}
namespace content {
struct AssetRequestQueue;
}
namespace renderer {
// The queue container is content-owned; this keeps the
// established renderer service vocabulary in the signatures below.
using AssetManager = content::AssetRequestQueue;
struct GpuMeshRegistry;
}

/// Asset ids of the built-in procedural meshes registered at bootstrap.
struct BootstrapMeshIds final {
  content::AssetId bootstrap = content::kInvalidAssetId;
  content::AssetId plane = content::kInvalidAssetId;
  content::AssetId cube = content::kInvalidAssetId;
  content::AssetId sphere = content::kInvalidAssetId;
  content::AssetId cylinder = content::kInvalidAssetId;
  content::AssetId capsule = content::kInvalidAssetId;
  content::AssetId pyramid = content::kInvalidAssetId;
  content::AssetId grass = content::kInvalidAssetId;
  content::AssetId character = content::kInvalidAssetId;
};

/// Loads the sample mesh asset and registers every built-in primitive.
bool load_bootstrap_meshes(renderer::AssetManager *assetManager,
                           renderer::AssetDatabase *assetDatabase,
                           content::AssetCatalog *catalog,
                           renderer::GpuMeshRegistry *meshRegistry,
                           BootstrapMeshIds *out) noexcept;

/// Creates the default editor scene from the bootstrap meshes.
void create_bootstrap_scene(runtime::World *world,
                            const BootstrapMeshIds &meshIds) noexcept;

} // namespace engine
