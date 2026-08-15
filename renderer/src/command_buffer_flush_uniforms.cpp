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
constexpr std::uint32_t kInstanceModelAttrib0 = 3U;
constexpr std::uint32_t kInstanceModelAttribCount = 4U;
constexpr std::uint32_t kInstanceFoliageAttrib = 7U;
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
  if (backend.pbrIrradianceMapLoc >= 0) {
    dev->set_uniform_int(backend.pbrIrradianceMapLoc, kIblIrradianceUnit);
  }
  if (backend.pbrPrefilteredMapLoc >= 0) {
    dev->set_uniform_int(backend.pbrPrefilteredMapLoc, kIblPrefilteredUnit);
  }
  if (backend.pbrBrdfLutLoc >= 0) {
    dev->set_uniform_int(backend.pbrBrdfLutLoc, kIblBrdfLutUnit);
  }

  const bool enabled = iblAvailable && (backend.pbrIblEnabledLoc >= 0) &&
                       (dev->bind_texture_cubemap != nullptr);
  if (backend.pbrIblEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrIblEnabledLoc, enabled ? 1 : 0);
  }
  if (!enabled) {
    return;
  }

  dev->bind_texture_cubemap(kIblIrradianceUnit,
                            backend.irradianceEnvironmentTexture);
  dev->bind_texture_cubemap(kIblPrefilteredUnit,
                            backend.prefilteredEnvironmentTexture);
  dev->bind_texture(kIblBrdfLutUnit, backend.brdfLutTexture);
  if (backend.pbrPrefilteredMipsLoc >= 0) {
    dev->set_uniform_float(
        backend.pbrPrefilteredMipsLoc,
        static_cast<float>(backend.prefilteredEnvironmentMipLevels));
  }
}

void upload_pbr_lighting_uniforms(const BackendState &backend,
                                  const RenderDevice *dev,
                                  const SceneLightData &lights) noexcept {
  const std::size_t dirCount =
      std::min(lights.directionalLightCount, kMaxDirectionalLights);
  if (backend.pbrDirLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrDirLightCountLocation,
                         static_cast<std::int32_t>(dirCount));
  }
  for (std::size_t i = 0U; i < dirCount; ++i) {
    const auto &dl = lights.directionalLights[i];
    if (backend.pbrDirLightDir[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrDirLightDir[i], &dl.direction.x);
    }
    if (backend.pbrDirLightColor[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrDirLightColor[i], &dl.color.x);
    }
    if (backend.pbrDirLightIntensity[i] >= 0) {
      dev->set_uniform_float(backend.pbrDirLightIntensity[i], dl.intensity);
    }
  }

  const std::size_t pointCount =
      std::min(lights.pointLightCount, kForwardMaxPointLights);
  if (backend.pbrPointLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrPointLightCountLocation,
                         static_cast<std::int32_t>(pointCount));
  }
  for (std::size_t i = 0U; i < pointCount; ++i) {
    const auto &pl = lights.pointLights[i];
    if (backend.pbrPointLightPos[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrPointLightPos[i], &pl.position.x);
    }
    if (backend.pbrPointLightColor[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrPointLightColor[i], &pl.color.x);
    }
    if (backend.pbrPointLightIntensity[i] >= 0) {
      dev->set_uniform_float(backend.pbrPointLightIntensity[i], pl.intensity);
    }
    if (backend.pbrPointLightRadius[i] >= 0) {
      dev->set_uniform_float(backend.pbrPointLightRadius[i], pl.radius);
    }
  }

  const std::size_t spotCount =
      std::min(lights.spotLightCount, kForwardMaxSpotLights);
  if (backend.pbrSpotLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrSpotLightCountLocation,
                         static_cast<std::int32_t>(spotCount));
  }
  for (std::size_t i = 0U; i < spotCount; ++i) {
    const auto &sl = lights.spotLights[i];
    if (backend.pbrSpotLightPos[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrSpotLightPos[i], &sl.position.x);
    }
    if (backend.pbrSpotLightDir[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrSpotLightDir[i], &sl.direction.x);
    }
    if (backend.pbrSpotLightColor[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrSpotLightColor[i], &sl.color.x);
    }
    if (backend.pbrSpotLightIntensity[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightIntensity[i], sl.intensity);
    }
    if (backend.pbrSpotLightRadius[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightRadius[i], sl.radius);
    }
    // Shaders compare these against dot(L, -spotDir), a cosine — upload
    // cosines, not the stored radian angles.
    if (backend.pbrSpotLightInnerCone[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightInnerCone[i],
                             std::cos(sl.innerConeAngle));
    }
    if (backend.pbrSpotLightOuterCone[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightOuterCone[i],
                             std::cos(sl.outerConeAngle));
    }
  }
}

