// Implements the shared uniform-upload helpers the geometry passes bind:
// forward-PBR IBL/lighting/fog/foliage/shadow state, their deferred and
// G-Buffer counterparts, and per-instance attribute uploads.
#include "engine/renderer/command_buffer.h"

#include "command_buffer_capture.h"
#include "command_buffer_context.h"
#include "command_buffer_ibl.h"
#include "command_buffer_math.h"
#include "command_buffer_post_resources.h"
#include "command_buffer_sky.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/debug_draw.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/post_process_stack.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/texture_loader.h"
#include "command_buffer_flush_internal.h"

namespace engine::renderer {

namespace {
constexpr std::size_t kInstanceModelColumns = 4U;
} // namespace

/// Uploads the environment IBL uniforms for the forward PBR program and
/// binds its textures when enabled; every pbrProgram pass must call this so
/// stale program state never leaks between passes. The sampler units are
/// assigned even when IBL is off: a samplerCube uniform left at its default
/// unit 0 aliases the sampler2D albedo there, which is a draw-time
/// GL_INVALID_OPERATION that corrupts every draw.
void apply_pbr_ibl_uniforms(const BackendState &backend,
                            const RenderDevice *dev,
                            bool iblAvailable) noexcept {
  if (backend.pbrIrradianceMapLoc.valid()) {
    dev->set_param_i32(backend.pbrIrradianceMapLoc, kIblIrradianceUnit);
  }
  if (backend.pbrPrefilteredMapLoc.valid()) {
    dev->set_param_i32(backend.pbrPrefilteredMapLoc, kIblPrefilteredUnit);
  }
  if (backend.pbrBrdfLutLoc.valid()) {
    dev->set_param_i32(backend.pbrBrdfLutLoc, kIblBrdfLutUnit);
  }

  const bool enabled = iblAvailable && (backend.pbrIblEnabledLoc.valid()) &&
                       (dev->bind_texture_slot != nullptr);
  if (backend.pbrIblEnabledLoc.valid()) {
    dev->set_param_i32(backend.pbrIblEnabledLoc, enabled ? 1 : 0);
  }
  if (!enabled) {
    if (dev->bind_texture_slot != nullptr) {
      // Vulkan-family backends need valid descriptors on the declared
      // IBL samplers even when the ambient path is constant.
      dev->bind_texture_slot(kIblIrradianceUnit, backend.fallbackCubemap);
      dev->bind_texture_slot(kIblPrefilteredUnit, backend.fallbackCubemap);
      dev->bind_texture_slot(kIblBrdfLutUnit, backend.fallbackTexture2D);
    }
    return;
  }

  dev->bind_texture_slot(kIblIrradianceUnit,
                            backend.irradianceEnvironmentTexture);
  dev->bind_texture_slot(kIblPrefilteredUnit,
                            backend.prefilteredEnvironmentTexture);
  dev->bind_texture_slot(kIblBrdfLutUnit, backend.brdfLutTexture);
  if (backend.pbrPrefilteredMipsLoc.valid()) {
    dev->set_param_f32(
        backend.pbrPrefilteredMipsLoc,
        static_cast<float>(backend.prefilteredEnvironmentMipLevels));
  }
}

namespace {

/// Which scene lights fill the forward program's fixed arrays: the
/// kForwardMax* nearest the active camera, ties broken by index, so the
/// lit set is a function of the scene rather than of creation order.
/// Entries are indices into the scene arrays, nearest first.
struct ForwardLightSelection final {
  std::array<std::uint32_t, kForwardMaxPointLights> point{};
  std::size_t pointCount = 0U;
  std::array<std::uint32_t, kForwardMaxSpotLights> spot{};
  std::size_t spotCount = 0U;
};

template <typename Light>
std::size_t select_nearest(const Light *lights, std::size_t count,
                           std::size_t limit, const math::Vec3 &eye,
                           std::uint32_t *out) noexcept {
  std::array<std::uint32_t, kMaxPointLights> order{};
  const std::size_t total = std::min(count, order.size());
  for (std::size_t i = 0U; i < total; ++i) {
    order[i] = static_cast<std::uint32_t>(i);
  }
  const auto distSq = [&](std::uint32_t index) noexcept {
    const math::Vec3 &p = lights[index].position;
    const float dx = p.x - eye.x;
    const float dy = p.y - eye.y;
    const float dz = p.z - eye.z;
    return (dx * dx) + (dy * dy) + (dz * dz);
  };
  const std::size_t selected = std::min(total, limit);
  std::partial_sort(order.data(), order.data() + selected,
                    order.data() + total,
                    [&](std::uint32_t a, std::uint32_t b) noexcept {
                      const float da = distSq(a);
                      const float db = distSq(b);
                      return (da < db) || ((da == db) && (a < b));
                    });
  for (std::size_t i = 0U; i < selected; ++i) {
    out[i] = order[i];
  }
  return selected;
}

ForwardLightSelection select_forward_lights(const SceneLightData &lights) noexcept {
  ForwardLightSelection selection{};
  const math::Vec3 eye = renderer_context().activeCamera.position;
  selection.pointCount = select_nearest(
      lights.pointLights.data(), std::min(lights.pointLightCount, kMaxPointLights),
      kForwardMaxPointLights, eye, selection.point.data());
  selection.spotCount = select_nearest(
      lights.spotLights.data(), std::min(lights.spotLightCount, kMaxSpotLights),
      kForwardMaxSpotLights, eye, selection.spot.data());
  return selection;
}

/// The forward shader matches a shadow slot against its loop position
/// over the uploaded lights, so a slot's scene index maps to that
/// position, or -1 when its light was not among the nearest.
float forward_slot_index(const std::uint32_t *selected, std::size_t count,
                         int lightIndex) noexcept {
  if (lightIndex < 0) {
    return -1.0F;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    if (selected[i] == static_cast<std::uint32_t>(lightIndex)) {
      return static_cast<float>(i);
    }
  }
  return -1.0F;
}

} // namespace

void upload_pbr_lighting_uniforms(const BackendState &backend,
                                  const RenderDevice *dev,
                                  const SceneLightData &lights) noexcept {
  // Flat array vocabulary: pack per-light vec4 elements into fixed
  // scratch and upload each array in one set_param_vec4_array call.
  if (dev->set_param_vec4_array == nullptr) {
    return;
  }

  const std::size_t dirCount =
      std::min(lights.directionalLightCount, kMaxDirectionalLights);
  if (backend.pbrDirLightCountLocation.valid()) {
    dev->set_param_i32(backend.pbrDirLightCountLocation,
                         static_cast<std::int32_t>(dirCount));
  }
  if (dirCount > 0U) {
    float direction[kMaxDirectionalLights * 4U] = {};
    float colorIntensity[kMaxDirectionalLights * 4U] = {};
    for (std::size_t i = 0U; i < dirCount; ++i) {
      const auto &dl = lights.directionalLights[i];
      direction[i * 4U + 0U] = dl.direction.x;
      direction[i * 4U + 1U] = dl.direction.y;
      direction[i * 4U + 2U] = dl.direction.z;
      colorIntensity[i * 4U + 0U] = dl.color.x;
      colorIntensity[i * 4U + 1U] = dl.color.y;
      colorIntensity[i * 4U + 2U] = dl.color.z;
      colorIntensity[i * 4U + 3U] = dl.intensity;
    }
    dev->set_param_vec4_array(backend.pbrDirLightDirectionParam, direction,
                              static_cast<std::int32_t>(dirCount));
    dev->set_param_vec4_array(backend.pbrDirLightColorParam, colorIntensity,
                              static_cast<std::int32_t>(dirCount));
  }

  const ForwardLightSelection selection = select_forward_lights(lights);
  const std::size_t pointCount = selection.pointCount;
  if (backend.pbrPointLightCountLocation.valid()) {
    dev->set_param_i32(backend.pbrPointLightCountLocation,
                         static_cast<std::int32_t>(pointCount));
  }
  if (pointCount > 0U) {
    float posRadius[kForwardMaxPointLights * 4U] = {};
    float colorIntensity[kForwardMaxPointLights * 4U] = {};
    for (std::size_t i = 0U; i < pointCount; ++i) {
      const auto &pl = lights.pointLights[selection.point[i]];
      posRadius[i * 4U + 0U] = pl.position.x;
      posRadius[i * 4U + 1U] = pl.position.y;
      posRadius[i * 4U + 2U] = pl.position.z;
      posRadius[i * 4U + 3U] = pl.radius;
      colorIntensity[i * 4U + 0U] = pl.color.x;
      colorIntensity[i * 4U + 1U] = pl.color.y;
      colorIntensity[i * 4U + 2U] = pl.color.z;
      colorIntensity[i * 4U + 3U] = pl.intensity;
    }
    dev->set_param_vec4_array(backend.pbrPointLightPosRadiusParam, posRadius,
                              static_cast<std::int32_t>(pointCount));
    dev->set_param_vec4_array(backend.pbrPointLightColorParam,
                              colorIntensity,
                              static_cast<std::int32_t>(pointCount));
  }

  const std::size_t spotCount = selection.spotCount;
  if (backend.pbrSpotLightCountLocation.valid()) {
    dev->set_param_i32(backend.pbrSpotLightCountLocation,
                         static_cast<std::int32_t>(spotCount));
  }
  if (spotCount > 0U) {
    float posRadius[kForwardMaxSpotLights * 4U] = {};
    float dirInner[kForwardMaxSpotLights * 4U] = {};
    float colorIntensity[kForwardMaxSpotLights * 4U] = {};
    float params[kForwardMaxSpotLights * 4U] = {};
    for (std::size_t i = 0U; i < spotCount; ++i) {
      const auto &sl = lights.spotLights[selection.spot[i]];
      posRadius[i * 4U + 0U] = sl.position.x;
      posRadius[i * 4U + 1U] = sl.position.y;
      posRadius[i * 4U + 2U] = sl.position.z;
      posRadius[i * 4U + 3U] = sl.radius;
      dirInner[i * 4U + 0U] = sl.direction.x;
      dirInner[i * 4U + 1U] = sl.direction.y;
      dirInner[i * 4U + 2U] = sl.direction.z;
      // Shaders compare cone terms against dot(L, -spotDir), a cosine —
      // upload cosines, not the stored radian angles.
      dirInner[i * 4U + 3U] = std::cos(sl.innerConeAngle);
      colorIntensity[i * 4U + 0U] = sl.color.x;
      colorIntensity[i * 4U + 1U] = sl.color.y;
      colorIntensity[i * 4U + 2U] = sl.color.z;
      colorIntensity[i * 4U + 3U] = sl.intensity;
      params[i * 4U + 0U] = std::cos(sl.outerConeAngle);
    }
    dev->set_param_vec4_array(backend.pbrSpotLightPosRadiusParam, posRadius,
                              static_cast<std::int32_t>(spotCount));
    dev->set_param_vec4_array(backend.pbrSpotLightDirInnerParam, dirInner,
                              static_cast<std::int32_t>(spotCount));
    dev->set_param_vec4_array(backend.pbrSpotLightColorParam, colorIntensity,
                              static_cast<std::int32_t>(spotCount));
    dev->set_param_vec4_array(backend.pbrSpotLightParamsParam, params,
                              static_cast<std::int32_t>(spotCount));
  }
}

struct DistanceFogUniformLocations final {
  ShaderParam mode{};
  ShaderParam start{};
  ShaderParam end{};
  ShaderParam density{};
  ShaderParam color{};
};

struct HeightFogUniformLocations final {
  ShaderParam enabled{};
  ShaderParam baseHeight{};
  ShaderParam density{};
  ShaderParam falloff{};
  ShaderParam stepCount{};
};

void upload_distance_fog_uniforms(
    const RenderDevice *dev, const DistanceFogUniformLocations &locations,
    const DistanceFogSettings &settings) noexcept {
  const DistanceFogSettings fog = normalize_distance_fog_settings(settings);
  if (locations.mode.valid()) {
    dev->set_param_i32(locations.mode, static_cast<std::int32_t>(fog.mode));
  }
  if (locations.start.valid()) {
    dev->set_param_f32(locations.start, fog.start);
  }
  if (locations.end.valid()) {
    dev->set_param_f32(locations.end, fog.end);
  }
  if (locations.density.valid()) {
    dev->set_param_f32(locations.density, fog.density);
  }
  if (locations.color.valid()) {
    dev->set_param_vec3(locations.color, &fog.color.x);
  }
}

void upload_height_fog_uniforms(
    const RenderDevice *dev, const HeightFogUniformLocations &locations,
    const HeightFogSettings &settings) noexcept {
  const HeightFogSettings fog = normalize_height_fog_settings(settings);
  if (locations.enabled.valid()) {
    dev->set_param_i32(locations.enabled, fog.enabled ? 1 : 0);
  }
  if (locations.baseHeight.valid()) {
    dev->set_param_f32(locations.baseHeight, fog.baseHeight);
  }
  if (locations.density.valid()) {
    dev->set_param_f32(locations.density, fog.density);
  }
  if (locations.falloff.valid()) {
    dev->set_param_f32(locations.falloff, fog.falloff);
  }
  if (locations.stepCount.valid()) {
    dev->set_param_i32(locations.stepCount, fog.stepCount);
  }
}

void upload_pbr_distance_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const DistanceFogSettings &settings) noexcept {
  upload_distance_fog_uniforms(
      dev,
      DistanceFogUniformLocations{backend.pbrFogModeLocation,
                                  backend.pbrFogStartLocation,
                                  backend.pbrFogEndLocation,
                                  backend.pbrFogDensityLocation,
                                  backend.pbrFogColorLocation},
      settings);
}

