// Implements command buffer behavior for the Engine renderer system.

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
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/post_process_stack.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/texture_loader.h"
#include "command_buffer_init_internal.h"

namespace engine::renderer {

namespace {

/// Latched by shutdown_renderer and cleared by initialize_renderer, so a
/// shut-down module is distinguishable from one that has never started.
/// The backend initializes lazily from the first flush and shutdown ends
/// with `backend = BackendState{}`, which clears both the initialized and
/// failed flags — without this latch those two states are identical, and
/// a flush issued after teardown re-runs full initialization against a
/// destroyed device and shader system. Ownership contract (#168): no
/// global may lazily resurrect a subsystem.
bool g_shutDown = false;

/// True once the refusal below has been logged, so a caller that keeps
/// flushing after shutdown logs the reason once rather than once a frame.
bool g_shutDownRefusalLogged = false;

/// Builds a configured shader path from a shader file name.
bool make_shader_path(const char *fileName, char *outPath,
                      std::size_t outCapacity) noexcept {
  if ((fileName == nullptr) || (outPath == nullptr) || (outCapacity == 0U)) {
    return false;
  }
  const int written =
      std::snprintf(outPath, outCapacity, "%s/%s",
                    renderer_context().shaderRootPath, fileName);
  return (written > 0) &&
         (static_cast<std::size_t>(written) < outCapacity);
}


} // namespace

/// Loads a shader program from the configured shader root.
ShaderProgramHandle load_configured_shader_program(
    const char *vertexShader, const char *fragmentShader) noexcept {
  char vertexPath[512]{};
  char fragmentPath[512]{};
  if (!make_shader_path(vertexShader, vertexPath, sizeof(vertexPath)) ||
      !make_shader_path(fragmentShader, fragmentPath, sizeof(fragmentPath))) {
    return ShaderProgramHandle{};
  }
  return load_shader_program(vertexPath, fragmentPath);
}

/// Loads a shader-pair variant from the configured shader root with the
/// given preprocessor defines.
ShaderProgramHandle load_configured_shader_variant(
    const char *vertexShader, const char *fragmentShader,
    const ShaderDefine *defines, std::size_t defineCount) noexcept {
  char vertexPath[512]{};
  char fragmentPath[512]{};
  if (!make_shader_path(vertexShader, vertexPath, sizeof(vertexPath)) ||
      !make_shader_path(fragmentShader, fragmentPath, sizeof(fragmentPath))) {
    return ShaderProgramHandle{};
  }
  ShaderVariantDesc desc{};
  desc.vertPath = vertexPath;
  desc.fragPath = fragmentPath;
  desc.defines = defines;
  desc.defineCount = defineCount;
  return load_shader_variant(desc);
}

/// Initializes the GL backend once: the hard-fail core first, then the
/// soft-fail environment, lighting, and post groups whose availability
/// flags gate their passes.
bool initialize_backend() noexcept {
  // Checked before the resource flags: while the module is shut down its
  // backend state says nothing, and lazily rebuilding it here is the
  // resurrection the latch exists to refuse.
  if (g_shutDown) {
    if (!g_shutDownRefusalLogged) {
      g_shutDownRefusalLogged = true;
      core::log_message(core::LogLevel::Warning, "renderer",
                        "renderer work issued after shutdown_renderer; "
                        "ignored rather than re-initializing the backend");
    }
    return false;
  }

  BackendState &backend = backend_state();
  if (backend.initialized) {
    return true;
  }
  if (backend.failed) {
    return false;
  }

  if (!init_backend_core(backend)) {
    return false;
  }

  const RenderDevice *dev = render_device();
  init_backend_environment(backend, dev);
  init_backend_lighting(backend, dev);
  init_backend_post(backend, dev);

  backend.programCacheEpoch = shader_reload_epoch();
  backend.initialized = true;
  return true;
}

namespace {

/// Recomputes one family's availability from this reload's reflection
/// result and its (reload-invariant) resource readiness, logging only on
/// transitions so a broken edit warns once and a corrected reload
/// restores the feature (review item 7: flags used to latch false).
void recompute_availability(bool *availability, bool reflectionOk,
                            bool resourcesReady, const char *familyName) {
  const bool nowAvailable = reflectionOk && resourcesReady;
  if (*availability && !nowAvailable) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "%s lost required state on shader reload — disabled",
                  familyName);
    core::log_message(core::LogLevel::Warning, "renderer", message);
  } else if (!*availability && nowAvailable) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "%s restored by shader reload — re-enabled", familyName);
    core::log_message(core::LogLevel::Info, "renderer", message);
  }
  *availability = nowAvailable;
}

} // namespace

