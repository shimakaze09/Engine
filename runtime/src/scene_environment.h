// Declares the binding of the scene's sky light to the renderer's
// environment: the first SkyLightComponent in the World names an
// environment map, and the renderer lights the scene from the cubemap it
// loads to (renderer::set_skybox_texture). The binding resolves and loads
// only when what the scene names changes, so a frame with nothing new costs
// one comparison.

#pragma once

#include <cstdint>

#include "engine/content/asset_catalog.h"
#include "engine/core/asset_identity.h"
#include "engine/renderer/texture_loader.h"

namespace engine::runtime {

class World;

/// Loads an environment map from a VFS virtual path into a cubemap;
/// kInvalidTextureHandle on failure. Main thread, render device live.
using EnvironmentLoadFn = renderer::TextureHandle (*)(const char *virtualPath,
                                                      void *userData) noexcept;
/// Releases a cubemap the load function made.
using EnvironmentReleaseFn = void (*)(renderer::TextureHandle cubemap,
                                      void *userData) noexcept;

/// What the scene's environment is bound to. Owned by the frame pipeline.
struct SceneEnvironment final {
  /// The reference the first sky light named when it was last bound.
  core::AssetRef ref{};
  /// What `ref` resolved to; kInvalidAssetId when it did not.
  content::AssetId assetId = content::kInvalidAssetId;
  /// The cubemap the renderer lights the scene from; the binding owns it.
  renderer::TextureHandle cubemap = renderer::kInvalidTextureHandle;
  /// The catalog generation `ref` was last resolved against, so a
  /// reference that did not resolve or load is tried again once the
  /// catalog changes rather than every frame.
  std::uint64_t catalogGeneration = 0U;
  /// True once `ref` has been bound, whether or not it resolved.
  bool bound = false;
};

/// Binds the renderer's environment to the World's first sky light. When
/// the reference it names differs from the bound one, the old cubemap is
/// released and the new reference is resolved through the catalog (it must
/// be an Environment asset), loaded and set; the resolved id is written
/// back to the component for the editor's picker. A reference that does
/// not resolve or load is logged once and leaves the scene with no
/// environment until it or the catalog changes. No sky light clears the
/// environment. Main thread, outside render prep.
void update_scene_environment(World *world, const content::AssetCatalog *catalog,
                              SceneEnvironment *state, EnvironmentLoadFn load,
                              EnvironmentReleaseFn release,
                              void *userData) noexcept;

/// Clears the renderer's environment if it is the bound cubemap, releases
/// the cubemap, and forgets the binding.
void release_scene_environment(SceneEnvironment *state,
                               EnvironmentReleaseFn release,
                               void *userData) noexcept;

} // namespace engine::runtime