void upload_pbr_height_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const HeightFogSettings &settings) noexcept {
  upload_height_fog_uniforms(
      dev,
      HeightFogUniformLocations{backend.pbrHeightFogEnabledLocation,
                                backend.pbrHeightFogBaseHeightLocation,
                                backend.pbrHeightFogDensityLocation,
                                backend.pbrHeightFogFalloffLocation,
                                backend.pbrHeightFogStepCountLocation},
      settings);
}

void upload_pbr_foliage_uniforms(const BackendState &backend,
                                 const RenderDevice *dev,
                                 const DrawCommand &command) noexcept {
  if (backend.pbrFoliageWindStrengthLocation.valid()) {
    dev->set_param_f32(backend.pbrFoliageWindStrengthLocation,
                           command.foliageWindStrength);
  }
  if (backend.pbrFoliageWindFrequencyLocation.valid()) {
    dev->set_param_f32(backend.pbrFoliageWindFrequencyLocation,
                           command.foliageWindFrequency);
  }
  if (backend.pbrFoliagePhaseLocation.valid()) {
    dev->set_param_f32(backend.pbrFoliagePhaseLocation,
                           command.foliageWindPhase);
  }
}

void upload_gbuffer_foliage_uniforms(const BackendState &backend,
                                     const RenderDevice *dev,
                                     const DrawCommand &command) noexcept {
  if (backend.gbufFoliageWindStrengthLoc.valid()) {
    dev->set_param_f32(backend.gbufFoliageWindStrengthLoc,
                           command.foliageWindStrength);
  }
  if (backend.gbufFoliageWindFrequencyLoc.valid()) {
    dev->set_param_f32(backend.gbufFoliageWindFrequencyLoc,
                           command.foliageWindFrequency);
  }
  if (backend.gbufFoliagePhaseLoc.valid()) {
    dev->set_param_f32(backend.gbufFoliagePhaseLoc,
                           command.foliageWindPhase);
  }
}

