// Names the engine's built-in cube for pipeline suites that need a mesh in
// the scene. The startup scene is the empty 3D template (a camera, a
// directional light and the scene controller), so a suite that draws
// authors its own mesh entity rather than borrowing demo content.

#pragma once

#include <cstdint>

#include "engine/content/asset_metadata.h"
#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::tests {

/// The asset id the bootstrap registers the built-in cube under: derived
/// from its path, as every built-in's is.
inline std::uint64_t builtin_cube_mesh_id() noexcept {
  return content::make_asset_id_from_path("builtin://cube");
}

/// Adds a built-in cube at `position`; kInvalidEntity on failure.
inline runtime::Entity add_builtin_cube(runtime::World &world,
                                        const math::Vec3 &position) noexcept {
  runtime::Transform transform{};
  transform.position = position;
  const runtime::Entity entity = world.create_scene_object(transform);
  runtime::MeshComponent mesh{};
  mesh.meshAssetId = builtin_cube_mesh_id();
  return ((entity != runtime::kInvalidEntity) &&
          world.add_mesh_component(entity, mesh))
             ? entity
             : runtime::kInvalidEntity;
}

} // namespace engine::tests