struct DistanceFogUniformLocations final {
  int mode = -1;
  int start = -1;
  int end = -1;
  int density = -1;
  int color = -1;
};

struct HeightFogUniformLocations final {
  int enabled = -1;
  int baseHeight = -1;
  int density = -1;
  int falloff = -1;
  int stepCount = -1;
};

void upload_distance_fog_uniforms(
    const RenderDevice *dev, const DistanceFogUniformLocations &locations,
    const DistanceFogSettings &settings) noexcept {
  const DistanceFogSettings fog = normalize_distance_fog_settings(settings);
  if (locations.mode >= 0) {
    dev->set_uniform_int(locations.mode, static_cast<std::int32_t>(fog.mode));
  }
  if (locations.start >= 0) {
    dev->set_uniform_float(locations.start, fog.start);
  }
  if (locations.end >= 0) {
    dev->set_uniform_float(locations.end, fog.end);
  }
  if (locations.density >= 0) {
    dev->set_uniform_float(locations.density, fog.density);
  }
  if (locations.color >= 0) {
    dev->set_uniform_vec3(locations.color, &fog.color.x);
  }
}

void upload_height_fog_uniforms(
    const RenderDevice *dev, const HeightFogUniformLocations &locations,
    const HeightFogSettings &settings) noexcept {
  const HeightFogSettings fog = normalize_height_fog_settings(settings);
  if (locations.enabled >= 0) {
    dev->set_uniform_int(locations.enabled, fog.enabled ? 1 : 0);
  }
  if (locations.baseHeight >= 0) {
    dev->set_uniform_float(locations.baseHeight, fog.baseHeight);
  }
  if (locations.density >= 0) {
    dev->set_uniform_float(locations.density, fog.density);
  }
  if (locations.falloff >= 0) {
    dev->set_uniform_float(locations.falloff, fog.falloff);
  }
  if (locations.stepCount >= 0) {
    dev->set_uniform_int(locations.stepCount, fog.stepCount);
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
  if (backend.pbrFoliageWindStrengthLocation >= 0) {
    dev->set_uniform_float(backend.pbrFoliageWindStrengthLocation,
                           command.foliageWindStrength);
  }
  if (backend.pbrFoliageWindFrequencyLocation >= 0) {
    dev->set_uniform_float(backend.pbrFoliageWindFrequencyLocation,
                           command.foliageWindFrequency);
  }
  if (backend.pbrFoliagePhaseLocation >= 0) {
    dev->set_uniform_float(backend.pbrFoliagePhaseLocation,
                           command.foliageWindPhase);
  }
}

void upload_gbuffer_foliage_uniforms(const BackendState &backend,
                                     const RenderDevice *dev,
                                     const DrawCommand &command) noexcept {
  if (backend.gbufFoliageWindStrengthLoc >= 0) {
    dev->set_uniform_float(backend.gbufFoliageWindStrengthLoc,
                           command.foliageWindStrength);
  }
  if (backend.gbufFoliageWindFrequencyLoc >= 0) {
    dev->set_uniform_float(backend.gbufFoliageWindFrequencyLoc,
                           command.foliageWindFrequency);
  }
  if (backend.gbufFoliagePhaseLoc >= 0) {
    dev->set_uniform_float(backend.gbufFoliagePhaseLoc,
                           command.foliageWindPhase);
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
  if ((dev == nullptr) || (dev->set_uniform_int == nullptr)) {
    return;
  }

  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    const int texUnit = 6 + static_cast<int>(c);
    if (shadowEnabled) {
      dev->bind_texture(texUnit, backend.shadowState.depthTextures[c]);
    }
    if (backend.pbrShadowMapLocs[c] >= 0) {
      dev->set_uniform_int(backend.pbrShadowMapLocs[c], texUnit);
    }
    if (backend.pbrShadowMatrixLocs[c] >= 0) {
      dev->set_uniform_mat4(
          backend.pbrShadowMatrixLocs[c],
          &backend.shadowState.cascades[c].lightViewProjection.columns[0].x);
    }
    if (backend.pbrCascadeSplitLocs[c] >= 0) {
      dev->set_uniform_float(backend.pbrCascadeSplitLocs[c],
                             backend.shadowState.cascades[c].splitDistance);
    }
  }
  if (backend.pbrShadowEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrShadowEnabledLoc, shadowEnabled ? 1 : 0);
  }

  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    const auto &slot = backend.spotShadowState.slots[s];
    const int texUnit = 10 + static_cast<int>(s);
    if (spotShadowEnabled) {
      dev->bind_texture(texUnit, slot.depthTexture);
    }
    if (backend.pbrSpotShadowMapLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrSpotShadowMapLocs[s], texUnit);
    }
    if (backend.pbrSpotShadowMatrixLocs[s] >= 0) {
      dev->set_uniform_mat4(backend.pbrSpotShadowMatrixLocs[s],
                            &slot.lightViewProjection.columns[0].x);
    }
    if (backend.pbrSpotShadowLightIdxLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrSpotShadowLightIdxLocs[s],
                           slot.lightIndex);
    }
  }
  if (backend.pbrSpotShadowEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrSpotShadowEnabledLoc,
                         spotShadowEnabled ? 1 : 0);
  }

  for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
    const auto &slot = backend.pointShadowState.slots[s];
    const int texUnit = 14 + static_cast<int>(s);
    if (pointShadowEnabled && (dev->bind_texture_cubemap != nullptr)) {
      dev->bind_texture_cubemap(texUnit, slot.depthCubemap);
    }
    if (backend.pbrPointShadowMapLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrPointShadowMapLocs[s], texUnit);
    }
    if (backend.pbrPointShadowLightPosLocs[s] >= 0) {
      const math::Vec3 lightPos =
          point_shadow_slot_light_position(slot.lightIndex, lights);
      dev->set_uniform_vec3(backend.pbrPointShadowLightPosLocs[s], &lightPos.x);
    }
    if (backend.pbrPointShadowFarPlaneLocs[s] >= 0) {
      dev->set_uniform_float(backend.pbrPointShadowFarPlaneLocs[s],
                             slot.farPlane);
    }
    if (backend.pbrPointShadowLightIdxLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrPointShadowLightIdxLocs[s],
                           slot.lightIndex);
    }
  }
  if (backend.pbrPointShadowEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrPointShadowEnabledLoc,
                         pointShadowEnabled ? 1 : 0);
  }
}

