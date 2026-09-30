// The asset catalog a run names its content through: how one is made and
// what fills it. The engine's run makes its one catalog here and fills it
// at boot; a tool that runs no engine, such as engine_validate, makes and
// fills its own the same way, so an identity resolves identically in both.

#pragma once

#include <memory>

#include "engine/content/asset_catalog.h"

namespace engine {
struct EngineConfig;
} // namespace engine

namespace engine::runtime {

/// Allocates an empty catalog; nullptr when memory is short. The catalog
/// grows in pages as it is filled, so this costs only its index.
std::unique_ptr<content::AssetCatalog> create_asset_catalog() noexcept;

/// Catalogues everything a run of `config` can name, in the order its
/// identities take precedence: the engine's own content (skipped when
/// `config.engineRoot` is empty), the project's content (skipped when
/// `config.assetRoot` is empty), each package after it, and the built-in
/// meshes, which ship with the engine and are found on no disk. False when
/// any mount does not index cleanly; the walk has already logged every
/// offending path.
bool catalogue_engine_content(content::AssetCatalog *catalog,
                              const EngineConfig &config) noexcept;

} // namespace engine::runtime
