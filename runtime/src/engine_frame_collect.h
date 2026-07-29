// Declares per-frame scene data collection: renderer light/capture gathering
// and the diagnostics counters the pipeline logs.

#pragma once

#include <cstddef>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/command_buffer.h"

namespace engine {

namespace runtime {
class World;
}

/// Mesh asset counts per lifecycle state, for diagnostics.
struct MeshAssetStateCounts final {
  std::size_t ready = 0U;
  std::size_t loading = 0U;
  std::size_t failed = 0U;
};

/// Rigid bodies with non-zero motion, for diagnostics.
std::size_t count_moving_rigid_bodies(const runtime::World &world) noexcept;

/// Total mesh components, for diagnostics.
std::size_t count_mesh_components(const runtime::World &world) noexcept;

/// Tallies mesh assets by state, for diagnostics.
MeshAssetStateCounts
count_mesh_asset_states(const renderer::AssetDatabase *assets) noexcept;

/// Mesh components whose assets are Ready, for diagnostics.
std::size_t
count_ready_mesh_components(const runtime::World &world,
                            const renderer::AssetDatabase *assets) noexcept;

/// Gathers the world's lights into the renderer's scene light data.
renderer::SceneLightData
collect_scene_lights(const runtime::World &world) noexcept;

/// Gathers enabled scene-capture requests; returns the count written.
std::size_t collect_scene_captures(const runtime::World &world,
                                   renderer::SceneCaptureRequest *outRequests,
                                   std::size_t capacity) noexcept;

} // namespace engine
