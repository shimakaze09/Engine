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

void refresh_backend_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  if (dev == nullptr) {
    return;
  }

  static_cast<void>(resolve_default_program_state(backend, dev));
  if (!resolve_pbr_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "required PBR uniforms missing after shader reload");
  }
  static_cast<void>(resolve_tonemap_program_state(backend, dev));

  if ((backend.skyboxShaderHandle != kInvalidShaderProgram) &&
      !resolve_skybox_program_state(backend, dev) &&
      backend.skyboxAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "skybox shader lost uniforms on reload — disabled");
    backend.skyboxAvailable = false;
  }
  if ((backend.preethamSkyShaderHandle != kInvalidShaderProgram) &&
      !resolve_preetham_sky_program_state(backend, dev) &&
      backend.preethamSkyAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "Preetham sky shader lost uniforms on reload — disabled");
    backend.preethamSkyAvailable = false;
  }
  if ((backend.hosekSkyShaderHandle != kInvalidShaderProgram) &&
      !resolve_hosek_sky_program_state(backend, dev) &&
      backend.hosekSkyAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "procedural sky shader lost uniforms on reload — "
                      "disabled");
    backend.hosekSkyAvailable = false;
  }
  if ((backend.environmentPrefilterShaderHandle != kInvalidShaderProgram) &&
      !resolve_environment_prefilter_program_state(backend, dev) &&
      backend.environmentPrefilterAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "IBL prefilter shader lost uniforms on reload — "
                      "disabled");
    backend.environmentPrefilterAvailable = false;
  }
  if ((backend.environmentIrradianceShaderHandle != kInvalidShaderProgram) &&
      !resolve_environment_irradiance_program_state(backend, dev) &&
      backend.environmentIrradianceAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "IBL irradiance shader lost uniforms on reload — "
                      "disabled");
    backend.environmentIrradianceAvailable = false;
  }
  if (backend.environmentBrdfLutShaderHandle != kInvalidShaderProgram) {
    static_cast<void>(
        resolve_environment_brdf_lut_program_state(backend, dev));
  }

  if (backend.deferredAvailable) {
    const bool gbufferOk = resolve_gbuffer_program_state(backend, dev);
    const bool deferredOk = resolve_deferred_light_program_state(backend, dev);
    if (!gbufferOk || !deferredOk) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "deferred shaders lost programs on reload — deferred "
                        "path disabled");
      backend.deferredAvailable = false;
    }
  }
  if (backend.gbufferDebugShaderHandle != kInvalidShaderProgram) {
    static_cast<void>(resolve_gbuffer_debug_program_state(backend, dev));
  }

  if ((backend.shadowDepthShaderHandle != kInvalidShaderProgram) &&
      !resolve_shadow_depth_program_state(backend, dev) &&
      backend.shadowAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "shadow depth shader lost program on reload — shadows "
                      "disabled");
    backend.shadowAvailable = false;
    backend.spotShadowAvailable = false;
  }
  if ((backend.shadowDepthPointShaderHandle != kInvalidShaderProgram) &&
      !resolve_shadow_depth_point_program_state(backend, dev) &&
      backend.pointShadowAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "point shadow shader lost program on reload — disabled");
    backend.pointShadowAvailable = false;
  }
  if ((backend.gbufferSkinnedShaderHandle != kInvalidShaderProgram) &&
      !resolve_gbuffer_skinned_program_state(backend, dev) &&
      backend.skinningAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "skinned G-buffer shader lost program on reload — GPU "
                      "skinning disabled");
    backend.skinningAvailable = false;
  }
  if (backend.shadowDepthSkinnedShaderHandle != kInvalidShaderProgram) {
    static_cast<void>(
        resolve_shadow_depth_skinned_program_state(backend, dev));
  }

  if (backend.fxaaShaderHandle != kInvalidShaderProgram) {
    static_cast<void>(resolve_fxaa_program_state(backend, dev));
  }
  static_cast<void>(resolve_bloom_program_state(backend, dev));
  if (!resolve_ssao_program_state(backend, dev) && backend.ssaoAvailable) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "SSAO shaders lost programs on reload — SSAO disabled");
    backend.ssaoAvailable = false;
  }
  if ((backend.debugLineShaderHandle != kInvalidShaderProgram) &&
      !resolve_debug_line_program_state(backend, dev) &&
      backend.debugLineAvailable) {
    backend.debugLineAvailable = false;
  }
  if ((backend.luminanceShaderHandle != kInvalidShaderProgram) &&
      !resolve_luminance_program_state(backend, dev) &&
      backend.autoExposureAvailable) {
    backend.autoExposureAvailable = false;
  }
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
  if (backend->tileLightTex != 0U && dev != nullptr) {
    dev->destroy_texture(backend->tileLightTex);
    backend->tileLightTex = 0U;
  }
  backend->tileLightTexRows = 0;
  backend->tileBuffer.clear();

  // Destroy per-light data texture.
  if (backend->lightDataTex != 0U && dev != nullptr) {
    dev->destroy_texture(backend->lightDataTex);
    backend->lightDataTex = 0U;
  }

  // Destroy debug line resources.
  if (backend->debugLineVao != 0U && dev != nullptr) {
    dev->destroy_vertex_array(backend->debugLineVao);
    backend->debugLineVao = 0U;
  }
  if (backend->debugLineVbo != 0U && dev != nullptr) {
    dev->destroy_buffer(backend->debugLineVbo);
    backend->debugLineVbo = 0U;
  }
  if (backend->debugLineShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->debugLineShaderHandle);
    backend->debugLineShaderHandle = ShaderProgramHandle{};
  }
  backend->debugLineProgram = 0U;
  backend->debugLineAvailable = false;

  // Destroy scene capture render targets.
  destroy_scene_capture_targets(*backend, dev);

  // Destroy bloom resources.
  destroy_bloom_resources(*backend);
  if (backend->bloomUpsampleShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->bloomUpsampleShaderHandle);
    backend->bloomUpsampleShaderHandle = ShaderProgramHandle{};
  }
  backend->bloomUpsampleProgram = 0U;
  if (backend->bloomDownsampleShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->bloomDownsampleShaderHandle);
    backend->bloomDownsampleShaderHandle = ShaderProgramHandle{};
  }
  backend->bloomDownsampleProgram = 0U;
  if (backend->bloomThresholdShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->bloomThresholdShaderHandle);
    backend->bloomThresholdShaderHandle = ShaderProgramHandle{};
  }
  backend->bloomThresholdProgram = 0U;

  // Destroy SSAO resources.
  if (backend->ssaoNoiseTexture != 0U && dev != nullptr) {
    dev->destroy_texture(backend->ssaoNoiseTexture);
    backend->ssaoNoiseTexture = 0U;
  }
  if (backend->ssaoBlurShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->ssaoBlurShaderHandle);
    backend->ssaoBlurShaderHandle = ShaderProgramHandle{};
  }
  backend->ssaoBlurProgram = 0U;
  if (backend->ssaoShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->ssaoShaderHandle);
    backend->ssaoShaderHandle = ShaderProgramHandle{};
  }
  backend->ssaoProgram = 0U;
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
  backend->shadowDepthProgram = 0U;

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
  backend->shadowDepthPointProgram = 0U;

  // Destroy auto-exposure resources.
  destroy_luminance_resources(*backend);
  if (backend->luminanceShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->luminanceShaderHandle);
    backend->luminanceShaderHandle = ShaderProgramHandle{};
  }
  backend->luminanceProgram = 0U;
  backend->autoExposureAvailable = false;

  destroy_brdf_lut_resources(*backend);
  destroy_environment_irradiance_resources(*backend);
  destroy_environment_prefilter_resources(*backend);
  destroy_skybox_resources(*backend);

  if (backend->emptyVao != 0U && dev != nullptr) {
    dev->destroy_vertex_array(backend->emptyVao);
    backend->emptyVao = 0U;
  }
  if (backend->instanceMatrixBuffer != 0U && dev != nullptr) {
    dev->destroy_buffer(backend->instanceMatrixBuffer);
    backend->instanceMatrixBuffer = 0U;
  }
  backend->instanceAttributes.clear();
  backend->staticMeshBatches.clear();

  // Destroy GPU skinning state.
  if (backend->bonePaletteUbo != 0U && dev != nullptr) {
    dev->destroy_buffer(backend->bonePaletteUbo);
    backend->bonePaletteUbo = 0U;
  }
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
  backend->deferredAvailable = false;

  if (backend->fxaaShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->fxaaShaderHandle);
    backend->fxaaShaderHandle = ShaderProgramHandle{};
  }
  backend->fxaaProgram = 0U;

  if (backend->tonemapShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->tonemapShaderHandle);
    backend->tonemapShaderHandle = ShaderProgramHandle{};
  }
  backend->tonemapProgram = 0U;

  if (backend->pbrShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->pbrShaderHandle);
    backend->pbrShaderHandle = ShaderProgramHandle{};
  }
  if (backend->defaultShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->defaultShaderHandle);
    backend->defaultShaderHandle = ShaderProgramHandle{};
  }
  backend->pbrProgram = 0U;
  backend->defaultProgram = 0U;
  backend->initialized = false;
  shutdown_gpu_profiler();
}

} // namespace

/// Shuts down the owning system for renderer.
void shutdown_renderer() noexcept {
  BackendState &backend = backend_state();
  if (!backend.initialized && !backend.failed) {
    // No GL resources exist yet, but capture slots may hold texture-system
    // handles that would go stale across a texture-system restart.
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

std::uint32_t get_scene_viewport_texture() noexcept {
  const PassResources &passRes = get_pass_resources();
  if (renderer_context().fxaaAppliedThisFrame) {
    return pass_resource_gpu_texture(passRes.sceneColor);
  }
  return pass_resource_gpu_texture(passRes.finalColor);
}

RendererFrameStats renderer_get_last_frame_stats() noexcept {
  return renderer_context().lastFrameStats;
}

} // namespace engine::renderer