#include "engine/renderer/shadow_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "engine/core/logging.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

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

/// Extract 8 world-space frustum corners from inverse view-projection.
void extract_frustum_corners(const math::Mat4 &invViewProj,
                             math::Vec3 outCorners[8]) noexcept {
  // NDC corners: near z=-1, far z=+1 in OpenGL convention.
  constexpr float ndcCorners[8][3] = {
      {-1.0F, -1.0F, -1.0F}, {1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, -1.0F},
      {-1.0F, 1.0F, -1.0F},  {-1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, 1.0F},
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

math::Mat4 compute_cascade_matrix(const math::Mat4 &viewMatrix,
                                  const math::Mat4 &projMatrix,
                                  const math::Vec3 &lightDir, float cascadeNear,
                                  float cascadeFar,
                                  float /*texelSize*/) noexcept {
  // Build a sub-frustum projection that covers [cascadeNear, cascadeFar].
  // We modify the projection matrix to clip to the cascade range by
  // interpolating the frustum corners between near and far.
  const math::Mat4 viewProj = math::mul(projMatrix, viewMatrix);
  math::Mat4 invViewProj{};
  if (!math::inverse(viewProj, &invViewProj)) {
    return math::Mat4{};
  }

  // Get full frustum corners and interpolate to cascade range.
  math::Vec3 fullCorners[8]{};
  extract_frustum_corners(invViewProj, fullCorners);

  // Compute cascade sub-frustum corners by lerp between near/far planes.
  // Near plane corners: fullCorners[0..3], Far plane: fullCorners[4..7].
  // We need to compute sub-frustum near/far as ratio of full near/far.

  // Extract camera near/far from the projection matrix.
  // For perspective: P[2][2] = -(f+n)/(f-n), P[3][2] = -2fn/(f-n)
  // nearClip, farClip are already provided as parameters.
  // Use the frustum corner interpolation approach.

  // Inverse of full projection gives us the full frustum at NDC z=-1 and z=1.
  // We want sub-frustum at cascadeNear/cascadeFar. So we compute the ratio.
  const float projNear =
      projMatrix.columns[3].z / (projMatrix.columns[2].z - 1.0F);
  const float projFar =
      projMatrix.columns[3].z / (projMatrix.columns[2].z + 1.0F);

  const float nearRatio = (std::abs(projFar - projNear) > 1e-7F)
                              ? (cascadeNear - projNear) / (projFar - projNear)
                              : 0.0F;
  const float farRatio = (std::abs(projFar - projNear) > 1e-7F)
                             ? (cascadeFar - projNear) / (projFar - projNear)
                             : 1.0F;

  math::Vec3 cascadeCorners[8]{};
  for (int i = 0; i < 4; ++i) {
    // Near plane corners: lerp between full near and full far.
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

  // Compute center of the sub-frustum.
  math::Vec3 center(0.0F, 0.0F, 0.0F);
  for (int i = 0; i < 8; ++i) {
    center.x += cascadeCorners[i].x;
    center.y += cascadeCorners[i].y;
    center.z += cascadeCorners[i].z;
  }
  center.x /= 8.0F;
  center.y /= 8.0F;
  center.z /= 8.0F;

  // Build a look-at matrix from the light's perspective.
  const math::Vec3 lightPos(center.x - lightDir.x * 50.0F,
                            center.y - lightDir.y * 50.0F,
                            center.z - lightDir.z * 50.0F);
  const math::Mat4 lightView =
      math::look_at(lightPos, center, math::Vec3(0.0F, 1.0F, 0.0F));

  // Find AABB of cascade corners in light space.
  float minX = 1e30F, maxX = -1e30F;
  float minY = 1e30F, maxY = -1e30F;
  float minZ = 1e30F, maxZ = -1e30F;

  for (int i = 0; i < 8; ++i) {
    math::Vec4 lsCorner = math::mul(
        lightView, math::Vec4(cascadeCorners[i].x, cascadeCorners[i].y,
                              cascadeCorners[i].z, 1.0F));
    minX = std::min(minX, lsCorner.x);
    maxX = std::max(maxX, lsCorner.x);
    minY = std::min(minY, lsCorner.y);
    maxY = std::max(maxY, lsCorner.y);
    minZ = std::min(minZ, lsCorner.z);
    maxZ = std::max(maxZ, lsCorner.z);
  }

  // Extend the near plane to catch shadow casters behind the frustum.
  constexpr float kShadowNearExtend = 50.0F;
  minZ -= kShadowNearExtend;

  const math::Mat4 lightProj = math::ortho(minX, maxX, minY, maxY, minZ, maxZ);
  return math::mul(lightProj, lightView);
}

math::Mat4 snap_to_texel(const math::Mat4 &lightViewProj,
                         int shadowMapSize) noexcept {
  const float texelWorld = 2.0F / static_cast<float>(shadowMapSize);

  // Snap the x/y offset to texel boundaries.
  math::Mat4 result = lightViewProj;
  result.columns[3].x =
      std::floor(result.columns[3].x / texelWorld) * texelWorld;
  result.columns[3].y =
      std::floor(result.columns[3].y / texelWorld) * texelWorld;
  return result;
}

bool initialize_shadow_maps(ShadowMapState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return false;
  }

  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    state.depthTextures[i] =
        dev->create_depth_texture(kShadowMapResolution, kShadowMapResolution);
    if (state.depthTextures[i] == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create shadow cascade depth texture");
      shutdown_shadow_maps(state);
      return false;
    }

    // Depth-only FBO (no color attachment → pass 0 for color).
    state.depthFbos[i] = dev->create_framebuffer(0U, state.depthTextures[i]);
    if (state.depthFbos[i] == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create shadow cascade FBO");
      shutdown_shadow_maps(state);
      return false;
    }
  }

  state.initialized = true;
  return true;
}

void shutdown_shadow_maps(ShadowMapState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    if (state.depthFbos[i] != 0U) {
      dev->destroy_framebuffer(state.depthFbos[i]);
      state.depthFbos[i] = 0U;
    }
    if (state.depthTextures[i] != 0U) {
      dev->destroy_texture(state.depthTextures[i]);
      state.depthTextures[i] = 0U;
    }
  }

  state.initialized = false;
}