void refresh_backend_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  if (dev == nullptr) {
    return;
  }

  static_cast<void>(resolve_default_program_state(backend, dev));
  // Instanced siblings cache no parameters (global-registry tokens);
  // refresh only re-reads their device programs.
  backend.pbrInstancedProgram =
      shader_device_program(backend.pbrInstancedShaderHandle);
  backend.gbufferInstancedProgram =
      shader_device_program(backend.gbufferInstancedShaderHandle);
  backend.depthCopyProgram =
      shader_device_program(backend.depthCopyShaderHandle);
  if ((backend.depthCopyProgram != kInvalidDeviceProgram) &&
      (dev->shader_param != nullptr)) {
    backend.depthCopyDepthLoc =
        dev->shader_param(backend.depthCopyProgram, "uDepth");
  }
  if (!resolve_pbr_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "required PBR uniforms missing after shader reload");
  }
  if (!resolve_tonemap_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "required tonemap uniforms missing after shader reload");
  }

  // Every family recomputes availability from scratch: reflection is
  // re-evaluated on each reload while resource readiness (geometry,
  // FBOs, UBOs) is reload-invariant, so a corrected shader re-enables
  // its feature instead of latching it off.
  const bool skyGeometryReady =
      backend.skyboxGeometry != kInvalidDeviceGeometry;
  recompute_availability(
      &backend.skyboxAvailable,
      (backend.skyboxShaderHandle != kInvalidShaderProgram) &&
          resolve_skybox_program_state(backend, dev),
      skyGeometryReady, "skybox");
  recompute_availability(
      &backend.preethamSkyAvailable,
      (backend.preethamSkyShaderHandle != kInvalidShaderProgram) &&
          resolve_preetham_sky_program_state(backend, dev),
      skyGeometryReady, "Preetham sky");
  recompute_availability(
      &backend.hosekSkyAvailable,
      (backend.hosekSkyShaderHandle != kInvalidShaderProgram) &&
          resolve_hosek_sky_program_state(backend, dev),
      skyGeometryReady, "procedural sky");
  recompute_availability(
      &backend.environmentPrefilterAvailable,
      (backend.environmentPrefilterShaderHandle != kInvalidShaderProgram) &&
          resolve_environment_prefilter_program_state(backend, dev),
      skyGeometryReady, "IBL prefilter");
  recompute_availability(
      &backend.environmentIrradianceAvailable,
      (backend.environmentIrradianceShaderHandle != kInvalidShaderProgram) &&
          resolve_environment_irradiance_program_state(backend, dev),
      skyGeometryReady, "IBL irradiance");
  recompute_availability(
      &backend.environmentBrdfLutAvailable,
      (backend.environmentBrdfLutShaderHandle != kInvalidShaderProgram) &&
          resolve_environment_brdf_lut_program_state(backend, dev),
      true, "BRDF LUT");

  const bool gbufferOk =
      (backend.gbufferShaderHandle != kInvalidShaderProgram) &&
      resolve_gbuffer_program_state(backend, dev);
  const bool deferredLightOk =
      (backend.deferredLightShaderHandle != kInvalidShaderProgram) &&
      resolve_deferred_light_program_state(backend, dev);
  recompute_availability(&backend.deferredAvailable,
                         gbufferOk && deferredLightOk, true,
                         "deferred path");
  // Program-gated families (no availability flag): their resolvers zero
  // the cached program id on a broken interface, which is what their
  // draw paths gate on, so a failure here disables the pass and a
  // corrected reload restores it from the retained handle.
  if ((backend.gbufferDebugShaderHandle != kInvalidShaderProgram) &&
      !resolve_gbuffer_debug_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "G-Buffer debug lost required uniforms on shader "
                      "reload — disabled");
  }

  recompute_availability(
      &backend.shadowAvailable,
      (backend.shadowDepthShaderHandle != kInvalidShaderProgram) &&
          resolve_shadow_depth_program_state(backend, dev),
      backend.shadowState.initialized, "cascade shadows");
  recompute_availability(&backend.spotShadowAvailable,
                         backend.shadowAvailable,
                         backend.spotShadowState.initialized, "spot shadows");
  recompute_availability(
      &backend.pointShadowAvailable,
      (backend.shadowDepthPointShaderHandle != kInvalidShaderProgram) &&
          resolve_shadow_depth_point_program_state(backend, dev),
      backend.pointShadowState.initialized, "point shadows");
  recompute_availability(
      &backend.skinningAvailable,
      (backend.gbufferSkinnedShaderHandle != kInvalidShaderProgram) &&
          resolve_gbuffer_skinned_program_state(backend, dev),
      backend.gbufSkinnedBonesParam.valid(), "GPU skinning");
  if ((backend.shadowDepthSkinnedShaderHandle != kInvalidShaderProgram) &&
      !resolve_shadow_depth_skinned_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "skinned shadow variant lost required state on shader "
                      "reload — skinned meshes cast bind-pose shadows");
  }

  if ((backend.fxaaShaderHandle != kInvalidShaderProgram) &&
      !resolve_fxaa_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "FXAA lost required uniforms on shader reload — "
                      "disabled");
  }
  if ((backend.presentBlitShaderHandle != kInvalidShaderProgram) &&
      !resolve_present_blit_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "present blit lost required uniforms on shader "
                      "reload — player-mode present disabled");
  }
  if (!resolve_bloom_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "bloom lost required state on shader reload — "
                      "disabled");
  }
  recompute_availability(&backend.ssaoAvailable,
                         resolve_ssao_program_state(backend, dev),
                         backend.ssaoNoiseTexture != kInvalidDeviceTexture,
                         "SSAO");
  recompute_availability(
      &backend.debugLineAvailable,
      (backend.debugLineShaderHandle != kInvalidShaderProgram) &&
          resolve_debug_line_program_state(backend, dev),
      (backend.debugLineGeometry != kInvalidDeviceGeometry) &&
          (backend.debugLineVbo != kInvalidDeviceBuffer),
      "debug lines");
  recompute_availability(
      &backend.autoExposureAvailable,
      (backend.luminanceShaderHandle != kInvalidShaderProgram) &&
          resolve_luminance_program_state(backend, dev),
      true, "auto exposure");
}

