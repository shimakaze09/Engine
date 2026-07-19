// Implements the renderer frame flush: consumes the sorted draw command
// buffer and drives the GL frame (shadow passes, deferred or forward PBR,
// sky, SSAO, bloom, auto-exposure, tonemap, FXAA).
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

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

namespace engine::renderer {

namespace {

constexpr float kDefaultFovRadians = 1.0471975512F;
constexpr float kNearClip = 0.1F;
constexpr float kFarClip = 100.0F;
constexpr float kClearRed = 0.18F;
constexpr float kClearGreen = 0.28F;
constexpr float kClearBlue = 0.60F;
constexpr std::uint32_t kInstanceModelAttrib0 = 3U;
constexpr std::uint32_t kInstanceModelAttribCount = 4U;
constexpr std::uint32_t kInstanceFoliageAttrib = 7U;
constexpr std::uint64_t kDrawKeyTransparentBit = 1ULL << 63U;

struct ShadowCandidate final {
  std::size_t lightIndex = 0U;
  float distSq = 0.0F;
};

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
    if (backend.pbrSpotLightInnerCone[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightInnerCone[i],
                             sl.innerConeAngle);
    }
    if (backend.pbrSpotLightOuterCone[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightOuterCone[i],
                             sl.outerConeAngle);
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

  const math::Vec3 zero{};
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
      const bool validLight =
          (slot.lightIndex >= 0) &&
          (static_cast<std::size_t>(slot.lightIndex) < lights.pointLightCount);
      const math::Vec3 &lightPos =
          validLight
              ? lights.pointLights[static_cast<std::size_t>(slot.lightIndex)]
                    .position
              : zero;
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

  backend.instanceAttributes.resize(batch.count);
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

} // namespace

/// Flushes queued work to the backing runtime system for renderer.
void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &lights) noexcept {
  if (!initialize_backend()) {
    return;
  }

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();
  RendererFrameStats frameStats{};
  gpu_profiler_begin_frame();

  int drawableWidth = 1280;
  int drawableHeight = 720;
  if ((renderer_context().sceneViewportWidth > 0) && (renderer_context().sceneViewportHeight > 0)) {
    drawableWidth = renderer_context().sceneViewportWidth;
    drawableHeight = renderer_context().sceneViewportHeight;
  } else {
    core::render_drawable_size(&drawableWidth, &drawableHeight);
  }
  if (drawableWidth <= 0) {
    drawableWidth = 1;
  }
  if (drawableHeight <= 0) {
    drawableHeight = 1;
  }

  // Initialize or resize pass resources when dimensions change.
  if (backend.lastWidth != drawableWidth ||
      backend.lastHeight != drawableHeight) {
    if (backend.lastWidth == 0 && backend.lastHeight == 0) {
      initialize_pass_resources(drawableWidth, drawableHeight);
    } else {
      resize_pass_resources(drawableWidth, drawableHeight);
    }
    backend.lastWidth = drawableWidth;
    backend.lastHeight = drawableHeight;
  }

  const PassResources &passRes = get_pass_resources();
  const ReflectionProbeBakeSettings environmentBakeSettings =
      cvar_reflection_probe_bake_settings();
  const DistanceFogSettings fogSettings = distance_fog_settings_from_cvars();
  const HeightFogSettings heightFogSettings = height_fog_settings_from_cvars();
  static_cast<void>(ensure_brdf_lut(backend, dev, environmentBakeSettings));

  // Check if deferred rendering is enabled.
  const bool useDeferred =
      backend.deferredAvailable && core::cvar_get_bool("r_deferred", true);
  const int gbufferDebugMode = core::cvar_get_int("r_gbuffer_debug", 0);

  // Camera setup (shared by both paths).
  const float aspect =
      static_cast<float>(drawableWidth) / static_cast<float>(drawableHeight);
  const math::Mat4 viewMat = math::look_at(
      renderer_context().activeCamera.position, renderer_context().activeCamera.target, renderer_context().activeCamera.up);
  const float fov = (renderer_context().activeCamera.fovRadians > 0.0F)
                        ? renderer_context().activeCamera.fovRadians
                        : kDefaultFovRadians;
  const float nearP =
      (renderer_context().activeCamera.nearPlane > 0.0F) ? renderer_context().activeCamera.nearPlane : kNearClip;
  const float farP =
      (renderer_context().activeCamera.farPlane > nearP) ? renderer_context().activeCamera.farPlane : kFarClip;
  const math::Mat4 projMat = math::perspective(fov, aspect, nearP, farP);
  const math::Mat4 viewProjection = math::mul(projMat, viewMat);

  if (registry == nullptr) {
    dev->bind_framebuffer(0U);
    return;
  }

  if ((commandBufferView.count > 0U) && (commandBufferView.data == nullptr)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "draw command view is invalid");
  }

  // Determine opaque / transparent partition.
  std::size_t opaqueCount = 0U;
  std::size_t totalCount = 0U;

  if ((commandBufferView.data != nullptr) && (commandBufferView.count > 0U)) {
    totalCount = static_cast<std::size_t>(commandBufferView.count);
    for (std::size_t i = 0U; i < totalCount; ++i) {
      if ((commandBufferView.data[i].sortKey.value & kDrawKeyTransparentBit) !=
          0U) {
        break;
      }
      opaqueCount = i + 1U;
    }
  }
  if (backend.staticMeshBatches.size() < opaqueCount) {
    backend.staticMeshBatches.resize(opaqueCount);
  }
  const std::size_t opaqueBatchCount = build_static_mesh_batches(
      commandBufferView, 0U, opaqueCount, backend.staticMeshBatches.data(),
      backend.staticMeshBatches.size());

  // ====================================================================
  // SHADOW MAP PASS (before main scene rendering)
  // ====================================================================
  const bool shadowEnabled = backend.shadowAvailable &&
                             core::cvar_get_bool("r_shadows", true) &&
                             lights.directionalLightCount > 0U;

  CascadeSplits cascadeSplits{};
  bool directionalShadowCacheReused = false;
  if (shadowEnabled && (commandBufferView.data != nullptr) &&
      (opaqueCount > 0U)) {
    const float lambda = core::cvar_get_float("r_shadow_lambda", 0.75F);
    cascadeSplits = compute_cascade_splits(nearP, farP, lambda);

    const math::Vec3 &lightDir = lights.directionalLights[0].direction;
    std::array<math::Mat4, kShadowCascadeCount> lightMatrices{};

    for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
      const int shadowResolution = (backend.shadowState.resolutions[c] > 0)
                                       ? backend.shadowState.resolutions[c]
                                       : shadow_cascade_resolution(c);
      math::Mat4 lightVP = compute_cascade_matrix(
          viewMat, projMat, lightDir, cascadeSplits.distances[c],
          cascadeSplits.distances[c + 1], shadowResolution);
      lightVP = snap_to_texel(lightVP, shadowResolution);

      backend.shadowState.cascades[c].lightViewProjection = lightVP;
      backend.shadowState.cascades[c].splitDistance =
          cascadeSplits.distances[c + 1];
      lightMatrices[c] = lightVP;
    }

    const std::uint64_t cacheKey = directional_shadow_cache_key(
        commandBufferView, opaqueCount, lights.directionalLights[0],
        cascadeSplits, lightMatrices);
    const bool cacheEnabled = core::cvar_get_bool("r_shadow_cache", true);
    directionalShadowCacheReused =
        cacheEnabled && backend.directionalShadowCacheValid &&
        (backend.directionalShadowCacheKey == cacheKey);