// ---- Spot Light Shadow Maps ----

math::Mat4 compute_spot_shadow_matrix(const math::Vec3 &position,
                                      const math::Vec3 &direction,
                                      float outerConeAngle,
                                      float radius) noexcept {
  // Build light view matrix.
  const math::Vec3 target(position.x + direction.x, position.y + direction.y,
                          position.z + direction.z);

  // Choose an up vector that is not collinear with direction.
  math::Vec3 up(0.0F, 1.0F, 0.0F);
  if (std::abs(direction.y) > 0.99F) {
    up = math::Vec3(1.0F, 0.0F, 0.0F);
  }

  const math::Mat4 lightView = math::look_at(position, target, up);

  // FOV slightly wider than the outer cone to avoid edge clipping.
  const float fov = outerConeAngle * 2.0F + 0.05F;
  constexpr float kNearPlane = 0.1F;
  const float farPlane = std::max(radius, 1.0F);

  const math::Mat4 lightProj = math::perspective(fov, 1.0F, kNearPlane, farPlane);
  return math::mul(lightProj, lightView);
}

bool initialize_spot_shadow_maps(SpotShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return false;
  }

  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    state.slots[i].depthTexture = dev->create_depth_texture(
        kSpotShadowMapResolution, kSpotShadowMapResolution);
    if (state.slots[i].depthTexture == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create spot shadow depth texture");
      shutdown_spot_shadow_maps(state);
      return false;
    }

    state.slots[i].depthFbo =
        dev->create_framebuffer(0U, state.slots[i].depthTexture);
    if (state.slots[i].depthFbo == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create spot shadow FBO");
      shutdown_spot_shadow_maps(state);
      return false;
    }
  }

  state.initialized = true;
  return true;
}

void shutdown_spot_shadow_maps(SpotShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    if (state.slots[i].depthFbo != 0U) {
      dev->destroy_framebuffer(state.slots[i].depthFbo);
      state.slots[i].depthFbo = 0U;
    }
    if (state.slots[i].depthTexture != 0U) {
      dev->destroy_texture(state.slots[i].depthTexture);
      state.slots[i].depthTexture = 0U;
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
  constexpr float kFov = 3.14159265F / 2.0F; // 90 degrees

  const math::Mat4 proj = math::perspective(kFov, 1.0F, kNearPlane, farPlane);

  // Six face directions: +X, -X, +Y, -Y, +Z, -Z.
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

bool initialize_point_shadow_maps(PointShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_depth_cubemap == nullptr)) {
    return false;
  }

  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    state.slots[i].depthCubemap =
        dev->create_depth_cubemap(kPointShadowMapResolution);
    if (state.slots[i].depthCubemap == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create point shadow cubemap");
      shutdown_point_shadow_maps(state);
      return false;
    }

    // Single FBO per slot — face is re-attached each frame.
    state.slots[i].depthFbo = dev->create_framebuffer(0U, 0U);
    if (state.slots[i].depthFbo == 0U) {
      core::log_message(core::LogLevel::Error, "shadow_map",
                        "failed to create point shadow FBO");
      shutdown_point_shadow_maps(state);
      return false;
    }
  }

  state.initialized = true;
  return true;
}

void shutdown_point_shadow_maps(PointShadowState &state) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    if (state.slots[i].depthFbo != 0U) {
      dev->destroy_framebuffer(state.slots[i].depthFbo);
      state.slots[i].depthFbo = 0U;
    }
    if (state.slots[i].depthCubemap != 0U) {
      dev->destroy_texture(state.slots[i].depthCubemap);
      state.slots[i].depthCubemap = 0U;
    }
    state.slots[i].lightIndex = -1;
  }

  state.initialized = false;
}

} // namespace engine::renderer

