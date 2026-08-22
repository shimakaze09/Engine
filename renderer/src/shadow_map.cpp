// Implements shadow map behavior for the Engine renderer system.

#include "engine/renderer/shadow_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

/// Square Depth24 shadow texture. Point sampling: the shaders take their
/// own PCF taps and compare depths explicitly, and WebGL2 treats
/// linear-filtered depth textures as incomplete (all-zero samples, #293).
DeviceTextureHandle create_shadow_depth_texture(const RenderDevice *dev,
                                                int resolution) noexcept {
  if ((dev == nullptr) || (dev->create_texture == nullptr)) {
    return kInvalidDeviceTexture;
  }
  TextureDesc desc{};
  desc.kind = TextureKind::Tex2D;
  desc.format = TextureFormat::Depth24;
  desc.width = resolution;
  desc.height = resolution;
  desc.filter = TextureFilter::Nearest;
  // ClampEdge: border PCF taps must not wrap to the map's opposite
  // edge — every other depth target already clamps.
  desc.wrap = TextureWrap::ClampEdge;
  return dev->create_texture(desc);
}

/// Depth-only render target over one shadow depth texture.
RenderTargetHandle create_depth_only_target(const RenderDevice *dev,
                                            DeviceTextureHandle depth) noexcept {
  if ((dev == nullptr) || (dev->create_render_target == nullptr)) {
    return RenderTargetHandle{};
  }
  RenderTargetDesc desc{};
  desc.depth.texture = depth;
  return dev->create_render_target(desc);
}

} // namespace

int shadow_cascade_resolution(std::size_t cascadeIndex) noexcept {
  if (cascadeIndex >= kShadowCascadeCount) {
    return kShadowCascadeResolutions[kShadowCascadeCount - 1U];
  }
  return kShadowCascadeResolutions[cascadeIndex];
}

CascadeSplits compute_cascade_splits(float nearClip, float farClip,
                                     float lambda) noexcept {
  CascadeSplits splits{};
  splits.distances[0] = nearClip;
  splits.distances[kShadowCascadeCount] = farClip;

  for (std::size_t i = 1U; i < kShadowCascadeCount; ++i) {
    const float p =
        static_cast<float>(i) / static_cast<float>(kShadowCascadeCount);
    const float logSplit = nearClip * std::pow(farClip / nearClip, p);
    const float uniformSplit = nearClip + (farClip - nearClip) * p;
    splits.distances[i] = lambda * logSplit + (1.0F - lambda) * uniformSplit;
  }

  return splits;
}

namespace {

constexpr float kShadowEpsilon = 1.0e-6F;

float snap_to_grid(float value, float step) noexcept {
  if (step <= kShadowEpsilon) {
    return value;
  }
  return std::floor((value / step) + 0.5F) * step;
}

math::Vec3 choose_light_up(const math::Vec3 &lightDir) noexcept {
  return (std::abs(lightDir.y) > 0.99F) ? math::Vec3(1.0F, 0.0F, 0.0F)
                                        : math::Vec3(0.0F, 1.0F, 0.0F);
}

/// Extract 8 world-space frustum corners from inverse view-projection.
void extract_frustum_corners(const math::Mat4 &invViewProj,
                             math::Vec3 outCorners[8]) noexcept {
  // Near-plane NDC z follows the device convention: -1 on GL, 0 on
  // zero-to-one APIs — unprojecting -1 there lands inside the frustum
  // (half the near distance) and over-extends every cascade's fit.
  const float nearZ = device_depth_zero_one() ? 0.0F : -1.0F;
  const float ndcCorners[8][3] = {
      {-1.0F, -1.0F, nearZ}, {1.0F, -1.0F, nearZ}, {1.0F, 1.0F, nearZ},
      {-1.0F, 1.0F, nearZ},  {-1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, 1.0F},
      {1.0F, 1.0F, 1.0F},    {-1.0F, 1.0F, 1.0F},
  };

  for (int i = 0; i < 8; ++i) {
    math::Vec4 clip(ndcCorners[i][0], ndcCorners[i][1], ndcCorners[i][2], 1.0F);
    math::Vec4 world = math::mul(invViewProj, clip);
    if (std::abs(world.w) > 1e-7F) {
      world.x /= world.w;
      world.y /= world.w;
      world.z /= world.w;
    }
    outCorners[i] = math::Vec3(world.x, world.y, world.z);
  }
}

} // namespace

