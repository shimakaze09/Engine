#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

namespace engine::renderer {

/// Number of cascades for directional light CSM.
inline constexpr std::size_t kShadowCascadeCount = 4U;

/// Shadow map resolution per cascade (square).
inline constexpr int kShadowMapResolution = 1024;

/// Cascade split distances computed from camera near/far and a log/uniform
/// blend factor (lambda). lambda=1 is fully logarithmic, lambda=0 is uniform.
struct CascadeSplits final {
  float distances[kShadowCascadeCount + 1]{}; // [near, split1..3, far]
};

/// Per-cascade data: light-space view-projection matrix and split distance.
struct CascadeData final {
  math::Mat4 lightViewProjection{};
  float splitDistance = 0.0F;
};

/// Full CSM state for one directional light.
struct ShadowMapState final {
  CascadeData cascades[kShadowCascadeCount]{};
  std::uint32_t depthTextures[kShadowCascadeCount]{};
  std::uint32_t depthFbos[kShadowCascadeCount]{};
  bool initialized = false;
};

/// Compute cascade split distances using a log/uniform blend.
/// @param nearClip Camera near plane distance.
/// @param farClip  Camera far plane distance.
/// @param lambda   Blend factor: 0 = uniform, 1 = logarithmic.
CascadeSplits compute_cascade_splits(float nearClip, float farClip,
                                     float lambda) noexcept;

/// Compute cascade light view-projection matrix for a single cascade.
/// @param viewMatrix     Camera view matrix.
/// @param projMatrix     Camera projection matrix.
/// @param lightDir       Normalized light direction (pointing toward scene).
/// @param cascadeNear    Near split distance for this cascade.
/// @param cascadeFar     Far split distance for this cascade.
/// @param texelSize      Shadow map texel size for stable snapping.
math::Mat4 compute_cascade_matrix(const math::Mat4 &viewMatrix,
                                  const math::Mat4 &projMatrix,
                                  const math::Vec3 &lightDir, float cascadeNear,
                                  float cascadeFar, float texelSize) noexcept;

/// Snap an orthographic projection to texel boundaries to prevent shadow
/// swimming when the camera moves.
math::Mat4 snap_to_texel(const math::Mat4 &lightViewProj,
                         int shadowMapSize) noexcept;

/// Initialize shadow map GPU resources (depth textures + FBOs).
bool initialize_shadow_maps(ShadowMapState &state) noexcept;

/// Destroy shadow map GPU resources.
void shutdown_shadow_maps(ShadowMapState &state) noexcept;

} // namespace engine::renderer