void upload_material_texture_slots(
    const MaterialTextureUniformLocs &locs, const RenderDevice *dev,
    const Material &material, DeviceTextureHandle fallbackTex,
    DeviceTextureHandle boundMaterialTex[4]) noexcept {
  constexpr std::uint32_t kMetallicRoughnessSlot = 1U;
  constexpr std::uint32_t kEmissiveSlot = 2U;
  constexpr std::uint32_t kOcclusionSlot = 3U;
  constexpr std::uint32_t kOpacitySlot = 4U;

  auto bindSlot = [&](ShaderParam hasParam, ShaderParam mapParam,
                      std::uint32_t slot, TextureHandle handle,
                      DeviceTextureHandle *boundTex) {
    const DeviceTextureHandle deviceTex = texture_device_handle(handle);
    const bool has = (handle != kInvalidTextureHandle) &&
                     (deviceTex != kInvalidDeviceTexture);
    if (hasParam.valid()) {
      dev->set_param_i32(hasParam, has ? 1 : 0);
    }
    if (mapParam.valid()) {
      dev->set_param_i32(mapParam, static_cast<std::int32_t>(slot));
    }
    // Absent slots bind the fallback, never nothing: a stale binding
    // of the pass's own render target trips WebGL's declaration-based
    // feedback-loop rejection and silently drops the draw.
    const DeviceTextureHandle desired = has ? deviceTex : fallbackTex;
    if (desired != *boundTex) {
      dev->bind_texture_slot(slot, desired);
      *boundTex = desired;
    }
  };

  bindSlot(locs.hasMetallicRoughness, locs.metallicRoughnessMap,
          kMetallicRoughnessSlot, material.metallicRoughnessTexture,
          &boundMaterialTex[0]);
  bindSlot(locs.hasEmissive, locs.emissiveMap, kEmissiveSlot,
          material.emissiveTexture, &boundMaterialTex[1]);
  bindSlot(locs.hasOcclusion, locs.occlusionMap, kOcclusionSlot,
          material.occlusionTexture, &boundMaterialTex[2]);
  bindSlot(locs.hasOpacity, locs.opacityMap, kOpacitySlot,
          material.opacityTexture, &boundMaterialTex[3]);

  if (locs.alphaMode.valid()) {
    dev->set_param_i32(locs.alphaMode,
                       static_cast<std::int32_t>(material.alphaMode));
  }
  if (locs.alphaCutoff.valid()) {
    dev->set_param_f32(locs.alphaCutoff, material.alphaCutoff);
  }
  if (locs.uvTiling.valid()) {
    dev->set_param_vec2(locs.uvTiling, &material.uvTiling.x);
  }
  if (locs.uvOffset.valid()) {
    dev->set_param_vec2(locs.uvOffset, &material.uvOffset.x);
  }
}