namespace {

/// Destroys or releases the requested object, handle, or resource for backend resources.
void destroy_backend_resources(BackendState *backend) noexcept {
  if (backend == nullptr) {
    return;
  }

  shutdown_pass_resources();

  const RenderDevice *dev = render_device();

  // Destroy tile light texture.
  if ((backend->tileLightTex != kInvalidDeviceTexture) && (dev != nullptr)) {
    dev->destroy_texture(backend->tileLightTex);
    backend->tileLightTex = kInvalidDeviceTexture;
  }
  backend->tileLightTexWidth = 0;
  backend->tileLightTexHeight = 0;
  backend->tileBuffer.clear();

  // Destroy per-light data texture.
  if ((backend->lightDataTex != kInvalidDeviceTexture) && (dev != nullptr)) {
    dev->destroy_texture(backend->lightDataTex);
    backend->lightDataTex = kInvalidDeviceTexture;
  }

  // Destroy debug line resources.
  if ((backend->debugLineGeometry != kInvalidDeviceGeometry) &&
      (dev != nullptr)) {
    dev->destroy_geometry(backend->debugLineGeometry);
    backend->debugLineGeometry = kInvalidDeviceGeometry;
  }
  if ((backend->debugLineVbo != kInvalidDeviceBuffer) && (dev != nullptr)) {
    dev->destroy_buffer(backend->debugLineVbo);
    backend->debugLineVbo = kInvalidDeviceBuffer;
  }
  if (backend->debugLineShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->debugLineShaderHandle);
    backend->debugLineShaderHandle = ShaderProgramHandle{};
  }
  backend->debugLineProgram = kInvalidDeviceProgram;
  backend->debugLineAvailable = false;

  // Destroy scene capture render targets.
  destroy_scene_capture_targets(*backend, dev);