void unbind_pbr_shadow_textures(const RenderDevice *dev) noexcept {
  if (dev == nullptr) {
    return;
  }
  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    dev->bind_texture(6 + static_cast<int>(c), 0U);
  }
  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    dev->bind_texture(10 + static_cast<int>(s), 0U);
  }
  if (dev->bind_texture_cubemap != nullptr) {
    for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
      dev->bind_texture_cubemap(14 + static_cast<int>(s), 0U);
    }
  }
}

/// Unbinds the environment IBL texture units after a forward PBR pass.
void unbind_pbr_ibl_textures(const RenderDevice *dev) noexcept {
  if ((dev == nullptr) || (dev->bind_texture_cubemap == nullptr)) {
    return;
  }
  dev->bind_texture_cubemap(kIblIrradianceUnit, 0U);
  dev->bind_texture_cubemap(kIblPrefilteredUnit, 0U);
  dev->bind_texture(kIblBrdfLutUnit, 0U);
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
  return (dev != nullptr) && (dev->vertex_attrib_divisor != nullptr) &&
         (dev->draw_elements_triangles_u32_instanced != nullptr);
}

bool upload_instance_matrices(BackendState &backend, const RenderDevice *dev,
                              const GpuMesh &mesh,
                              CommandBufferView commandBufferView,
                              const StaticMeshBatch &batch) noexcept {
  if (!can_upload_instance_matrices(dev) || (mesh.vertexArray == 0U) ||
      (batch.count == 0U) || (commandBufferView.data == nullptr)) {
    return false;
  }

  if (backend.instanceMatrixBuffer == 0U) {
    backend.instanceMatrixBuffer = dev->create_buffer();
    if (backend.instanceMatrixBuffer == 0U) {
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

  dev->bind_vertex_array(mesh.vertexArray);
  dev->bind_array_buffer(backend.instanceMatrixBuffer);
  dev->buffer_data_array(
      backend.instanceAttributes.data(),
      static_cast<std::ptrdiff_t>(backend.instanceAttributes.size() *
                                  sizeof(InstanceAttributes)));

  constexpr std::int32_t stride =
      static_cast<std::int32_t>(sizeof(InstanceAttributes));
  for (std::uint32_t column = 0U; column < kInstanceModelAttribCount;
       ++column) {
    const std::uint32_t attrib = kInstanceModelAttrib0 + column;
    const auto offset = reinterpret_cast<const void *>(
        static_cast<std::uintptr_t>(offsetof(InstanceAttributes, model) +
                                    (sizeof(float) * 4U * column)));
    dev->enable_vertex_attrib(attrib);
    dev->vertex_attrib_float(attrib, 4, stride, offset);
    dev->vertex_attrib_divisor(attrib, 1U);
  }

  const auto foliageOffset = reinterpret_cast<const void *>(
      static_cast<std::uintptr_t>(offsetof(InstanceAttributes, foliage)));
  dev->enable_vertex_attrib(kInstanceFoliageAttrib);
  dev->vertex_attrib_float(kInstanceFoliageAttrib, 4, stride, foliageOffset);
  dev->vertex_attrib_divisor(kInstanceFoliageAttrib, 1U);

  return true;
}


} // namespace engine::renderer