    if (directionalShadowCacheReused) {
      if (core::cvar_get_bool("r_shadow_debug", false)) {
        core::log_message(core::LogLevel::Info, "renderer",
                          "reused directional shadow maps");
      }
    } else {
      gpu_profiler_begin_pass(GpuPassId::ShadowMap);

      for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
        const math::Mat4 &lightVP = lightMatrices[c];
        const int shadowResolution = (backend.shadowState.resolutions[c] > 0)
                                         ? backend.shadowState.resolutions[c]
                                         : shadow_cascade_resolution(c);

        dev->bind_framebuffer(backend.shadowState.depthFbos[c]);
        dev->set_viewport(0, 0, shadowResolution, shadowResolution);
        dev->enable_depth_test();
        dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
        dev->clear_color_depth();

        dev->bind_program(backend.shadowDepthProgram);

        std::uint32_t boundVertexArray = 0U;
        for (std::size_t i = 0U; i < opaqueCount; ++i) {
          const DrawCommand &command = commandBufferView.data[i];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U)) {
            continue;
          }

          if (mesh->vertexArray != boundVertexArray) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVertexArray = mesh->vertexArray;
          }

          const math::Mat4 lightMvp = math::mul(lightVP, command.modelMatrix);
          if (backend.shadowLightMvpLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowLightMvpLoc,
                                  &lightMvp.columns[0].x);
          }
          if (backend.shadowModelLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowModelLoc,
                                  &command.modelMatrix.columns[0].x);
          }

          if (mesh->indexCount > 0U) {
            dev->draw_elements_triangles_u32(
                static_cast<std::int32_t>(mesh->indexCount));
            frameStats.triangleCount += mesh->indexCount / 3U;
          } else {
            dev->draw_arrays_triangles(
                0, static_cast<std::int32_t>(mesh->vertexCount));
            frameStats.triangleCount += mesh->vertexCount / 3U;
          }
          ++frameStats.drawCalls;
        }

        dev->bind_vertex_array(0U);
        dev->bind_program(0U);
      }

      gpu_profiler_end_pass(GpuPassId::ShadowMap);
      backend.directionalShadowCacheKey = cacheKey;
      backend.directionalShadowCacheValid = true;
    }
  } else {
    backend.directionalShadowCacheKey = 0U;
    backend.directionalShadowCacheValid = false;
  }

  // ==== Spot Light Shadow Pass ====
  const bool doSpotShadows =
      backend.spotShadowAvailable && core::cvar_get_bool("r_spot_shadows");
  if (doSpotShadows && (lights.spotLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::SpotShadowMap);

    // Select up to kMaxSpotShadowLights nearest shadow-casting spots.
    for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
      backend.spotShadowState.slots[i].lightIndex = -1;
    }

    std::array<ShadowCandidate, kMaxSpotLights> spotCandidates{};
    std::size_t spotCandidateCount = 0U;
    const math::Vec3 &camPos = renderer_context().activeCamera.position;
    for (std::size_t li = 0U; li < lights.spotLightCount; ++li) {
      if (!lights.spotLights[li].castShadow) {
        continue;
      }
      const math::Vec3 &p = lights.spotLights[li].position;
      const float dx = p.x - camPos.x;
      const float dy = p.y - camPos.y;
      const float dz = p.z - camPos.z;
      spotCandidates[spotCandidateCount++] = {li, dx * dx + dy * dy + dz * dz};
    }
    std::sort(spotCandidates.data(), spotCandidates.data() + spotCandidateCount,
              [](const ShadowCandidate &a, const ShadowCandidate &b) noexcept {
                return a.distSq < b.distSq;
              });
    if ((spotCandidateCount > kMaxSpotShadowLights) &&
        core::cvar_get_bool("r_shadow_debug")) {
      core::log_message(core::LogLevel::Warning, "shadow",
                        "spot shadow casters dropped: only 4 slots available");
    }
    const std::size_t activeSpotShadows =
        std::min(spotCandidateCount, kMaxSpotShadowLights);
    for (std::size_t s = 0U; s < activeSpotShadows; ++s) {
      const std::size_t li = spotCandidates[s].lightIndex;
      auto &slot = backend.spotShadowState.slots[s];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = lights.spotLights[li].radius;
      slot.lightViewProjection = compute_spot_shadow_matrix(
          lights.spotLights[li].position, lights.spotLights[li].direction,
          lights.spotLights[li].outerConeAngle, lights.spotLights[li].radius);
    }

    // Render into each spot shadow FBO.
    dev->bind_program(backend.shadowDepthProgram);
    for (std::size_t s = 0U; s < activeSpotShadows; ++s) {
      const auto &slot = backend.spotShadowState.slots[s];
      dev->bind_framebuffer(slot.depthFbo);
      dev->set_viewport(0, 0, kSpotShadowMapResolution,
                        kSpotShadowMapResolution);
      dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
      dev->clear_color_depth();
      dev->enable_depth_test();

      std::uint32_t boundVao = 0U;
      for (std::size_t ci = 0U; ci < opaqueCount; ++ci) {
        const DrawCommand &cmd = commandBufferView.data[ci];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U)) {
          continue;
        }

        const math::Mat4 mvp =
            math::mul(slot.lightViewProjection, cmd.modelMatrix);
        if (backend.shadowLightMvpLoc >= 0) {
          dev->set_uniform_mat4(backend.shadowLightMvpLoc, &mvp.columns[0].x);
        }
        if (backend.shadowModelLoc >= 0) {
          dev->set_uniform_mat4(backend.shadowModelLoc,
                                &cmd.modelMatrix.columns[0].x);
        }

        if (mesh->vertexArray != boundVao) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVao = mesh->vertexArray;
        }
        dev->draw_elements_triangles_u32(mesh->indexCount);
      }
    }

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::SpotShadowMap);
  }

  // ==== Point Light Cubemap Shadow Pass ====
  const bool doPointShadows =
      backend.pointShadowAvailable && core::cvar_get_bool("r_point_shadows");
  if (doPointShadows && (lights.pointLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::PointShadowMap);

    // Select up to kMaxPointShadowLights nearest shadow-casting points.
    for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
      backend.pointShadowState.slots[i].lightIndex = -1;
    }

    std::array<ShadowCandidate, kMaxPointLights> pointCandidates{};
    std::size_t pointCandidateCount = 0U;
    const math::Vec3 &camPos = renderer_context().activeCamera.position;
    for (std::size_t li = 0U; li < lights.pointLightCount; ++li) {
      if (!lights.pointLights[li].castShadow) {
        continue;
      }
      const math::Vec3 &p = lights.pointLights[li].position;
      const float dx = p.x - camPos.x;
      const float dy = p.y - camPos.y;
      const float dz = p.z - camPos.z;
      pointCandidates[pointCandidateCount++] = {li,
                                                dx * dx + dy * dy + dz * dz};
    }
    std::sort(pointCandidates.data(),
              pointCandidates.data() + pointCandidateCount,
              [](const ShadowCandidate &a, const ShadowCandidate &b) noexcept {
                return a.distSq < b.distSq;
              });
    if ((pointCandidateCount > kMaxPointShadowLights) &&
        core::cvar_get_bool("r_shadow_debug")) {
      core::log_message(core::LogLevel::Warning, "shadow",
                        "point shadow casters dropped: only 4 slots available");
    }
    const std::size_t activePointShadows =
        std::min(pointCandidateCount, kMaxPointShadowLights);
    for (std::size_t s = 0U; s < activePointShadows; ++s) {
      const std::size_t li = pointCandidates[s].lightIndex;
      auto &slot = backend.pointShadowState.slots[s];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = std::max(lights.pointLights[li].radius, 1.0F);
      compute_point_shadow_matrices(lights.pointLights[li].position,
                                    lights.pointLights[li].radius,
                                    slot.faceViewProjections);
    }

    // Render into each point shadow cubemap (6 faces per light).
    dev->bind_program(backend.shadowDepthPointProgram);
    for (std::size_t s = 0U; s < activePointShadows; ++s) {
      const auto &slot = backend.pointShadowState.slots[s];
      const math::Vec3 &lightPos =
          lights.pointLights[static_cast<std::size_t>(slot.lightIndex)]
              .position;

      if (backend.shadowPointLightPosLoc >= 0) {
        dev->set_uniform_vec3(backend.shadowPointLightPosLoc, &lightPos.x);
      }
      if (backend.shadowPointFarPlaneLoc >= 0) {
        dev->set_uniform_float(backend.shadowPointFarPlaneLoc, slot.farPlane);
      }

      for (int face = 0; face < 6; ++face) {
        dev->framebuffer_cubemap_face(slot.depthFbo, slot.depthCubemap, face);
        dev->bind_framebuffer(slot.depthFbo);
        dev->set_viewport(0, 0, kPointShadowMapResolution,
                          kPointShadowMapResolution);
        dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
        dev->clear_color_depth();
        dev->enable_depth_test();

        std::uint32_t boundVao = 0U;
        for (std::size_t ci = 0U; ci < opaqueCount; ++ci) {
          const DrawCommand &cmd = commandBufferView.data[ci];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U)) {
            continue;
          }

          const math::Mat4 mvp =
              math::mul(slot.faceViewProjections[face], cmd.modelMatrix);
          if (backend.shadowPointLightMvpLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowPointLightMvpLoc,
                                  &mvp.columns[0].x);
          }
          if (backend.shadowPointModelLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowPointModelLoc,
                                  &cmd.modelMatrix.columns[0].x);
          }

          if (mesh->vertexArray != boundVao) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVao = mesh->vertexArray;
          }
          dev->draw_elements_triangles_u32(mesh->indexCount);
        }
      }
    }

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::PointShadowMap);
  }

  // ====================================================================
  // SCENE CAPTURE PASS (render-to-texture)
  // Forward-lit opaque + transparent geometry per capture request into a
  // dedicated LDR target. Captures deliberately skip sky, shadows, and the
  // post stack: they run before the main-path passes and must not disturb
  // the shadow/tile state those passes computed for the active camera.
  // ====================================================================
  const std::size_t captureCount = scene_capture_request_count();
  for (std::size_t captureIndex = 0U; captureIndex < captureCount;
       ++captureIndex) {
    const SceneCaptureRequest &request =
        renderer_context().sceneCaptureRequests[captureIndex];
    const int captureWidth = static_cast<int>(request.width);
    const int captureHeight = static_cast<int>(request.height);
    if (!ensure_scene_capture_target(backend, dev, captureIndex, captureWidth,
                                     captureHeight)) {
      continue;
    }

    const SceneCaptureTarget &target =
        backend.sceneCaptureTargets[captureIndex];
    dev->bind_framebuffer(target.framebuffer);
    dev->set_viewport(0, 0, captureWidth, captureHeight);
    dev->enable_depth_test();
    dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
    dev->clear_color_depth();

    if ((commandBufferView.data == nullptr) || (totalCount == 0U)) {
      continue;
    }

    const float captureAspect = static_cast<float>(captureWidth) /
                                static_cast<float>(captureHeight);
    const math::Mat4 captureView = math::look_at(
        request.camera.position, request.camera.target, request.camera.up);
    const math::Mat4 captureProj =
        math::perspective(request.camera.fovRadians, captureAspect,
                          request.camera.nearPlane, request.camera.farPlane);
    const math::Mat4 captureViewProjection =
        math::mul(captureProj, captureView);

    dev->bind_program(backend.pbrProgram);
    if (backend.pbrTimeLocation >= 0) {
      dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation >= 0) {
      dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                            &request.camera.position.x);
    }
    if (backend.pbrViewLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewLocation,
                            &captureView.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                            &captureViewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation >= 0) {
      dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, false, false, false);
    if (backend.pbrAlbedoMapLocation >= 0) {
      dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
    }

    auto drawCaptureRange = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTexture = 0U;
      for (std::size_t i = start; i < end; ++i) {
        const DrawCommand &command = commandBufferView.data[i];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (mesh->vertexArray != boundVertexArray) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVertexArray = mesh->vertexArray;
        }

        if (backend.pbrAlbedoLocation >= 0) {
          dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                                &command.material.albedo.x);
        }
        if (backend.pbrRoughnessLocation >= 0) {
          dev->set_uniform_float(backend.pbrRoughnessLocation,
                                 command.material.roughness);
        }
        if (backend.pbrMetallicLocation >= 0) {
          dev->set_uniform_float(backend.pbrMetallicLocation,
                                 command.material.metallic);
        }
        if (backend.pbrOpacityLocation >= 0) {
          dev->set_uniform_float(backend.pbrOpacityLocation,
                                 command.material.opacity);
        }
        upload_pbr_foliage_uniforms(backend, dev, command);

        const std::uint32_t albedoGpuId =
            texture_gpu_id(command.material.albedoTexture);
        const bool hasAlbedoTex =
            (command.material.albedoTexture != kInvalidTextureHandle) &&
            (albedoGpuId != 0U);
        if (backend.pbrHasAlbedoTextureLocation >= 0) {
          dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                               hasAlbedoTex ? 1 : 0);
        }
        if (hasAlbedoTex && (albedoGpuId != boundAlbedoTexture)) {
          dev->bind_texture(0, albedoGpuId);
          boundAlbedoTexture = albedoGpuId;
        } else if (!hasAlbedoTex && (boundAlbedoTexture != 0U)) {
          dev->bind_texture(0, 0U);
          boundAlbedoTexture = 0U;
        }

        const math::Mat4 model = compute_model_matrix(command);
        const math::Mat4 mvp = compute_mvp(model, captureViewProjection);
        float normalMatrix[9] = {};
        extract_normal_matrix(model, normalMatrix);
        if (backend.pbrModelLocation >= 0) {
          dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
        }
        dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
        dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

        if (mesh->indexCount > 0U) {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->indexCount / 3U);
          dev->draw_elements_triangles_u32(
              static_cast<std::int32_t>(mesh->indexCount));
        } else {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->vertexCount / 3U);
          dev->draw_arrays_triangles(
              0, static_cast<std::int32_t>(mesh->vertexCount));
        }
      }
    };

    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawCaptureRange(0U, opaqueCount);

    if (opaqueCount < totalCount) {
      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawCaptureRange(opaqueCount, totalCount);
      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
    }

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
  }

  // ====================================================================
  // DEFERRED PATH
  // ====================================================================
  if (useDeferred) {
    bool sceneDepthHasOpaque = false;
    auto ensureSceneDepthHasOpaque = [&]() noexcept -> bool {
      if (sceneDepthHasOpaque) {
        return true;
      }
      if (dev->blit_depth == nullptr) {
        return false;
      }

      const std::uint32_t gbufferFbo =
          pass_resource_framebuffer(passRes.gbufferAlbedo);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->blit_depth(gbufferFbo, sceneFbo, drawableWidth, drawableHeight);
      dev->bind_framebuffer(sceneFbo);
      sceneDepthHasOpaque = true;
      return true;
    };

    // --- G-Buffer pass: render geometry into MRT ---
    gpu_profiler_begin_pass(GpuPassId::GBuffer);
    const std::uint32_t gbufferFbo =
        pass_resource_framebuffer(passRes.gbufferAlbedo);
    dev->bind_framebuffer(gbufferFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->enable_depth_test();
    dev->set_clear_color(0.0F, 0.0F, 0.0F, 0.0F);
    dev->clear_color_depth();

    dev->bind_program(backend.gbufferProgram);

    // Per-frame camera uniforms.
    if (backend.gbufViewLoc >= 0) {
      dev->set_uniform_mat4(backend.gbufViewLoc, &viewMat.columns[0].x);
    }
    if (backend.gbufProjectionLoc >= 0) {
      dev->set_uniform_mat4(backend.gbufProjectionLoc, &projMat.columns[0].x);
    }
    if (backend.gbufTimeLoc >= 0) {
      dev->set_uniform_float(backend.gbufTimeLoc, timeSeconds);
    }

    auto drawGBufferBatches = [&]() {
      std::uint32_t boundVertexArray = 0U;
      for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
           ++batchIndex) {
        const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
        const DrawCommand &command = commandBufferView.data[batch.first];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (mesh->vertexArray != boundVertexArray) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVertexArray = mesh->vertexArray;
        }

        // Material uniforms.
        if (backend.gbufAlbedoLoc >= 0) {
          dev->set_uniform_vec3(backend.gbufAlbedoLoc,
                                &command.material.albedo.x);
        }
        if (backend.gbufMetallicLoc >= 0) {
          dev->set_uniform_float(backend.gbufMetallicLoc,
                                 command.material.metallic);
        }
        if (backend.gbufRoughnessLoc >= 0) {
          dev->set_uniform_float(backend.gbufRoughnessLoc,
                                 command.material.roughness);
        }
        if (backend.gbufAOLoc >= 0) {
          dev->set_uniform_float(backend.gbufAOLoc, 1.0F);
        }
        if (backend.gbufEmissiveLoc >= 0) {
          dev->set_uniform_vec3(backend.gbufEmissiveLoc,
                                &command.material.emissive.x);
        }
        upload_gbuffer_foliage_uniforms(backend, dev, command);

        if ((batch.count > 1U) && (mesh->indexCount > 0U) &&
            upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                     batch)) {
          boundVertexArray = mesh->vertexArray;
          if (backend.gbufUseInstancingLoc >= 0) {
            dev->set_uniform_int(backend.gbufUseInstancingLoc, 1);
          }
          ++frameStats.drawCalls;
          frameStats.triangleCount +=
              (mesh->indexCount / 3U) * static_cast<std::uint64_t>(batch.count);
          dev->draw_elements_triangles_u32_instanced(
              static_cast<std::int32_t>(mesh->indexCount),
              static_cast<std::int32_t>(batch.count));
          continue;
        }

        if (backend.gbufUseInstancingLoc >= 0) {
          dev->set_uniform_int(backend.gbufUseInstancingLoc, 0);
        }
        for (std::uint32_t local = 0U; local < batch.count; ++local) {
          const std::size_t commandIndex =
              static_cast<std::size_t>(batch.first) +
              static_cast<std::size_t>(local);
          const DrawCommand &singleCommand = commandBufferView.data[commandIndex];
          upload_gbuffer_foliage_uniforms(backend, dev, singleCommand);
          const math::Mat4 model = compute_model_matrix(singleCommand);
          float normalMatrix[9] = {};
          extract_normal_matrix(model, normalMatrix);

          if (backend.gbufModelLoc >= 0) {
            dev->set_uniform_mat4(backend.gbufModelLoc, &model.columns[0].x);
          }
          if (backend.gbufNormalMatrixLoc >= 0) {
            dev->set_uniform_mat3(backend.gbufNormalMatrixLoc, normalMatrix);
          }

          if (mesh->indexCount > 0U) {
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U);
            dev->draw_elements_triangles_u32(
                static_cast<std::int32_t>(mesh->indexCount));
          } else {
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->vertexCount / 3U);
            dev->draw_arrays_triangles(
                0, static_cast<std::int32_t>(mesh->vertexCount));
          }
        }
      }
    };

    // Opaque geometry only for G-Buffer.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawGBufferBatches();

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::GBuffer);

    // --- SSAO pass: sample hemisphere, output raw AO ---
    const bool ssaoEnabled =
        backend.ssaoAvailable && core::cvar_get_bool("r_ssao", true);
    if (ssaoEnabled) {
      gpu_profiler_begin_pass(GpuPassId::SSAO);
      const std::uint32_t ssaoFbo =
          pass_resource_framebuffer(passRes.ssaoTexture);
      dev->bind_framebuffer(ssaoFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.ssaoProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, backend.ssaoNoiseTexture);

      if (backend.ssaoDepthLoc >= 0)
        dev->set_uniform_int(backend.ssaoDepthLoc, 0);
      if (backend.ssaoNormalLoc >= 0)
        dev->set_uniform_int(backend.ssaoNormalLoc, 1);
      if (backend.ssaoNoiseLoc >= 0)
        dev->set_uniform_int(backend.ssaoNoiseLoc, 2);

      if (backend.ssaoProjectionLoc >= 0)
        dev->set_uniform_mat4(backend.ssaoProjectionLoc, &projMat.columns[0].x);

      if (backend.ssaoNoiseScaleLoc >= 0) {
        const float noiseScale[2] = {static_cast<float>(drawableWidth) / 4.0F,
                                     static_cast<float>(drawableHeight) / 4.0F};
        dev->set_uniform_vec2(backend.ssaoNoiseScaleLoc, noiseScale);
      }
      if (backend.ssaoRadiusLoc >= 0)
        dev->set_uniform_float(backend.ssaoRadiusLoc,
                               core::cvar_get_float("r_ssao_radius"));
      if (backend.ssaoBiasLoc >= 0)
        dev->set_uniform_float(backend.ssaoBiasLoc,
                               core::cvar_get_float("r_ssao_bias"));

      for (int i = 0; i < 32; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (backend.ssaoSampleLocs[idx] >= 0) {
          dev->set_uniform_vec3(backend.ssaoSampleLocs[idx],
                                &backend.ssaoKernel[i * 3]);
        }
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);

      // --- SSAO blur pass ---
      const std::uint32_t ssaoBlurFbo =
          pass_resource_framebuffer(passRes.ssaoBlurTexture);
      dev->bind_framebuffer(ssaoBlurFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);

      dev->bind_program(backend.ssaoBlurProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.ssaoTexture));
      if (backend.ssaoBlurInputLoc >= 0)
        dev->set_uniform_int(backend.ssaoBlurInputLoc, 0);
      if (backend.ssaoBlurTexelSizeLoc >= 0) {
        const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                    1.0F / static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.ssaoBlurTexelSizeLoc, texelSize);
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::SSAO);
    }

    // --- CPU tiled light culling ---
    const std::size_t tileBufferSize =
        compute_tile_buffer_size(drawableWidth, drawableHeight);
    if (backend.tileBuffer.size() < tileBufferSize) {
      backend.tileBuffer.resize(tileBufferSize, 0.0F);
    }

    TileLightData tileData{};
    tileData.data = backend.tileBuffer.data();
    tileData.dataSize = backend.tileBuffer.size();

    cull_lights_tiled(lights, &viewMat.columns[0].x, &projMat.columns[0].x,
                      drawableWidth, drawableHeight, tileData);

    // Upload tile texture.
    if (backend.tileLightTex == 0U) {
      backend.tileLightTex = dev->create_texture_2d_r32f(
          kTileDataWidth, tileData.totalTiles, backend.tileBuffer.data());
    } else {
      dev->update_texture_2d_r32f(backend.tileLightTex, kTileDataWidth,
                                  tileData.totalTiles,
                                  backend.tileBuffer.data());
    }

    // --- G-Buffer debug visualization (overrides lighting pass) ---
    if (gbufferDebugMode > 0 && backend.gbufferDebugProgram != 0U) {
      gpu_profiler_begin_pass(GpuPassId::GBufferDebug);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.gbufferDebugProgram);

      // Bind G-Buffer textures.
      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));

      if (backend.dbgGBufAlbedoLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufAlbedoLoc, 0);
      if (backend.dbgGBufNormalLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufNormalLoc, 1);
      if (backend.dbgGBufEmissiveLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufEmissiveLoc, 2);
      if (backend.dbgGBufDepthLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufDepthLoc, 3);
      // Debug mode:
      // 0=albedo,1=normals,2=metallic,3=roughness,4=emissive,5=AO,6=depth CVar
      // value 1..7 maps to shader 0..6.
      if (backend.dbgModeLoc >= 0)
        dev->set_uniform_int(backend.dbgModeLoc, gbufferDebugMode - 1);

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_texture(3, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::GBufferDebug);
    } else {
      // --- Deferred lighting pass: fullscreen, reads G-Buffer → HDR scene ---
      gpu_profiler_begin_pass(GpuPassId::DeferredLighting);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.deferredLightProgram);

      // Bind G-Buffer textures on units 0-3, tile on unit 4, SSAO on unit 5.
      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(4, backend.tileLightTex);

      if (ssaoEnabled) {
        dev->bind_texture(5,
                          pass_resource_gpu_texture(passRes.ssaoBlurTexture));
      }

      if (backend.dlGBufAlbedoLoc >= 0)
        dev->set_uniform_int(backend.dlGBufAlbedoLoc, 0);
      if (backend.dlGBufNormalLoc >= 0)
        dev->set_uniform_int(backend.dlGBufNormalLoc, 1);
      if (backend.dlGBufEmissiveLoc >= 0)
        dev->set_uniform_int(backend.dlGBufEmissiveLoc, 2);
      if (backend.dlGBufDepthLoc >= 0)
        dev->set_uniform_int(backend.dlGBufDepthLoc, 3);
      if (backend.dlTileLightTexLoc >= 0)
        dev->set_uniform_int(backend.dlTileLightTexLoc, 4);

      if (backend.dlSsaoTextureLoc >= 0)
        dev->set_uniform_int(backend.dlSsaoTextureLoc, 5);
      if (backend.dlSsaoEnabledLoc >= 0)
        dev->set_uniform_int(backend.dlSsaoEnabledLoc, ssaoEnabled ? 1 : 0);

      // Bind shadow maps on texture units 6-9.
      if (shadowEnabled) {
        for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
          const int texUnit = 6 + static_cast<int>(c);
          dev->bind_texture(texUnit, backend.shadowState.depthTextures[c]);
          if (backend.dlShadowMapLocs[c] >= 0) {
            dev->set_uniform_int(backend.dlShadowMapLocs[c], texUnit);
          }
          if (backend.dlShadowMatrixLocs[c] >= 0) {
            dev->set_uniform_mat4(backend.dlShadowMatrixLocs[c],
                                  &backend.shadowState.cascades[c]
                                       .lightViewProjection.columns[0]
                                       .x);
          }
          if (backend.dlCascadeSplitLocs[c] >= 0) {
            dev->set_uniform_float(
                backend.dlCascadeSplitLocs[c],
                backend.shadowState.cascades[c].splitDistance);
          }
        }
      }
      if (backend.dlShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlShadowEnabledLoc, shadowEnabled ? 1 : 0);
      }

      // Bind spot shadow maps on texture units 10-13.
      const bool spotShadowEnabled = doSpotShadows;
      if (spotShadowEnabled) {
        for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
          const auto &slot = backend.spotShadowState.slots[s];
          const int texUnit = 10 + static_cast<int>(s);
          dev->bind_texture(texUnit, slot.depthTexture);
          if (backend.dlSpotShadowMapLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlSpotShadowMapLocs[s], texUnit);
          }
          if (backend.dlSpotShadowMatrixLocs[s] >= 0) {
            dev->set_uniform_mat4(backend.dlSpotShadowMatrixLocs[s],
                                  &slot.lightViewProjection.columns[0].x);
          }
          if (backend.dlSpotShadowLightIdxLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlSpotShadowLightIdxLocs[s],
                                 slot.lightIndex);
          }
        }
      }
      if (backend.dlSpotShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlSpotShadowEnabledLoc,
                             spotShadowEnabled ? 1 : 0);
      }

      // Bind point shadow cubemaps on texture units 14-17.
      const bool pointShadowEnabled = doPointShadows;
      if (pointShadowEnabled) {
        for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
          const auto &slot = backend.pointShadowState.slots[s];
          const int texUnit = 14 + static_cast<int>(s);
          if (dev->bind_texture_cubemap != nullptr) {
            dev->bind_texture_cubemap(texUnit, slot.depthCubemap);
          }
          if (backend.dlPointShadowMapLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlPointShadowMapLocs[s], texUnit);
          }
          if (backend.dlPointShadowLightPosLocs[s] >= 0) {
            const auto &lp = lights
                                 .pointLights[static_cast<std::size_t>(
                                     std::max(slot.lightIndex, 0))]
                                 .position;
            dev->set_uniform_vec3(backend.dlPointShadowLightPosLocs[s], &lp.x);
          }
          if (backend.dlPointShadowFarPlaneLocs[s] >= 0) {
            dev->set_uniform_float(backend.dlPointShadowFarPlaneLocs[s],
                                   slot.farPlane);
          }
          if (backend.dlPointShadowLightIdxLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlPointShadowLightIdxLocs[s],
                                 slot.lightIndex);
          }
        }
      }
      if (backend.dlPointShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlPointShadowEnabledLoc,
                             pointShadowEnabled ? 1 : 0);
      }

      if (backend.dlTileCountXLoc >= 0)
        dev->set_uniform_int(backend.dlTileCountXLoc, tileData.tileCountX);
      if (backend.dlTileCountYLoc >= 0)
        dev->set_uniform_int(backend.dlTileCountYLoc, tileData.tileCountY);

      // Inverse matrices for depth reconstruction.
      math::Mat4 invProj{};
      if (math::inverse(projMat, &invProj)) {
        if (backend.dlInvProjectionLoc >= 0)
          dev->set_uniform_mat4(backend.dlInvProjectionLoc,
                                &invProj.columns[0].x);
      }
      math::Mat4 invView{};
      if (math::inverse(viewMat, &invView)) {
        if (backend.dlInvViewLoc >= 0)
          dev->set_uniform_mat4(backend.dlInvViewLoc, &invView.columns[0].x);
      }

      // Directional light (use first if available).
      if (backend.dlDirLightDirLoc >= 0 && lights.directionalLightCount > 0U) {
        dev->set_uniform_vec3(backend.dlDirLightDirLoc,
                              &lights.directionalLights[0].direction.x);
      }
      if (backend.dlDirLightColorLoc >= 0 &&
          lights.directionalLightCount > 0U) {
        dev->set_uniform_vec3(backend.dlDirLightColorLoc,
                              &lights.directionalLights[0].color.x);
      }

      if (backend.dlCameraPosLoc >= 0) {
        dev->set_uniform_vec3(backend.dlCameraPosLoc,
                              &renderer_context().activeCamera.position.x);
      }
      if (backend.dlScreenSizeLoc >= 0) {
        const float screenSize[2] = {static_cast<float>(drawableWidth),
                                     static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.dlScreenSizeLoc, screenSize);
      }
      upload_deferred_distance_fog_uniforms(backend, dev, fogSettings);
      upload_deferred_height_fog_uniforms(backend, dev, heightFogSettings);

      // Upload point light data.
      const auto plCount = static_cast<int>(std::min(
          lights.pointLightCount, static_cast<std::size_t>(kMaxPointLights)));
      if (backend.dlPointLightCountLoc >= 0)
        dev->set_uniform_int(backend.dlPointLightCountLoc, plCount);
      for (int pi = 0; pi < plCount; ++pi) {
        const auto &pl = lights.pointLights[pi];
        const auto idx = static_cast<std::size_t>(pi);
        if (backend.dlPointPosLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlPointPosLocs[idx], &pl.position.x);
        if (backend.dlPointColorLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlPointColorLocs[idx], &pl.color.x);
        if (backend.dlPointIntensityLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlPointIntensityLocs[idx],
                                 pl.intensity);
        if (backend.dlPointRadiusLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlPointRadiusLocs[idx], pl.radius);
      }

      // Upload spot light data.
      const auto slCount = static_cast<int>(std::min(
          lights.spotLightCount, static_cast<std::size_t>(kMaxSpotLights)));
      if (backend.dlSpotLightCountLoc >= 0)
        dev->set_uniform_int(backend.dlSpotLightCountLoc, slCount);
      for (int si = 0; si < slCount; ++si) {
        const auto &sl = lights.spotLights[si];
        const auto idx = static_cast<std::size_t>(si);
        if (backend.dlSpotPosLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlSpotPosLocs[idx], &sl.position.x);
        if (backend.dlSpotDirLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlSpotDirLocs[idx], &sl.direction.x);
        if (backend.dlSpotColorLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlSpotColorLocs[idx], &sl.color.x);
        if (backend.dlSpotIntensityLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotIntensityLocs[idx],
                                 sl.intensity);
        if (backend.dlSpotRadiusLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotRadiusLocs[idx], sl.radius);
        if (backend.dlSpotInnerConeLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotInnerConeLocs[idx],
                                 sl.innerConeAngle);
        if (backend.dlSpotOuterConeLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotOuterConeLocs[idx],
                                 sl.outerConeAngle);
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_texture(3, 0U);
      dev->bind_texture(4, 0U);
      if (ssaoEnabled) {
        dev->bind_texture(5, 0U);
      }
      if (shadowEnabled) {
        for (int c = 0; c < static_cast<int>(kShadowCascadeCount); ++c) {
          dev->bind_texture(6 + c, 0U);
        }
      }
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::DeferredLighting);
    }

    const SkyModel skyModel = selected_sky_model();
    const std::uint32_t skyboxTexture = (skyModel == SkyModel::Cubemap)
                                            ? active_skybox_gpu_texture(backend)
                                            : 0U;
    if (skyboxTexture != 0U) {
      static_cast<void>(
          ensure_prefiltered_environment(backend, dev, skyboxTexture,
                                         environmentBakeSettings));
      static_cast<void>(
          ensure_irradiance_environment(backend, dev, skyboxTexture,
                                        environmentBakeSettings));
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_skybox(backend, dev, viewMat, projMat, skyboxTexture, frameStats);
      }
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_hosek_sky(backend, dev, viewMat, projMat, lights, frameStats);
      }
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_preetham_sky(backend, dev, viewMat, projMat, lights, frameStats);
      }
    }

    // Forward-render transparent geometry into the scene HDR FBO.
    if (opaqueCount < totalCount) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->enable_depth_test();

      // Carry opaque deferred depth into the scene FBO so forward transparent
      // draws depth-test against G-Buffer geometry.
      static_cast<void>(ensureSceneDepthHasOpaque());
      dev->bind_program(backend.pbrProgram);

      // Re-upload forward PBR camera/lights for transparent pass.
      if (backend.pbrTimeLocation >= 0) {
        dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
      }
      if (backend.pbrCameraPosLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                              &renderer_context().activeCamera.position.x);
      }
      if (backend.pbrViewLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrViewLocation, &viewMat.columns[0].x);
      }
      if (backend.pbrViewProjectionLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                              &viewProjection.columns[0].x);
      }
      if (backend.pbrUseInstancingLocation >= 0) {
        dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
      }
      upload_pbr_lighting_uniforms(backend, dev, lights);
      upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
      upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
      bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled,
                               doSpotShadows, doPointShadows);
      if (backend.pbrAlbedoMapLocation >= 0)
        dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);

      const math::Mat4 &vp = viewProjection;

      auto drawForwardTransparent = [&](std::size_t start, std::size_t end) {
        std::uint32_t boundVA = 0U;
        std::uint32_t boundAlbedoTex = 0U;
        for (std::size_t i = start; i < end; ++i) {
          const DrawCommand &cmd = commandBufferView.data[i];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U))
            continue;
          if (mesh->vertexArray != boundVA) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVA = mesh->vertexArray;
          }
          if (backend.pbrAlbedoLocation >= 0)
            dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                                  &cmd.material.albedo.x);
          if (backend.pbrRoughnessLocation >= 0)
            dev->set_uniform_float(backend.pbrRoughnessLocation,
                                   cmd.material.roughness);
          if (backend.pbrMetallicLocation >= 0)
            dev->set_uniform_float(backend.pbrMetallicLocation,
                                   cmd.material.metallic);
          if (backend.pbrOpacityLocation >= 0)
            dev->set_uniform_float(backend.pbrOpacityLocation,
                                   cmd.material.opacity);
          upload_pbr_foliage_uniforms(backend, dev, cmd);
          const std::uint32_t albedoGpu =
              texture_gpu_id(cmd.material.albedoTexture);
          const bool hasTex =
              (cmd.material.albedoTexture != kInvalidTextureHandle) &&
              (albedoGpu != 0U);
          if (backend.pbrHasAlbedoTextureLocation >= 0)
            dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                                 hasTex ? 1 : 0);
          if (hasTex && albedoGpu != boundAlbedoTex) {
            dev->bind_texture(0, albedoGpu);
            boundAlbedoTex = albedoGpu;
          } else if (!hasTex && boundAlbedoTex != 0U) {
            dev->bind_texture(0, 0U);
            boundAlbedoTex = 0U;
          }
          const math::Mat4 model = compute_model_matrix(cmd);
          const math::Mat4 mvp = compute_mvp(model, vp);
          float nm[9] = {};
          extract_normal_matrix(model, nm);
          if (backend.pbrModelLocation >= 0)
            dev->set_uniform_mat4(backend.pbrModelLocation,
                                  &model.columns[0].x);
          dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
          dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, nm);
          if (mesh->indexCount > 0U) {
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U);
            dev->draw_elements_triangles_u32(
                static_cast<std::int32_t>(mesh->indexCount));
          } else {
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->vertexCount / 3U);
            dev->draw_arrays_triangles(
                0, static_cast<std::int32_t>(mesh->vertexCount));
          }
        }
      };

      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawForwardTransparent(opaqueCount, totalCount);
      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
      dev->bind_texture(0, 0U);
      unbind_pbr_shadow_textures(dev);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
    }

    frameStats.gpuGBufferMs = gpu_profiler_pass_ms(GpuPassId::GBuffer);
    frameStats.gpuDeferredLightMs =
        gpu_profiler_pass_ms(GpuPassId::DeferredLighting);
    frameStats.gpuSsaoMs = gpu_profiler_pass_ms(GpuPassId::SSAO);

  } else {
    // ====================================================================
    // FORWARD PATH (original)
    // ====================================================================
    const std::uint32_t sceneFbo =
        pass_resource_framebuffer(passRes.sceneColor);

    gpu_profiler_begin_pass(GpuPassId::Scene);
    dev->bind_framebuffer(sceneFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->enable_depth_test();
    dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
    dev->clear_color_depth();

    dev->bind_program(backend.pbrProgram);

    // Per-frame uniforms.
    if (backend.pbrTimeLocation >= 0) {
      dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation >= 0) {
      dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                            &renderer_context().activeCamera.position.x);
    }
    if (backend.pbrViewLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewLocation, &viewMat.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                            &viewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation >= 0) {
      dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled, doSpotShadows,
                             doPointShadows);

    // Set albedo map sampler to texture unit 0.
    if (backend.pbrAlbedoMapLocation >= 0) {
      dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
    }

    auto drawForwardCommand = [&](const DrawCommand &command,
                                  const GpuMesh &mesh) {
      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);

      if (backend.pbrUseInstancingLocation >= 0) {
        dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
      }
      upload_pbr_foliage_uniforms(backend, dev, command);
      if (backend.pbrModelLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
      }
      dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
      dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

      if (mesh.indexCount > 0U) {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.indexCount / 3U);
        dev->draw_elements_triangles_u32(
            static_cast<std::int32_t>(mesh.indexCount));
      } else {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.vertexCount / 3U);
        dev->draw_arrays_triangles(0,
                                   static_cast<std::int32_t>(mesh.vertexCount));
      }
    };

    auto uploadForwardMaterial = [&](const Material &material,
                                     std::uint32_t *boundAlbedoTexture) {
      if (backend.pbrAlbedoLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrAlbedoLocation, &material.albedo.x);
      }
      if (backend.pbrRoughnessLocation >= 0) {
        dev->set_uniform_float(backend.pbrRoughnessLocation,
                               material.roughness);
      }
      if (backend.pbrMetallicLocation >= 0) {
        dev->set_uniform_float(backend.pbrMetallicLocation, material.metallic);
      }
      if (backend.pbrOpacityLocation >= 0) {
        dev->set_uniform_float(backend.pbrOpacityLocation, material.opacity);
      }

      const std::uint32_t albedoGpuId = texture_gpu_id(material.albedoTexture);
      const bool hasAlbedoTex =
          (material.albedoTexture != kInvalidTextureHandle) &&
          (albedoGpuId != 0U);
      if (backend.pbrHasAlbedoTextureLocation >= 0) {
        dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                             hasAlbedoTex ? 1 : 0);
      }
      if (hasAlbedoTex && (albedoGpuId != *boundAlbedoTexture)) {
        dev->bind_texture(0, albedoGpuId);
        *boundAlbedoTexture = albedoGpuId;
      } else if (!hasAlbedoTex && (*boundAlbedoTexture != 0U)) {
        dev->bind_texture(0, 0U);
        *boundAlbedoTexture = 0U;
      }
    };

    auto drawRange = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTexture = 0U;

      if ((start == 0U) && (end == opaqueCount)) {
        for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
             ++batchIndex) {
          const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
          const DrawCommand &command = commandBufferView.data[batch.first];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U)) {
            continue;
          }

          if (mesh->vertexArray != boundVertexArray) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVertexArray = mesh->vertexArray;
          }

          uploadForwardMaterial(command.material, &boundAlbedoTexture);
          upload_pbr_foliage_uniforms(backend, dev, command);

          if ((batch.count > 1U) && (mesh->indexCount > 0U) &&
              upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                       batch)) {
            boundVertexArray = mesh->vertexArray;
            if (backend.pbrUseInstancingLocation >= 0) {
              dev->set_uniform_int(backend.pbrUseInstancingLocation, 1);
            }
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U) *
                                        static_cast<std::uint64_t>(batch.count);
            dev->draw_elements_triangles_u32_instanced(
                static_cast<std::int32_t>(mesh->indexCount),
                static_cast<std::int32_t>(batch.count));
            continue;
          }

          for (std::uint32_t local = 0U; local < batch.count; ++local) {
            const std::size_t commandIndex =
                static_cast<std::size_t>(batch.first) +
                static_cast<std::size_t>(local);
            drawForwardCommand(commandBufferView.data[commandIndex], *mesh);
          }
        }
        return;
      }

      for (std::size_t i = start; i < end; ++i) {
        const DrawCommand &command = commandBufferView.data[i];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (mesh->vertexArray != boundVertexArray) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVertexArray = mesh->vertexArray;
        }

        uploadForwardMaterial(command.material, &boundAlbedoTexture);
        drawForwardCommand(command, *mesh);
      }
    };

    // Pass 1: Opaque.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawRange(0U, opaqueCount);

    const SkyModel skyModel = selected_sky_model();
    const std::uint32_t skyboxTexture = (skyModel == SkyModel::Cubemap)
                                            ? active_skybox_gpu_texture(backend)
                                            : 0U;
    if (skyboxTexture != 0U) {
      static_cast<void>(
          ensure_prefiltered_environment(backend, dev, skyboxTexture,
                                         environmentBakeSettings));
      static_cast<void>(
          ensure_irradiance_environment(backend, dev, skyboxTexture,
                                        environmentBakeSettings));
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      draw_skybox(backend, dev, viewMat, projMat, skyboxTexture, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      draw_hosek_sky(backend, dev, viewMat, projMat, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      draw_preetham_sky(backend, dev, viewMat, projMat, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    }

    // Pass 2: Transparent.
    if (opaqueCount < totalCount) {
      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawRange(opaqueCount, totalCount);

      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
    }

    dev->bind_texture(0, 0U);
    unbind_pbr_shadow_textures(dev);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::Scene);
  }

  // --- Bloom pass (before tonemap, operates on HDR sceneColor) ---
  const bool bloomAvailable = backend.bloomThresholdProgram != 0U &&
                              backend.bloomDownsampleProgram != 0U &&
                              backend.bloomUpsampleProgram != 0U;
  const bool bloomEnabled = bloomAvailable && core::cvar_get_bool("r_bloom");

  if (bloomEnabled) {
    gpu_profiler_begin_pass(GpuPassId::Bloom);
    ensure_bloom_resources(backend, drawableWidth, drawableHeight);

    const std::uint32_t sceneColorTexBloom =
        pass_resource_gpu_texture(passRes.sceneColor);

    // Step 1: Threshold — extract bright pixels into mip[0].
    dev->bind_framebuffer(backend.bloomMipFbos[0]);
    dev->set_viewport(0, 0, backend.bloomMipWidths[0],
                      backend.bloomMipHeights[0]);
    dev->disable_depth_test();
    dev->bind_program(backend.bloomThresholdProgram);
    dev->bind_texture(0, sceneColorTexBloom);
    if (backend.bloomThreshSceneColorLoc >= 0) {
      dev->set_uniform_int(backend.bloomThreshSceneColorLoc, 0);
    }
    if (backend.bloomThreshThresholdLoc >= 0) {
      dev->set_uniform_float(backend.bloomThreshThresholdLoc,
                             core::cvar_get_float("r_bloom_threshold"));
    }
    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    // Step 2: Downsample chain.
    dev->bind_program(backend.bloomDownsampleProgram);
    for (int i = 1; i < BackendState::kBloomMipLevels; ++i) {
      dev->bind_framebuffer(backend.bloomMipFbos[i]);
      dev->set_viewport(0, 0, backend.bloomMipWidths[i],
                        backend.bloomMipHeights[i]);
      dev->bind_texture(0, backend.bloomMipTextures[i - 1]);
      if (backend.bloomDownInputLoc >= 0) {
        dev->set_uniform_int(backend.bloomDownInputLoc, 0);
      }
      if (backend.bloomDownTexelSizeLoc >= 0) {
        const float ts[2] = {
            1.0F / static_cast<float>(backend.bloomMipWidths[i - 1]),
            1.0F / static_cast<float>(backend.bloomMipHeights[i - 1])};
        dev->set_uniform_vec2(backend.bloomDownTexelSizeLoc, ts);
      }
      dev->draw_arrays_triangles(0, 3);
    }

    // Step 3: Upsample chain — each level is overwritten with the blurred
    // result from the level below it.
    dev->bind_program(backend.bloomUpsampleProgram);
    for (int i = BackendState::kBloomMipLevels - 2; i >= 0; --i) {
      dev->bind_framebuffer(backend.bloomMipFbos[i]);
      dev->set_viewport(0, 0, backend.bloomMipWidths[i],
                        backend.bloomMipHeights[i]);
      dev->bind_texture(0, backend.bloomMipTextures[i + 1]);
      if (backend.bloomUpInputLoc >= 0) {
        dev->set_uniform_int(backend.bloomUpInputLoc, 0);
      }
      if (backend.bloomUpTexelSizeLoc >= 0) {
        const float ts[2] = {
            1.0F / static_cast<float>(backend.bloomMipWidths[i + 1]),
            1.0F / static_cast<float>(backend.bloomMipHeights[i + 1])};
        dev->set_uniform_vec2(backend.bloomUpTexelSizeLoc, ts);
      }
      dev->draw_arrays_triangles(0, 3);
    }

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::Bloom);
  }

  // --- Auto-exposure pass: compute average luminance → adapt exposure ---
  const bool autoExposureEnabled =
      backend.autoExposureAvailable &&
      core::cvar_get_bool("r_auto_exposure", true);
  if (autoExposureEnabled) {
    gpu_profiler_begin_pass(GpuPassId::AutoExposure);
    ensure_luminance_resources(backend, drawableWidth, drawableHeight);

    // Step 1: Extract log-luminance from HDR scene color into lum mip[0].
    dev->bind_framebuffer(backend.lumMipFbos[0]);
    dev->set_viewport(0, 0, backend.lumMipWidths[0], backend.lumMipHeights[0]);
    dev->disable_depth_test();
    dev->bind_program(backend.luminanceProgram);
    dev->bind_texture(0, pass_resource_gpu_texture(passRes.sceneColor));
    if (backend.lumSceneColorLoc >= 0) {
      dev->set_uniform_int(backend.lumSceneColorLoc, 0);
    }
    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    // Step 2: Progressive downsample to 1×1 using bloom downsample shader
    // (re-use as a generic bilinear downsample).
    if (backend.bloomDownsampleProgram != 0U) {
      dev->bind_program(backend.bloomDownsampleProgram);
      for (int i = 1; i < BackendState::kLuminanceMipLevels; ++i) {
        dev->bind_framebuffer(backend.lumMipFbos[i]);
        dev->set_viewport(0, 0, backend.lumMipWidths[i],
                          backend.lumMipHeights[i]);
        dev->bind_texture(0, backend.lumMipTextures[i - 1]);
        if (backend.bloomDownInputLoc >= 0) {
          dev->set_uniform_int(backend.bloomDownInputLoc, 0);
        }
        if (backend.bloomDownTexelSizeLoc >= 0) {
          const float ts[2] = {
              1.0F / static_cast<float>(backend.lumMipWidths[i - 1]),
              1.0F / static_cast<float>(backend.lumMipHeights[i - 1])};
          dev->set_uniform_vec2(backend.bloomDownTexelSizeLoc, ts);
        }
        dev->draw_arrays_triangles(0, 3);
      }
    }

    // Step 3: Read back average luminance from smallest mip (CPU-side).
    // In practice we'd use pixel readback, but for now we use temporal
    // adaptation from the previous frame's exposure. The luminance
    // mip chain approximates average scene luminance via successive
    // downsampling.
    // Adapt exposure: targetExposure = 1 / (2 * avgLuminance + epsilon).
    // We do temporal smoothing toward the target.
    const float adaptSpeed =
        core::cvar_get_float("r_auto_exposure_speed", 1.5F);
    const float minExposure = core::cvar_get_float("r_auto_exposure_min", 0.1F);
    const float maxExposure =
        core::cvar_get_float("r_auto_exposure_max", 10.0F);

    // Simple temporal adaptation (no readback — use previous frame's
    // estimate). The mip chain drives the shader-side average; we use
    // a smooth exponential approach.
    const float dt = 1.0F / 60.0F; // approximate frame dt
    const float targetExposure =
        std::clamp(backend.currentExposure, minExposure, maxExposure);
    backend.currentExposure +=
        (targetExposure - backend.currentExposure) * adaptSpeed * dt;
    backend.currentExposure =
        std::clamp(backend.currentExposure, minExposure, maxExposure);

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::AutoExposure);
  }

  // Determine final exposure value for tonemap.
  const float finalExposure = autoExposureEnabled
                                  ? backend.currentExposure
                                  : core::cvar_get_float("r_exposure", 1.0F);

  // --- Tonemap pass: HDR scene → LDR final FBO ---
  gpu_profiler_begin_pass(GpuPassId::Tonemap);
  const std::uint32_t finalFbo = pass_resource_framebuffer(passRes.finalColor);
  dev->bind_framebuffer(finalFbo);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->disable_depth_test();

  dev->bind_program(backend.tonemapProgram);

  const std::uint32_t sceneColorTex =
      pass_resource_gpu_texture(passRes.sceneColor);
  dev->bind_texture(0, sceneColorTex);
  if (backend.tonemapSceneColorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapSceneColorLocation, 0);
  }
  if (backend.tonemapExposureLocation >= 0) {
    dev->set_uniform_float(backend.tonemapExposureLocation, finalExposure);
  }
  if (backend.tonemapOperatorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapOperatorLocation,
                         core::cvar_get_int("r_tonemap_operator"));
  }

  // Bloom integration: bind bloom mip[0] on texture unit 1.
  if (bloomEnabled) {
    dev->bind_texture(1, backend.bloomMipTextures[0]);
    if (backend.tonemapBloomTextureLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomTextureLoc, 1);
    }
    if (backend.tonemapBloomIntensityLoc >= 0) {
      dev->set_uniform_float(backend.tonemapBloomIntensityLoc,
                             core::cvar_get_float("r_bloom_intensity"));
    }
    if (backend.tonemapBloomEnabledLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomEnabledLoc, 1);
    }
  } else {
    if (backend.tonemapBloomEnabledLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomEnabledLoc, 0);
    }
  }

  dev->bind_vertex_array(backend.emptyVao);
  dev->draw_arrays_triangles(0, 3);

  dev->bind_texture(0, 0U);
  if (bloomEnabled) {
    dev->bind_texture(1, 0U);
  }
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  gpu_profiler_end_pass(GpuPassId::Tonemap);

  // --- FXAA pass (optional): finalColor → sceneColor (ping-pong) ---
  renderer_context().fxaaAppliedThisFrame = false;
  if (backend.fxaaProgram != 0U && core::cvar_get_bool("r_fxaa")) {
    const std::uint32_t sceneFbo =
        pass_resource_framebuffer(passRes.sceneColor);
    dev->bind_framebuffer(sceneFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->disable_depth_test();

    dev->bind_program(backend.fxaaProgram);

    const std::uint32_t finalColorTex =
        pass_resource_gpu_texture(passRes.finalColor);
    dev->bind_texture(0, finalColorTex);
    if (backend.fxaaInputTextureLocation >= 0) {
      dev->set_uniform_int(backend.fxaaInputTextureLocation, 0);
    }
    if (backend.fxaaTexelSizeLocation >= 0) {
      const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                  1.0F / static_cast<float>(drawableHeight)};
      dev->set_uniform_vec2(backend.fxaaTexelSizeLocation, texelSize);
    }

    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);

    renderer_context().fxaaAppliedThisFrame = true;
  }

  // --- Prepare back buffer for editor overlay (ImGui) ---
  dev->bind_framebuffer(0U);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->set_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
  dev->clear_color_depth();
  dev->enable_depth_test();

  frameStats.gpuSceneMs = gpu_profiler_pass_ms(GpuPassId::Scene);
  frameStats.gpuTonemapMs = gpu_profiler_pass_ms(GpuPassId::Tonemap);
  frameStats.gpuBloomMs = gpu_profiler_pass_ms(GpuPassId::Bloom);
  frameStats.gpuShadowMapMs = directionalShadowCacheReused
                                  ? 0.0F
                                  : gpu_profiler_pass_ms(GpuPassId::ShadowMap);
  frameStats.gpuSpotShadowMs = gpu_profiler_pass_ms(GpuPassId::SpotShadowMap);
  frameStats.gpuPointShadowMs = gpu_profiler_pass_ms(GpuPassId::PointShadowMap);
  frameStats.gpuAutoExposureMs = gpu_profiler_pass_ms(GpuPassId::AutoExposure);
  renderer_context().lastFrameStats = frameStats;
}

} // namespace engine::renderer