/// Builds the light-space matrix for one cascade: the camera frustum's
/// corners are interpolated to the [cascadeNear, cascadeFar] range (near
/// and far recovered from the projection matrix), a bounding sphere fixes
/// the X/Y extent for stable texel density, and the light-space center is
/// snapped to texel steps so sub-texel camera motion does not move the
/// shadow projection.
math::Mat4 compute_cascade_matrix(const math::Mat4 &viewMatrix,
                                  const math::Mat4 &projMatrix,
                                  float projNear, float projFar,
                                  const math::Vec3 &lightDir, float cascadeNear,
                                  float cascadeFar,
                                  int shadowMapSize) noexcept {
  const math::Mat4 viewProj = math::mul(projMatrix, viewMatrix);
  math::Mat4 invViewProj{};
  if (!math::inverse(viewProj, &invViewProj)) {
    return math::Mat4{};
  }

  math::Vec3 fullCorners[8]{};
  extract_frustum_corners(invViewProj, fullCorners);

  const float nearRatio = (std::abs(projFar - projNear) > 1e-7F)
                              ? (cascadeNear - projNear) / (projFar - projNear)
                              : 0.0F;
  const float farRatio = (std::abs(projFar - projNear) > 1e-7F)
                             ? (cascadeFar - projNear) / (projFar - projNear)
                             : 1.0F;

  math::Vec3 cascadeCorners[8]{};
  for (int i = 0; i < 4; ++i) {
    const math::Vec3 &nrCorner = fullCorners[i];
    const math::Vec3 &frCorner = fullCorners[i + 4];
    cascadeCorners[i] =
        math::Vec3(nrCorner.x + (frCorner.x - nrCorner.x) * nearRatio,
                   nrCorner.y + (frCorner.y - nrCorner.y) * nearRatio,
                   nrCorner.z + (frCorner.z - nrCorner.z) * nearRatio);
    cascadeCorners[i + 4] =
        math::Vec3(nrCorner.x + (frCorner.x - nrCorner.x) * farRatio,
                   nrCorner.y + (frCorner.y - nrCorner.y) * farRatio,
                   nrCorner.z + (frCorner.z - nrCorner.z) * farRatio);
  }

  math::Vec3 center(0.0F, 0.0F, 0.0F);
  for (int i = 0; i < 8; ++i) {
    center.x += cascadeCorners[i].x;
    center.y += cascadeCorners[i].y;
    center.z += cascadeCorners[i].z;
  }
  center.x /= 8.0F;
  center.y /= 8.0F;
  center.z /= 8.0F;

  float radius = 0.0F;
  for (int i = 0; i < 8; ++i) {
    radius = std::max(radius, math::distance(center, cascadeCorners[i]));
  }
  if (radius <= kShadowEpsilon) {
    return math::Mat4{};
  }

  const math::Vec3 stableLightDir = math::normalize(lightDir);
  if (math::length_sq(stableLightDir) <= kShadowEpsilon) {
    return math::Mat4{};
  }

  const math::Vec3 lightUp = choose_light_up(stableLightDir);
  const math::Mat4 baseLightView =
      math::look_at(math::mul(stableLightDir, -50.0F), math::Vec3(), lightUp);
  math::Mat4 invBaseLightView{};
  if (!math::inverse(baseLightView, &invBaseLightView)) {
    return math::Mat4{};
  }

  const int safeShadowMapSize =
      (shadowMapSize > 0) ? shadowMapSize : kShadowMapResolution;
  const float snapStep =
      (2.0F * radius) / static_cast<float>(safeShadowMapSize);

  const math::Vec4 centerLs =
      math::mul(baseLightView, math::Vec4(center.x, center.y, center.z, 1.0F));
  const math::Vec4 snappedCenterLs(
      snap_to_grid(centerLs.x, snapStep), snap_to_grid(centerLs.y, snapStep),
      centerLs.z, 1.0F);
  const math::Vec4 snappedCenterWorld =
      math::mul(invBaseLightView, snappedCenterLs);
  const math::Vec3 snappedCenter(snappedCenterWorld.x, snappedCenterWorld.y,
                                 snappedCenterWorld.z);

  const math::Vec3 lightPos =
      math::sub(snappedCenter, math::mul(stableLightDir, 50.0F));
  const math::Mat4 lightView =
      math::look_at(lightPos, snappedCenter, lightUp);

  float minZ = 1e30F, maxZ = -1e30F;

  for (int i = 0; i < 8; ++i) {
    math::Vec4 lsCorner = math::mul(
        lightView, math::Vec4(cascadeCorners[i].x, cascadeCorners[i].y,
                              cascadeCorners[i].z, 1.0F));
    minZ = std::min(minZ, lsCorner.z);
    maxZ = std::max(maxZ, lsCorner.z);
  }

  // Extend the near plane to catch shadow casters behind the frustum.
  constexpr float kShadowNearExtend = 50.0F;

  const float minX = -radius;
  const float maxX = radius;
  const float minY = -radius;
  const float maxY = radius;
  // ortho() follows the glOrtho convention: view-space z = -nearZ maps to the
  // near plane. Light-space corners sit at negative z, so near/far are the
  // negated max/min corner depths — passing minZ/maxZ raw puts the whole
  // cascade outside the clip volume.
  const math::Mat4 lightProj =
      device_depth_zero_one()
          ? math::ortho_zero_one(minX, maxX, minY, maxY,
                                 -maxZ - kShadowNearExtend, -minZ)
          : math::ortho(minX, maxX, minY, maxY, -maxZ - kShadowNearExtend,
                        -minZ);
  return math::mul(lightProj, lightView);
}