void upload_deferred_distance_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const DistanceFogSettings &settings) noexcept {
  upload_distance_fog_uniforms(
      dev,
      DistanceFogUniformLocations{backend.dlFogModeLoc, backend.dlFogStartLoc,
                                  backend.dlFogEndLoc, backend.dlFogDensityLoc,
                                  backend.dlFogColorLoc},
      settings);
}

void upload_deferred_height_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const HeightFogSettings &settings) noexcept {
  upload_height_fog_uniforms(
      dev,
      HeightFogUniformLocations{backend.dlHeightFogEnabledLoc,
                                backend.dlHeightFogBaseHeightLoc,
                                backend.dlHeightFogDensityLoc,
                                backend.dlHeightFogFalloffLoc,
                                backend.dlHeightFogStepCountLoc},
      settings);
}

void bind_pbr_shadow_uniforms(const BackendState &backend,
                              const RenderDevice *dev,
                              const SceneLightData &lights, bool shadowEnabled,
                              bool spotShadowEnabled,
                              bool pointShadowEnabled) noexcept {
  if ((dev == nullptr) || (dev->set_param_i32 == nullptr)) {
    return;
  }

  // Flat vocabulary, array samplers: the cascade and spot
  // sets each bind one Tex2DArray (layer = slot); matrices go up as one
  // mat4 array, splits/light indices/pos+far as packed vec4 payloads.
  // The disabled state still binds the array fallback: Vulkan-family
  // backends need every declared sampler descriptor valid at draw.
  dev->bind_texture_slot(
      static_cast<std::uint32_t>(kShadowCascadeArrayUnit),
      shadowEnabled ? backend.shadowState.depthArrayTexture
                    : backend.fallbackTexture2DArray);
  if (backend.pbrShadowMapArrayLoc.valid()) {
    dev->set_param_i32(backend.pbrShadowMapArrayLoc,
                       kShadowCascadeArrayUnit);
  }
  float shadowMatrices[kShadowCascadeCount * 16U] = {};
  float cascadeSplits[4] = {};
  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    std::memcpy(
        &shadowMatrices[c * 16U],
        &backend.shadowState.cascades[c].lightViewProjection.columns[0].x,
        sizeof(float) * 16U);
    cascadeSplits[c] = backend.shadowState.cascades[c].splitDistance;
  }
  if ((dev->set_param_mat4_array != nullptr) &&
      backend.pbrShadowMatrixParam.valid()) {
    dev->set_param_mat4_array(backend.pbrShadowMatrixParam, shadowMatrices,
                              static_cast<std::int32_t>(kShadowCascadeCount));
  }
  if (backend.pbrCascadeSplitsParam.valid()) {
    dev->set_param_vec4(backend.pbrCascadeSplitsParam, cascadeSplits);
  }
  if (backend.pbrShadowEnabledLoc.valid()) {
    dev->set_param_i32(backend.pbrShadowEnabledLoc, shadowEnabled ? 1 : 0);
  }

  dev->bind_texture_slot(
      static_cast<std::uint32_t>(kSpotShadowArrayUnit),
      spotShadowEnabled ? backend.spotShadowState.depthArrayTexture
                        : backend.fallbackTexture2DArray);
  if (backend.pbrSpotShadowMapArrayLoc.valid()) {
    dev->set_param_i32(backend.pbrSpotShadowMapArrayLoc,
                       kSpotShadowArrayUnit);
  }
  // Slot indices are scene indices; the forward shader compares them
  // against its position in the uploaded (nearest) light arrays.
  const ForwardLightSelection selection = select_forward_lights(lights);
  float spotMatrices[kMaxSpotShadowLights * 16U] = {};
  float spotLightIdx[4] = {};
  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    const auto &slot = backend.spotShadowState.slots[s];
    std::memcpy(&spotMatrices[s * 16U],
                &slot.lightViewProjection.columns[0].x,
                sizeof(float) * 16U);
    spotLightIdx[s] = forward_slot_index(selection.spot.data(),
                                         selection.spotCount, slot.lightIndex);
  }
  if ((dev->set_param_mat4_array != nullptr) &&
      backend.pbrSpotShadowMatrixParam.valid()) {
    dev->set_param_mat4_array(backend.pbrSpotShadowMatrixParam, spotMatrices,
                              static_cast<std::int32_t>(kMaxSpotShadowLights));
  }
  if (backend.pbrSpotShadowLightIdxParam.valid()) {
    dev->set_param_vec4(backend.pbrSpotShadowLightIdxParam, spotLightIdx);
  }
  if (backend.pbrSpotShadowEnabledLoc.valid()) {
    dev->set_param_i32(backend.pbrSpotShadowEnabledLoc,
                         spotShadowEnabled ? 1 : 0);
  }

  float pointPosFar[kMaxPointShadowLights * 4U] = {};
  float pointLightIdx[4] = {};
  for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
    const auto &slot = backend.pointShadowState.slots[s];
    const auto texUnit = static_cast<std::uint32_t>(kPointShadowUnitBase) +
                         static_cast<std::uint32_t>(s);
    if (pointShadowEnabled && (dev->bind_texture_slot != nullptr)) {
      dev->bind_texture_slot(texUnit, slot.depthCubemap);
    } else if (dev->bind_texture_slot != nullptr) {
      dev->bind_texture_slot(texUnit, backend.fallbackCubemap);
    }
    if (backend.pbrPointShadowMapLocs[s].valid()) {
      dev->set_param_i32(backend.pbrPointShadowMapLocs[s],
                         static_cast<std::int32_t>(texUnit));
    }
    const math::Vec3 lightPos =
        point_shadow_slot_light_position(slot.lightIndex, lights);
    pointPosFar[s * 4U + 0U] = lightPos.x;
    pointPosFar[s * 4U + 1U] = lightPos.y;
    pointPosFar[s * 4U + 2U] = lightPos.z;
    pointPosFar[s * 4U + 3U] = slot.farPlane;
    pointLightIdx[s] = forward_slot_index(
        selection.point.data(), selection.pointCount, slot.lightIndex);
  }
  if ((dev->set_param_vec4_array != nullptr) &&
      backend.pbrPointShadowPosFarParam.valid()) {
    dev->set_param_vec4_array(
        backend.pbrPointShadowPosFarParam, pointPosFar,
        static_cast<std::int32_t>(kMaxPointShadowLights));
  }
  if (backend.pbrPointShadowLightIdxParam.valid()) {
    dev->set_param_vec4(backend.pbrPointShadowLightIdxParam, pointLightIdx);
  }
  if (backend.pbrPointShadowEnabledLoc.valid()) {
    dev->set_param_i32(backend.pbrPointShadowEnabledLoc,
                         pointShadowEnabled ? 1 : 0);
  }
}

