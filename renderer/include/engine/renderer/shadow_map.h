// Declares shadow map types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Number of cascades for directional light CSM.
inline constexpr std::size_t kShadowCascadeCount = 4U;
/// How far behind a cascade's light-space slab casters are still
/// rendered: the light projection's near plane is pushed back by this
/// much, and render prep sweeps each camera-culled draw this far along the
/// light direction to decide whether it can shadow the view.
inline constexpr float kShadowCasterSweepDistance = 50.0F;

/// Directional shadow map resolution (square). Every cascade renders at
/// this size, as one tile of the cascade atlas.
inline constexpr int kShadowMapResolution = 2048;

/// Shadow atlases hold their maps as a square grid of equal tiles: map i
/// sits at column i % kShadowAtlasTilesPerRow, row i /
/// kShadowAtlasTilesPerRow, counted from the top-left of the render
/// target. One 2-D depth texture per set keeps the whole set to one
/// sampler register (DXBC caps them at 16, WebGL2 guarantees 16), and
/// needs no texture arrays, which bgfx withholds under Emscripten. The
/// shaders' shadow tap carries the same layout.
inline constexpr int kShadowAtlasTilesPerRow = 2;

/// Top-left pixel of one atlas tile.
struct ShadowAtlasTile final {
  int x = 0;
  int y = 0;
};

/// The tile map `index` occupies in an atlas of `tileResolution` tiles.
ShadowAtlasTile shadow_atlas_tile(std::size_t index,
                                  int tileResolution) noexcept;

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

/// Full CSM state for one directional light. The cascades are tiles of
/// one depth atlas (tile c = cascade c) sampled through a single
/// register; each cascade renders into its tile through a viewport on the
/// atlas's one render target.
struct ShadowMapState final {
  CascadeData cascades[kShadowCascadeCount]{};
  DeviceTextureHandle depthAtlasTexture{};
  RenderTargetHandle atlasTarget{};
  bool initialized = false;
};

/// Return the configured shadow texture size for a cascade.
int shadow_cascade_resolution(std::size_t cascadeIndex) noexcept;

/// Compute cascade split distances using a log/uniform blend.
/// @param nearClip Camera near plane distance.
/// @param farClip  Camera far plane distance.
/// @param lambda   Blend factor: 0 = uniform, 1 = logarithmic.
CascadeSplits compute_cascade_splits(float nearClip, float farClip,
                                     float lambda) noexcept;

/// Compute cascade light view-projection matrix for a single cascade.
/// @param viewMatrix     Camera view matrix.
/// @param projMatrix     Camera projection matrix.
/// @param projNear       Camera near plane the projection was built with.
/// @param projFar        Camera far plane the projection was built with.
/// @param lightDir       Normalized light direction (pointing toward scene).
/// @param cascadeNear    Near split distance for this cascade.
/// @param cascadeFar     Far split distance for this cascade.
/// @param shadowMapSize  Cascade shadow texture size for stable snapping.
/// projNear/projFar are passed explicitly: recovering them from the
/// matrix used perspective-only algebra that produced silently wrong
/// cascade slabs for an orthographic camera.
math::Mat4 compute_cascade_matrix(const math::Mat4 &viewMatrix,
                                  const math::Mat4 &projMatrix,
                                  float projNear, float projFar,
                                  const math::Vec3 &lightDir, float cascadeNear,
                                  float cascadeFar,
                                  int shadowMapSize) noexcept;

/// Snap an orthographic projection to texel boundaries to prevent shadow
/// swimming when the camera moves.
math::Mat4 snap_to_texel(const math::Mat4 &lightViewProj,
                         int shadowMapSize) noexcept;

/// Initialize shadow map GPU resources (the depth atlas and its target).
bool initialize_shadow_maps(ShadowMapState &state) noexcept;

/// Destroy shadow map GPU resources.
void shutdown_shadow_maps(ShadowMapState &state) noexcept;

// ---- Spot Light Shadow Maps ----

inline constexpr std::size_t kMaxSpotShadowLights = 4U;
inline constexpr int kSpotShadowMapResolution = 1024;

/// One spot light's shadow slot: its light matrix (its map is tile s of
/// the atlas on SpotShadowState).
struct SpotShadowData final {
  math::Mat4 lightViewProjection{};
  int lightIndex = -1; // index into SceneLightData::spotLights, -1 = unused
  float farPlane = 0.0F;
};

/// All spot shadow maps plus their allocation state. Slots are tiles of
/// one depth atlas (tile s = slot s) sampled through a single register.
struct SpotShadowState final {
  SpotShadowData slots[kMaxSpotShadowLights]{};
  DeviceTextureHandle depthAtlasTexture{};
  RenderTargetHandle atlasTarget{};
  bool initialized = false;
};

/// Compute perspective view-projection for a spot light shadow map.
math::Mat4 compute_spot_shadow_matrix(const math::Vec3 &position,
                                      const math::Vec3 &direction,
                                      float outerConeAngle,
                                      float radius) noexcept;

/// Initialize spot light shadow map GPU resources.
bool initialize_spot_shadow_maps(SpotShadowState &state) noexcept;

/// Destroy spot light shadow map GPU resources.
void shutdown_spot_shadow_maps(SpotShadowState &state) noexcept;

// ---- Point Light Cubemap Shadow Maps ----

inline constexpr std::size_t kMaxPointShadowLights = 4U;
inline constexpr int kPointShadowMapResolution = 1024;

/// One point light's cube shadow map: per-face depth render targets over
/// one cubemap texture, plus the far plane.
struct PointShadowData final {
  math::Mat4 faceViewProjections[6]{};
  DeviceTextureHandle depthCubemap{};
  RenderTargetHandle faceTargets[6]{};
  int lightIndex = -1; // index into SceneLightData::pointLights, -1 = unused
  float farPlane = 0.0F;
};

/// All point shadow cubemaps plus their allocation state.
struct PointShadowState final {
  PointShadowData slots[kMaxPointShadowLights]{};
  bool initialized = false;
};

/// Compute 6 face view-projection matrices for a point light cubemap shadow.
void compute_point_shadow_matrices(const math::Vec3 &position, float radius,
                                   math::Mat4 outVP[6]) noexcept;

/// Initialize point light cubemap shadow GPU resources.
bool initialize_point_shadow_maps(PointShadowState &state) noexcept;

/// Destroy point light cubemap shadow GPU resources.
void shutdown_point_shadow_maps(PointShadowState &state) noexcept;

} // namespace engine::renderer
