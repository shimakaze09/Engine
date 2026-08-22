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

void upload_pbr_lighting_uniforms(const BackendState &backend,
                                  const RenderDevice *dev,
                                  const SceneLightData &lights) noexcept {
  // #138 flat array vocabulary: pack per-light vec4 elements into fixed
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

  const std::size_t pointCount =
      std::min(lights.pointLightCount, kForwardMaxPointLights);
  if (backend.pbrPointLightCountLocation.valid()) {
    dev->set_param_i32(backend.pbrPointLightCountLocation,
                         static_cast<std::int32_t>(pointCount));
  }
  if (pointCount > 0U) {
    float posRadius[kForwardMaxPointLights * 4U] = {};
    float colorIntensity[kForwardMaxPointLights * 4U] = {};
    for (std::size_t i = 0U; i < pointCount; ++i) {
      const auto &pl = lights.pointLights[i];
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

  const std::size_t spotCount =
      std::min(lights.spotLightCount, kForwardMaxSpotLights);
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
      const auto &sl = lights.spotLights[i];
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
    // feedback-loop rejection and silently drops the draw (#293).
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

  // #138 flat vocabulary: per-slot samplers stay individual (bgfx has no
  // sampler arrays); matrices go up as one mat4 array, splits/light
  // indices/pos+far as packed vec4 payloads.
  float shadowMatrices[kShadowCascadeCount * 16U] = {};
  float cascadeSplits[4] = {};
  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    const auto texUnit = static_cast<std::uint32_t>(6U + c);
    // Disabled slots still bind the fallback: Vulkan-family backends
    // need every declared sampler descriptor valid at draw.
    if (shadowEnabled) {
      dev->bind_texture_slot(texUnit, backend.shadowState.depthTextures[c]);
    } else {
      dev->bind_texture_slot(texUnit, backend.fallbackTexture2D);
    }
    if (backend.pbrShadowMapLocs[c].valid()) {
      dev->set_param_i32(backend.pbrShadowMapLocs[c],
                         static_cast<std::int32_t>(texUnit));
    }
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

  float spotMatrices[kMaxSpotShadowLights * 16U] = {};
  float spotLightIdx[4] = {};
  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    const auto &slot = backend.spotShadowState.slots[s];
    const auto texUnit = static_cast<std::uint32_t>(10U + s);
    if (spotShadowEnabled) {
      dev->bind_texture_slot(texUnit, slot.depthTexture);
    } else {
      dev->bind_texture_slot(texUnit, backend.fallbackTexture2D);
    }
    if (backend.pbrSpotShadowMapLocs[s].valid()) {
      dev->set_param_i32(backend.pbrSpotShadowMapLocs[s],
                         static_cast<std::int32_t>(texUnit));
    }
    std::memcpy(&spotMatrices[s * 16U],
                &slot.lightViewProjection.columns[0].x,
                sizeof(float) * 16U);
    spotLightIdx[s] = static_cast<float>(slot.lightIndex);
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
    const auto texUnit = static_cast<std::uint32_t>(14U + s);
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
    pointLightIdx[s] = static_cast<float>(slot.lightIndex);
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
  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    dev->bind_texture_slot(static_cast<std::uint32_t>(6U + c),
                           kInvalidDeviceTexture);
  }
  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    dev->bind_texture_slot(static_cast<std::uint32_t>(10U + s),
                           kInvalidDeviceTexture);
  }
  for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
    dev->bind_texture_slot(static_cast<std::uint32_t>(14U + s),
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

DistanceFogSettings distance_fog_settings_from_cvars() noexcept {
  DistanceFogSettings settings{};
  settings.mode =
      parse_distance_fog_mode(core::cvar_get_string("r_fog_mode", "exp2"));
  settings.start = core::cvar_get_float("r_fog_start", settings.start);
  settings.end = core::cvar_get_float("r_fog_end", settings.end);
  settings.density = core::cvar_get_float("r_fog_density", settings.density);

  math::Vec3 color = settings.color;
  if (parse_distance_fog_color(
          core::cvar_get_string("r_fog_color", "0.55 0.65 0.75"), &color)) {
    settings.color = color;
  }

  return normalize_distance_fog_settings(settings);
}

HeightFogSettings height_fog_settings_from_cvars() noexcept {
  HeightFogSettings settings{};
  settings.enabled = core::cvar_get_bool("r_height_fog", settings.enabled);
  settings.baseHeight =
      core::cvar_get_float("r_height_fog_base", settings.baseHeight);
  settings.density =
      core::cvar_get_float("r_height_fog_density", settings.density);
  settings.falloff =
      core::cvar_get_float("r_height_fog_falloff", settings.falloff);
  settings.stepCount =
      core::cvar_get_int("r_height_fog_steps", settings.stepCount);
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
    // A failed grow reports failure instead of terminating (audit #204:
    // nothrow instead of a terminating std::vector throw); the caller
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


} // namespace engine::renderer