void unbind_pbr_shadow_textures(const RenderDevice *dev) noexcept {
  if ((dev == nullptr) || (dev->bind_texture_slot == nullptr)) {
    return;
  }
  dev->bind_texture_slot(static_cast<std::uint32_t>(kShadowCascadeArrayUnit),
                         kInvalidDeviceTexture);
  dev->bind_texture_slot(static_cast<std::uint32_t>(kSpotShadowArrayUnit),
                         kInvalidDeviceTexture);
  for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
    dev->bind_texture_slot(
        static_cast<std::uint32_t>(kPointShadowUnitBase) +
            static_cast<std::uint32_t>(s),
        kInvalidDeviceTexture);
  }
}

/// Unbinds the environment IBL texture units after a forward PBR pass.
void unbind_pbr_ibl_textures(const RenderDevice *dev) noexcept {
  if ((dev == nullptr) || (dev->bind_texture_slot == nullptr)) {
    return;
  }
  dev->bind_texture_slot(kIblIrradianceUnit, kInvalidDeviceTexture);
  dev->bind_texture_slot(kIblPrefilteredUnit, kInvalidDeviceTexture);
  dev->bind_texture_slot(kIblBrdfLutUnit, kInvalidDeviceTexture);
}

DistanceFogSettings
distance_fog_settings_from_cvars(BackendState &backend) noexcept {
  const FlushCVars &cvars = backend.cvars;
  DistanceFogSettings settings{};

  // String cvars are read under the registry lock, so the flush re-parses
  // each one only when its change stamp moved and otherwise serves the
  // cached value; the fallbacks are the DistanceFogSettings defaults, the
  // same values an unregistered cvar (stamp 0) leaves in the cache.
  const std::uint64_t modeStamp = cvars.fogMode.change_stamp();
  if (modeStamp != backend.fogModeStamp) {
    backend.fogMode = parse_distance_fog_mode(cvars.fogMode.get_string("exp2"));
    backend.fogModeStamp = modeStamp;
  }
  settings.mode = backend.fogMode;

  const std::uint64_t colorStamp = cvars.fogColor.change_stamp();
  if (colorStamp != backend.fogColorStamp) {
    math::Vec3 color = settings.color;
    backend.fogColor =
        parse_distance_fog_color(cvars.fogColor.get_string("0.55 0.65 0.75"),
                                 &color)
            ? color
            : settings.color;
    backend.fogColorStamp = colorStamp;
  }
  settings.color = backend.fogColor;

  settings.start = cvars.fogStart.get_float(settings.start);
  settings.end = cvars.fogEnd.get_float(settings.end);
  settings.density = cvars.fogDensity.get_float(settings.density);

  return normalize_distance_fog_settings(settings);
}