/// Snaps the projection's x/y translation to shadow-texel boundaries.
math::Mat4 snap_to_texel(const math::Mat4 &lightViewProj,
                         int shadowMapSize) noexcept {
  const int safeShadowMapSize =
      (shadowMapSize > 0) ? shadowMapSize : kShadowMapResolution;
  const float texelWorld = 2.0F / static_cast<float>(safeShadowMapSize);

  math::Mat4 result = lightViewProj;
  result.columns[3].x =
      std::floor(result.columns[3].x / texelWorld) * texelWorld;
  result.columns[3].y =
      std::floor(result.columns[3].y / texelWorld) * texelWorld;
  return result;
}

/// Initializes the owning system for shadow maps.
bool initialize_shadow_maps(ShadowMapState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return false;
  }

  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    const int cascadeResolution = shadow_cascade_resolution(i);
    state.resolutions[i] = cascadeResolution;
    state.depthTextures[i] =
        create_shadow_depth_texture(dev, cascadeResolution);
    if (state.depthTextures[i] == kInvalidDeviceTexture) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create shadow cascade depth texture");
      shutdown_shadow_maps(state);
      return false;
    }

    state.depthTargets[i] =
        create_depth_only_target(dev, state.depthTextures[i]);
    if (state.depthTargets[i].value == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create shadow cascade render target");
      shutdown_shadow_maps(state);
      return false;
    }
  }

  state.initialized = true;
  return true;
}

/// Shuts down the owning system for shadow maps.
void shutdown_shadow_maps(ShadowMapState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    if (state.depthTargets[i].value != 0U) {
      dev->destroy_render_target(state.depthTargets[i]);
      state.depthTargets[i] = RenderTargetHandle{};
    }
    if (state.depthTextures[i] != kInvalidDeviceTexture) {
      dev->destroy_texture(state.depthTextures[i]);
      state.depthTextures[i] = kInvalidDeviceTexture;
    }
    state.resolutions[i] = 0;
  }

  state.initialized = false;
}

// ---- Spot Light Shadow Maps ----

/// Perspective light matrix for a spot: the FOV is slightly wider than
/// the outer cone to avoid edge clipping.
math::Mat4 compute_spot_shadow_matrix(const math::Vec3 &position,
                                      const math::Vec3 &direction,
                                      float outerConeAngle,
                                      float radius) noexcept {
  const math::Vec3 target(position.x + direction.x, position.y + direction.y,
                          position.z + direction.z);

  math::Vec3 up(0.0F, 1.0F, 0.0F);
  if (std::abs(direction.y) > 0.99F) {
    up = math::Vec3(1.0F, 0.0F, 0.0F);
  }

  const math::Mat4 lightView = math::look_at(position, target, up);

  const float fov = outerConeAngle * 2.0F + 0.05F;
  constexpr float kNearPlane = 0.1F;
  const float farPlane = std::max(radius, 1.0F);

  const math::Mat4 lightProj =
      device_depth_zero_one()
          ? math::perspective_zero_one(fov, 1.0F, kNearPlane, farPlane)
          : math::perspective(fov, 1.0F, kNearPlane, farPlane);
  return math::mul(lightProj, lightView);
}

/// Initializes the owning system for spot shadow maps.
bool initialize_spot_shadow_maps(SpotShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return false;
  }

  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    state.slots[i].depthTexture =
        create_shadow_depth_texture(dev, kSpotShadowMapResolution);
    if (state.slots[i].depthTexture == kInvalidDeviceTexture) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create spot shadow depth texture");
      shutdown_spot_shadow_maps(state);
      return false;
    }

    state.slots[i].depthTarget =
        create_depth_only_target(dev, state.slots[i].depthTexture);
    if (state.slots[i].depthTarget.value == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create spot shadow render target");
      shutdown_spot_shadow_maps(state);
      return false;
    }
  }

  state.initialized = true;
  return true;
}