  // Destroy bloom resources.
  destroy_bloom_resources(*backend);
  if (backend->bloomUpsampleShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->bloomUpsampleShaderHandle);
    backend->bloomUpsampleShaderHandle = ShaderProgramHandle{};
  }
  backend->bloomUpsampleProgram = kInvalidDeviceProgram;
  if (backend->bloomDownsampleShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->bloomDownsampleShaderHandle);
    backend->bloomDownsampleShaderHandle = ShaderProgramHandle{};
  }
  backend->bloomDownsampleProgram = kInvalidDeviceProgram;
  if (backend->bloomThresholdShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->bloomThresholdShaderHandle);
    backend->bloomThresholdShaderHandle = ShaderProgramHandle{};
  }
  backend->bloomThresholdProgram = kInvalidDeviceProgram;

  // Destroy SSAO resources.
  if ((backend->ssaoNoiseTexture != kInvalidDeviceTexture) &&
      (dev != nullptr)) {
    dev->destroy_texture(backend->ssaoNoiseTexture);
    backend->ssaoNoiseTexture = kInvalidDeviceTexture;
  }
  if (backend->ssaoBlurShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->ssaoBlurShaderHandle);
    backend->ssaoBlurShaderHandle = ShaderProgramHandle{};
  }
  backend->ssaoBlurProgram = kInvalidDeviceProgram;
  if (backend->ssaoShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->ssaoShaderHandle);
    backend->ssaoShaderHandle = ShaderProgramHandle{};
  }
  backend->ssaoProgram = kInvalidDeviceProgram;
  backend->ssaoAvailable = false;

  // Destroy shadow map resources.
  shutdown_shadow_maps(backend->shadowState);
  backend->shadowAvailable = false;
  backend->directionalShadowCacheKey = 0U;
  backend->directionalShadowCacheValid = false;
  if (backend->shadowDepthShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->shadowDepthShaderHandle);
    backend->shadowDepthShaderHandle = ShaderProgramHandle{};
  }
  backend->shadowDepthProgram = kInvalidDeviceProgram;

  // Destroy spot shadow resources.
  shutdown_spot_shadow_maps(backend->spotShadowState);
  backend->spotShadowAvailable = false;

  // Destroy point shadow resources.
  shutdown_point_shadow_maps(backend->pointShadowState);
  backend->pointShadowAvailable = false;
  if (backend->shadowDepthPointShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->shadowDepthPointShaderHandle);
    backend->shadowDepthPointShaderHandle = ShaderProgramHandle{};
  }
  backend->shadowDepthPointProgram = kInvalidDeviceProgram;

  // Destroy auto-exposure resources.
  destroy_luminance_resources(*backend);
  if (backend->luminanceShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->luminanceShaderHandle);
    backend->luminanceShaderHandle = ShaderProgramHandle{};
  }
  backend->luminanceProgram = kInvalidDeviceProgram;
  backend->autoExposureAvailable = false;

  destroy_brdf_lut_resources(*backend);
  destroy_environment_irradiance_resources(*backend);
  destroy_environment_prefilter_resources(*backend);
  destroy_skybox_resources(*backend);

  if ((backend->emptyGeometry != kInvalidDeviceGeometry) &&
      (dev != nullptr)) {
    dev->destroy_geometry(backend->emptyGeometry);
    backend->emptyGeometry = kInvalidDeviceGeometry;
  }
  if ((backend->instanceMatrixBuffer != kInvalidDeviceBuffer) &&
      (dev != nullptr)) {
    dev->destroy_buffer(backend->instanceMatrixBuffer);
    backend->instanceMatrixBuffer = kInvalidDeviceBuffer;
  }
  backend->instanceAttributes.clear();
  backend->staticMeshBatches.clear();

  // GPU skinning state is per-program uniform data; nothing to destroy.
  if (backend->gbufferSkinnedShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->gbufferSkinnedShaderHandle);
    backend->gbufferSkinnedShaderHandle = ShaderProgramHandle{};
  }
  if (backend->shadowDepthSkinnedShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->shadowDepthSkinnedShaderHandle);
    backend->shadowDepthSkinnedShaderHandle = ShaderProgramHandle{};
  }
  backend->skinningAvailable = false;

  // Destroy deferred shaders.
  if (backend->gbufferDebugShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->gbufferDebugShaderHandle);
    backend->gbufferDebugShaderHandle = ShaderProgramHandle{};
  }
  if (backend->deferredLightShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->deferredLightShaderHandle);
    backend->deferredLightShaderHandle = ShaderProgramHandle{};
  }
  if (backend->gbufferShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->gbufferShaderHandle);
    backend->gbufferShaderHandle = ShaderProgramHandle{};
  }
  if (backend->gbufferInstancedShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->gbufferInstancedShaderHandle);
    backend->gbufferInstancedShaderHandle = ShaderProgramHandle{};
  }
  backend->gbufferInstancedProgram = kInvalidDeviceProgram;
  if (backend->depthCopyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->depthCopyShaderHandle);
    backend->depthCopyShaderHandle = ShaderProgramHandle{};
  }
  backend->depthCopyProgram = kInvalidDeviceProgram;
  backend->deferredAvailable = false;

  if (backend->fxaaShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->fxaaShaderHandle);
    backend->fxaaShaderHandle = ShaderProgramHandle{};
  }
  backend->fxaaProgram = kInvalidDeviceProgram;

  if (backend->presentBlitShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->presentBlitShaderHandle);
    backend->presentBlitShaderHandle = ShaderProgramHandle{};
  }
  backend->presentBlitProgram = kInvalidDeviceProgram;

  if (backend->tonemapShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->tonemapShaderHandle);
    backend->tonemapShaderHandle = ShaderProgramHandle{};
  }
  backend->tonemapProgram = kInvalidDeviceProgram;

  if (backend->pbrShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->pbrShaderHandle);
    backend->pbrShaderHandle = ShaderProgramHandle{};
  }
  if (backend->pbrInstancedShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->pbrInstancedShaderHandle);
    backend->pbrInstancedShaderHandle = ShaderProgramHandle{};
  }
  backend->pbrInstancedProgram = kInvalidDeviceProgram;
  if (backend->defaultShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->defaultShaderHandle);
    backend->defaultShaderHandle = ShaderProgramHandle{};
  }
  if ((dev != nullptr) && (dev->destroy_texture != nullptr)) {
    if (backend->fallbackTexture2D != kInvalidDeviceTexture) {
      dev->destroy_texture(backend->fallbackTexture2D);
      backend->fallbackTexture2D = kInvalidDeviceTexture;
    }
    if (backend->fallbackCubemap != kInvalidDeviceTexture) {
      dev->destroy_texture(backend->fallbackCubemap);
      backend->fallbackCubemap = kInvalidDeviceTexture;
    }
    if (backend->fallbackTexture2DArray != kInvalidDeviceTexture) {
      dev->destroy_texture(backend->fallbackTexture2DArray);
      backend->fallbackTexture2DArray = kInvalidDeviceTexture;
    }
  }
  backend->pbrProgram = kInvalidDeviceProgram;
  backend->defaultProgram = kInvalidDeviceProgram;
  backend->initialized = false;
  shutdown_gpu_profiler();
}

} // namespace