HeightFogSettings
height_fog_settings_from_cvars(const FlushCVars &cvars) noexcept {
  HeightFogSettings settings{};
  settings.enabled = cvars.heightFog.get_bool(settings.enabled);
  settings.baseHeight = cvars.heightFogBase.get_float(settings.baseHeight);
  settings.density = cvars.heightFogDensity.get_float(settings.density);
  settings.falloff = cvars.heightFogFalloff.get_float(settings.falloff);
  settings.stepCount = cvars.heightFogSteps.get_int(settings.stepCount);
  return normalize_height_fog_settings(settings);
}

/// Returns whether can upload instance matrices.
bool can_upload_instance_matrices(const RenderDevice *dev) noexcept {
  return (dev != nullptr) && dev->caps.instancing &&
         (dev->set_geometry_instance_stream != nullptr) &&
         (dev->update_buffer != nullptr) &&
         (dev->draw_indexed_instanced != nullptr);
}

bool upload_instance_matrices(BackendState &backend, const RenderDevice *dev,
                              const GpuMesh &mesh,
                              CommandBufferView commandBufferView,
                              const StaticMeshBatch &batch) noexcept {
  if (!can_upload_instance_matrices(dev) ||
      (mesh.geometry == kInvalidDeviceGeometry) || (batch.count == 0U) ||
      (commandBufferView.data == nullptr)) {
    return false;
  }

  if (backend.instanceMatrixBuffer == kInvalidDeviceBuffer) {
    BufferDesc desc{};
    desc.usage = BufferUsage::Vertex;
    desc.access = BufferAccess::Stream;
    backend.instanceMatrixBuffer = dev->create_buffer(desc);
    if (backend.instanceMatrixBuffer == kInvalidDeviceBuffer) {
      return false;
    }
  }

  if (backend.instanceAttributes.size() < batch.count) {
    // A failed grow reports failure instead of terminating; the caller
    // already falls back to per-command (non-instanced) draws whenever this
    // function returns false, so a transient allocation failure degrades
    // this batch to individual draw calls rather than crashing the process.
    if (!backend.instanceAttributes.allocate(batch.count)) {
      return false;
    }
  }
  for (std::uint32_t i = 0U; i < batch.count; ++i) {
    const std::size_t commandIndex =
        static_cast<std::size_t>(batch.first) + static_cast<std::size_t>(i);
    const DrawCommand &command = commandBufferView.data[commandIndex];
    backend.instanceAttributes[i].model = command.modelMatrix;
    backend.instanceAttributes[i].foliage =
        math::Vec4(command.foliageWindPhase,
                   static_cast<float>(command.foliageLodIndex), 0.0F, 0.0F);
  }

  dev->update_buffer(
      backend.instanceMatrixBuffer, backend.instanceAttributes.data(),
      static_cast<std::ptrdiff_t>(backend.instanceAttributes.size() *
                                  sizeof(InstanceAttributes)));

  VertexLayout instanceLayout{};
  instanceLayout.strideBytes =
      static_cast<std::int32_t>(sizeof(InstanceAttributes));
  for (std::size_t column = 0U; column < kInstanceModelColumns; ++column) {
    instanceLayout.attributes[column] = {
        static_cast<VertexSemantic>(
            static_cast<std::uint8_t>(VertexSemantic::InstanceModel0) +
            static_cast<std::uint8_t>(column)),
        4,
        static_cast<std::int32_t>(offsetof(InstanceAttributes, model) +
                                  (sizeof(float) * 4U * column))};
  }
  instanceLayout.attributes[kInstanceModelColumns] = {
      VertexSemantic::InstanceParams, 4,
      static_cast<std::int32_t>(offsetof(InstanceAttributes, foliage))};
  instanceLayout.attributeCount = kInstanceModelColumns + 1U;

  return dev->set_geometry_instance_stream(
      mesh.geometry, backend.instanceMatrixBuffer, instanceLayout);
}

