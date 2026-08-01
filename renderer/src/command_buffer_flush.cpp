// Implements the renderer frame flush orchestration: per-frame setup
// (targets, camera, environment IBL baked before any lighting pass so
// both paths can sample it, opaque/transparent partition) and dispatch
// through the pass TUs sharing FrameFlushContext.

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

constexpr float kDefaultFovRadians = 1.0471975512F;
constexpr float kNearClip = 0.1F;
constexpr float kFarClip = 100.0F;
constexpr std::uint64_t kDrawKeyTransparentBit = 1ULL << 63U;

} // namespace

const SceneLightData &
sanitize_scene_light_counts(const SceneLightData &lights,
                            SceneLightData &storage) noexcept {
  if ((lights.pointLightCount <= kMaxPointLights) &&
      (lights.spotLightCount <= kMaxSpotLights)) {
    return lights;
  }
  core::log_message(core::LogLevel::Warning, "renderer",
                    "scene light counts exceed fixed capacities; clamped");
  storage = lights;
  storage.pointLightCount =
      std::min(lights.pointLightCount, kMaxPointLights);
  storage.spotLightCount = std::min(lights.spotLightCount, kMaxSpotLights);
  return storage;
}

void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &rawLights) noexcept {
  if (!initialize_backend()) {
    return;
  }

  // The flush runs on the render thread only, so one static sanitized
  // copy backs the rare oversized-count case without per-frame cost.
  static SceneLightData sanitizedLightStorage;
  const SceneLightData &lights =
      sanitize_scene_light_counts(rawLights, sanitizedLightStorage);

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();
  RendererFrameStats frameStats{};
  backend.lastUploadedBonePalette = 0xFFFFFFFFU;
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

  const std::uint32_t envSkyboxTexture =
      (selected_sky_model() == SkyModel::Cubemap)
          ? active_skybox_gpu_texture(backend)
          : 0U;
  std::uint32_t iblPrefilteredTex = 0U;
  std::uint32_t iblIrradianceTex = 0U;
  if (envSkyboxTexture != 0U) {
    iblPrefilteredTex = ensure_prefiltered_environment(
        backend, dev, envSkyboxTexture, environmentBakeSettings);
    iblIrradianceTex = ensure_irradiance_environment(
        backend, dev, envSkyboxTexture, environmentBakeSettings);
  }
  const bool iblAvailable = (iblPrefilteredTex != 0U) &&
                            (iblIrradianceTex != 0U) &&
                            (backend.brdfLutTexture != 0U);

  const bool useDeferred =
      backend.deferredAvailable && core::cvar_get_bool("r_deferred", true);
  const int gbufferDebugMode = core::cvar_get_int("r_gbuffer_debug", 0);

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

  FrameFlushContext ctx{backend,
                        dev,
                        commandBufferView,
                        registry,
                        lights,
                        timeSeconds,
                        passRes,
                        drawableWidth,
                        drawableHeight,
                        fogSettings,
                        heightFogSettings,
                        envSkyboxTexture,
                        iblPrefilteredTex,
                        iblIrradianceTex,
                        iblAvailable,
                        viewMat,
                        projMat,
                        viewProjection,
                        nearP,
                        farP,
                        opaqueCount,
                        totalCount,
                        opaqueBatchCount,
                        gbufferDebugMode};
  ctx.frameStats = frameStats;

  flush_shadow_passes(ctx);
  flush_scene_captures(ctx);
  if (useDeferred) {
    flush_deferred_path(ctx);
  } else {
    flush_forward_path(ctx);
  }
  flush_debug_overlay(ctx);
  flush_post_chain(ctx);

  frameStats = ctx.frameStats;

  frameStats.gpuSceneMs = gpu_profiler_pass_ms(GpuPassId::Scene);
  frameStats.gpuTonemapMs = gpu_profiler_pass_ms(GpuPassId::Tonemap);
  frameStats.gpuBloomMs = gpu_profiler_pass_ms(GpuPassId::Bloom);
  frameStats.gpuShadowMapMs = ctx.directionalShadowCacheReused
                                  ? 0.0F
                                  : gpu_profiler_pass_ms(GpuPassId::ShadowMap);
  frameStats.gpuSpotShadowMs = gpu_profiler_pass_ms(GpuPassId::SpotShadowMap);
  frameStats.gpuPointShadowMs = gpu_profiler_pass_ms(GpuPassId::PointShadowMap);
  frameStats.gpuAutoExposureMs = gpu_profiler_pass_ms(GpuPassId::AutoExposure);
  renderer_context().lastFrameStats = frameStats;
  frameStats.gpuGBufferMs = gpu_profiler_pass_ms(GpuPassId::GBuffer);
  frameStats.gpuDeferredLightMs =
      gpu_profiler_pass_ms(GpuPassId::DeferredLighting);
  frameStats.gpuSsaoMs = gpu_profiler_pass_ms(GpuPassId::SSAO);
  renderer_context().lastFrameStats = frameStats;
}

} // namespace engine::renderer