/// Shuts down the owning system for spot shadow maps.
void shutdown_spot_shadow_maps(SpotShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    if (state.slots[i].depthTarget.value != 0U) {
      dev->destroy_render_target(state.slots[i].depthTarget);
      state.slots[i].depthTarget = RenderTargetHandle{};
    }
    if (state.slots[i].depthTexture != kInvalidDeviceTexture) {
      dev->destroy_texture(state.slots[i].depthTexture);
      state.slots[i].depthTexture = kInvalidDeviceTexture;
    }
    state.slots[i].lightIndex = -1;
  }

  state.initialized = false;
}

// ---- Point Light Cubemap Shadow Maps ----

void compute_point_shadow_matrices(const math::Vec3 &position, float radius,
                                   math::Mat4 outVP[6]) noexcept {
  constexpr float kNearPlane = 0.1F;
  const float farPlane = std::max(radius, 1.0F);
  constexpr float kFov = 3.14159265F / 2.0F;

  const math::Mat4 proj =
      device_depth_zero_one()
          ? math::perspective_zero_one(kFov, 1.0F, kNearPlane, farPlane)
          : math::perspective(kFov, 1.0F, kNearPlane, farPlane);

  // Face order and up vectors follow the GL cubemap convention
  // (GL_TEXTURE_CUBE_MAP_POSITIVE_X + i): +X, -X, +Y, -Y, +Z, -Z.
  struct FaceDir {
    math::Vec3 target;
    math::Vec3 up;
  };
  const FaceDir faces[6] = {
      {{position.x + 1, position.y, position.z}, {0, -1, 0}},  // +X
      {{position.x - 1, position.y, position.z}, {0, -1, 0}},  // -X
      {{position.x, position.y + 1, position.z}, {0, 0, 1}},   // +Y
      {{position.x, position.y - 1, position.z}, {0, 0, -1}},  // -Y
      {{position.x, position.y, position.z + 1}, {0, -1, 0}},  // +Z
      {{position.x, position.y, position.z - 1}, {0, -1, 0}},  // -Z
  };

  for (int i = 0; i < 6; ++i) {
    const math::Mat4 view =
        math::look_at(position, faces[i].target, faces[i].up);
    outVP[i] = math::mul(proj, view);
  }
}

/// Initializes the owning system for point shadow maps.
bool initialize_point_shadow_maps(PointShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_texture == nullptr) ||
      (dev->create_render_target == nullptr)) {
    return false;
  }

  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    TextureDesc cubeDesc{};
    cubeDesc.kind = TextureKind::Cube;
    cubeDesc.format = TextureFormat::Depth24;
    cubeDesc.width = kPointShadowMapResolution;
    cubeDesc.filter = TextureFilter::Nearest;
    cubeDesc.wrap = TextureWrap::ClampEdge;
    state.slots[i].depthCubemap = dev->create_texture(cubeDesc);
    if (state.slots[i].depthCubemap == kInvalidDeviceTexture) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create point shadow cubemap");
      shutdown_point_shadow_maps(state);
      return false;
    }

    // Render targets are immutable, so each cube face gets its own
    // depth-only target over the shared cubemap.
    for (int face = 0; face < 6; ++face) {
      RenderTargetDesc faceDesc{};
      faceDesc.depth.texture = state.slots[i].depthCubemap;
      faceDesc.depth.face = static_cast<CubeFace>(face);
      state.slots[i].faceTargets[face] = dev->create_render_target(faceDesc);
      if (state.slots[i].faceTargets[face].value == 0U) {
        core::log_message(core::LogLevel::Error, "shadow_map",
                          "failed to create point shadow face target");
        shutdown_point_shadow_maps(state);
        return false;
      }
    }
  }

  state.initialized = true;
  return true;
}

/// Shuts down the owning system for point shadow maps.
void shutdown_point_shadow_maps(PointShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    for (int face = 0; face < 6; ++face) {
      if (state.slots[i].faceTargets[face].value != 0U) {
        dev->destroy_render_target(state.slots[i].faceTargets[face]);
        state.slots[i].faceTargets[face] = RenderTargetHandle{};
      }
    }
    if (state.slots[i].depthCubemap != kInvalidDeviceTexture) {
      dev->destroy_texture(state.slots[i].depthCubemap);
      state.slots[i].depthCubemap = kInvalidDeviceTexture;
    }
    state.slots[i].lightIndex = -1;
  }

  state.initialized = false;
}

} // namespace engine::renderer