// --- One forward draw -----------------------------------------------------

ForwardDrawProgram pbr_forward_draw_program(const BackendState &backend) noexcept {
  ForwardDrawProgram program{};
  program.albedo = backend.pbrAlbedoLocation;
  program.roughness = backend.pbrRoughnessLocation;
  program.metallic = backend.pbrMetallicLocation;
  program.opacity = backend.pbrOpacityLocation;
  program.emissive = backend.pbrEmissiveLocation;
  program.hasAlbedoTexture = backend.pbrHasAlbedoTextureLocation;
  program.model = backend.pbrModelLocation;
  program.mvp = backend.pbrMvpLocation;
  program.normalMatrix = backend.pbrNormalMatrixLocation;
  program.useInstancing = backend.pbrUseInstancingLocation;
  program.materialTextures = MaterialTextureUniformLocs{
      backend.pbrHasMetallicRoughnessTextureLocation,
      backend.pbrMetallicRoughnessMapLocation,
      backend.pbrHasEmissiveTextureLocation,
      backend.pbrEmissiveMapLocation,
      backend.pbrHasOcclusionTextureLocation,
      backend.pbrOcclusionMapLocation,
      backend.pbrHasOpacityTextureLocation,
      backend.pbrOpacityMapLocation,
      backend.pbrAlphaModeLocation,
      backend.pbrAlphaCutoffLocation,
      backend.pbrUvTilingLocation,
      backend.pbrUvOffsetLocation};
  return program;
}

std::size_t partition_shading_model_runs(const CommandBufferView &view,
                                         std::size_t start, std::size_t end,
                                         ShadingModelRun *runs,
                                         std::size_t capacity) noexcept {
  if ((runs == nullptr) || (capacity == 0U) || (view.data == nullptr) ||
      (start >= end)) {
    return 0U;
  }
  const std::size_t last =
      (end < static_cast<std::size_t>(view.count))
          ? end
          : static_cast<std::size_t>(view.count);
  if (start >= last) {
    return 0U;
  }

  std::size_t count = 0U;
  runs[0] = ShadingModelRun{start, 0U,
                            draw_key_shading_model(view.data[start].sortKey)};
  count = 1U;
  for (std::size_t i = start; i < last; ++i) {
    const std::uint8_t model = draw_key_shading_model(view.data[i].sortKey);
    if (model != runs[count - 1U].model) {
      if (count == capacity) {
        // More runs than the caller can hold. The tail keeps drawing,
        // joined onto the last run rather than dropped: a draw shaded by
        // the previous model is wrong, a draw missing entirely is worse.
        runs[count - 1U].count = last - runs[count - 1U].first;
        return count;
      }
      runs[count] = ShadingModelRun{i, 0U, model};
      ++count;
    }
    ++runs[count - 1U].count;
  }
  return count;
}