/// Opens a renderer lifetime for the owning system.
void initialize_renderer() noexcept {
  g_shutDown = false;
  g_shutDownRefusalLogged = false;
}

/// Shuts down the owning system for renderer.
void shutdown_renderer() noexcept {
  // Latched before the early return below, so the contract holds on every
  // path: once this call returns, the module is shut down whether or not
  // it had reached the point of owning device resources.
  g_shutDown = true;
  g_shutDownRefusalLogged = false;

  BackendState &backend = backend_state();
  if (!backend.initialized && !backend.failed) {
    // No device resources exist yet, but capture slots may hold texture-
    // system handles that would go stale across a texture-system restart.
    destroy_scene_capture_targets(backend, nullptr);
    backend.sceneCaptureTargets = {};
    reset_renderer_public_state();
    return;
  }

  if (core::make_render_context_current()) {
    destroy_backend_resources(&backend);
    shutdown_shader_system();
    shutdown_render_device();
    core::release_render_context();
  }

  backend = BackendState{};
  reset_renderer_public_state();
}

/// Sets the requested value for active camera.
void set_active_camera(const CameraState &camera) noexcept {
  renderer_context().activeCamera = camera;
}

/// Sets the virtual root used for built-in renderer shaders.
void set_shader_root_path(const char *path) noexcept {
  const char *source =
      ((path != nullptr) && (path[0] != '\0')) ? path : "assets/shaders";
  const std::size_t len = std::strlen(source);
  const std::size_t maxCopy =
      sizeof(renderer_context().shaderRootPath) - 1U;
  const std::size_t copyLen = (len < maxCopy) ? len : maxCopy;
  std::memcpy(renderer_context().shaderRootPath, source, copyLen);
  renderer_context().shaderRootPath[copyLen] = '\0';
  if ((copyLen > 0U) &&
      (renderer_context().shaderRootPath[copyLen - 1U] == '/')) {
    renderer_context().shaderRootPath[copyLen - 1U] = '\0';
  }
}

/// Sets the requested value for scene viewport size.
void set_scene_viewport_size(int width, int height) noexcept {
  renderer_context().sceneViewportWidth = (width > 0) ? width : 0;
  renderer_context().sceneViewportHeight = (height > 0) ? height : 0;
}

/// Sets the requested value for skybox texture.
void set_skybox_texture(TextureHandle cubemap) noexcept {
  renderer_context().activeSkyboxTexture = cubemap;
}

TextureHandle get_skybox_texture() noexcept {
  return renderer_context().activeSkyboxTexture;
}

CameraState get_active_camera() noexcept {
  return renderer_context().activeCamera;
}

DeviceTextureHandle get_scene_viewport_texture() noexcept {
  const PassResources &passRes = get_pass_resources();
  if (renderer_context().fxaaAppliedThisFrame) {
    return pass_resource_texture(passRes.sceneColor);
  }
  return pass_resource_texture(passRes.finalColor);
}

RendererFrameStats renderer_get_last_frame_stats() noexcept {
  return renderer_context().lastFrameStats;
}

} // namespace engine::renderer