DeviceProgramHandle shading_model_program(const BackendState &backend,
                                          std::uint8_t model) noexcept {
  const DeviceProgramHandle fallback = backend.pbrProgram;
  if (!shading_model_is_valid(model)) {
    return fallback;
  }
  const DeviceProgramHandle program =
      backend.shadingModelPrograms[static_cast<std::size_t>(model)];
  if (program != kInvalidDeviceProgram) {
    return program;
  }
  static bool warnedMissingProgram = false;
  if (!warnedMissingProgram) {
    warnedMissingProgram = true;
    core::log_message(core::LogLevel::Warning, "renderer",
                      "a material selects a shading model whose program is "
                      "unavailable; those draws are shaded as physically "
                      "based");
  }
  return fallback;
}

void upload_forward_material(const ForwardDrawProgram &program,
                             const BackendState &backend,
                             const RenderDevice *dev,
                             const DrawCommand &command,
                             ForwardDrawBindings *bindings) noexcept {
  if ((dev == nullptr) || (bindings == nullptr)) {
    return;
  }
  const Material &material = command.material;
  if (program.albedo.valid()) {
    dev->set_param_vec3(program.albedo, &material.albedo.x);
  }
  if (program.roughness.valid()) {
    dev->set_param_f32(program.roughness, material.roughness);
  }
  if (program.metallic.valid()) {
    dev->set_param_f32(program.metallic, material.metallic);
  }
  if (program.opacity.valid()) {
    dev->set_param_f32(program.opacity, material.opacity);
  }
  if (program.emissive.valid()) {
    dev->set_param_vec3(program.emissive, &material.emissive.x);
  }
  upload_pbr_foliage_uniforms(backend, dev, command);

  const DeviceTextureHandle albedoTex =
      texture_device_handle(material.albedoTexture);
  const bool hasAlbedoTex = (material.albedoTexture != kInvalidTextureHandle) &&
                            (albedoTex != kInvalidDeviceTexture);
  if (program.hasAlbedoTexture.valid()) {
    dev->set_param_i32(program.hasAlbedoTexture, hasAlbedoTex ? 1 : 0);
  }
  if (hasAlbedoTex && (albedoTex != bindings->albedo)) {
    dev->bind_texture_slot(0U, albedoTex);
    bindings->albedo = albedoTex;
  } else if (!hasAlbedoTex &&
             (bindings->albedo != backend.fallbackTexture2D)) {
    // Fallback, not nothing: WebGL rejects draws whose declared samplers
    // still reference the pass's render target.
    dev->bind_texture_slot(0U, backend.fallbackTexture2D);
    bindings->albedo = backend.fallbackTexture2D;
  }
  upload_material_texture_slots(program.materialTextures, dev, material,
                                backend.fallbackTexture2D,
                                bindings->materialSlots);
}

void draw_forward_command(const ForwardDrawProgram &program,
                          const RenderDevice *dev, const DrawCommand &command,
                          const GpuMesh &mesh,
                          const math::Mat4 &viewProjection,
                          RendererFrameStats *frameStats) noexcept {
  if ((dev == nullptr) || (frameStats == nullptr)) {
    return;
  }
  const math::Mat4 model = compute_model_matrix(command);
  const math::Mat4 mvp = compute_mvp(model, viewProjection);
  float normalMatrix[9] = {};
  extract_normal_matrix(model, normalMatrix);

  // Cleared per draw, not per pass: the opaque batching path sets this
  // to 1 to issue an instanced batch, and a single draw that inherited
  // that 1 would read its transform from the instance buffer.
  if (program.useInstancing.valid()) {
    dev->set_param_i32(program.useInstancing, 0);
  }
  if (program.model.valid()) {
    dev->set_param_mat4(program.model, &model.columns[0].x);
  }
  dev->set_param_mat4(program.mvp, &mvp.columns[0].x);
  dev->set_param_mat3(program.normalMatrix, normalMatrix);

  ++frameStats->drawCalls;
  if (mesh.indexCount > 0U) {
    frameStats->triangleCount += (mesh.indexCount / 3U);
    dev->draw_indexed(mesh.geometry,
                      static_cast<std::int32_t>(mesh.indexCount));
  } else {
    frameStats->triangleCount += (mesh.vertexCount / 3U);
    dev->draw(mesh.geometry, PrimitiveTopology::Triangles, 0,
              static_cast<std::int32_t>(mesh.vertexCount));
  }
}

} // namespace engine::renderer
