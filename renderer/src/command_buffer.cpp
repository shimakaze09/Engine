// Implements command buffer behavior for the Engine renderer system.

#include "engine/renderer/command_buffer.h"
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

void resolve_pbr_light_uniforms(BackendState &backend,
                                const RenderDevice *dev) noexcept {
  const std::uint32_t prog = backend.pbrProgram;

  backend.pbrDirLightCountLocation =
      dev->uniform_location(prog, "u_dirLightCount");
  for (std::size_t i = 0U; i < kMaxDirectionalLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].direction", i);
    backend.pbrDirLightDir[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].color", i);
    backend.pbrDirLightColor[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].intensity", i);
    backend.pbrDirLightIntensity[i] = dev->uniform_location(prog, name);
    if ((backend.pbrDirLightDir[i] < 0) || (backend.pbrDirLightColor[i] < 0) ||
        (backend.pbrDirLightIntensity[i] < 0)) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing directional light uniforms at "
                        "index — lights will be invisible");
    }
  }

  backend.pbrPointLightCountLocation =
      dev->uniform_location(prog, "u_pointLightCount");
  for (std::size_t i = 0U; i < kForwardMaxPointLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].position", i);
    backend.pbrPointLightPos[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].color", i);
    backend.pbrPointLightColor[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].intensity", i);
    backend.pbrPointLightIntensity[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].radius", i);
    backend.pbrPointLightRadius[i] = dev->uniform_location(prog, name);
    if ((backend.pbrPointLightPos[i] < 0) ||
        (backend.pbrPointLightColor[i] < 0) ||
        (backend.pbrPointLightIntensity[i] < 0) ||
        (backend.pbrPointLightRadius[i] < 0)) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing point light uniforms at "
                        "index — lights will be invisible");
    }
  }

  backend.pbrSpotLightCountLocation =
      dev->uniform_location(prog, "u_spotLightCount");
  for (std::size_t i = 0U; i < kForwardMaxSpotLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].position", i);
    backend.pbrSpotLightPos[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].direction", i);
    backend.pbrSpotLightDir[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].color", i);
    backend.pbrSpotLightColor[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].intensity", i);
    backend.pbrSpotLightIntensity[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].radius", i);
    backend.pbrSpotLightRadius[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].innerCone", i);
    backend.pbrSpotLightInnerCone[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].outerCone", i);
    backend.pbrSpotLightOuterCone[i] = dev->uniform_location(prog, name);
  }
}

void resolve_pbr_shadow_uniforms(BackendState &backend,
                                 const RenderDevice *dev) noexcept {
  const std::uint32_t prog = backend.pbrProgram;
  char name[64] = {};

  backend.pbrShadowEnabledLoc = dev->uniform_location(prog, "uShadowEnabled");
  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    std::snprintf(name, sizeof(name), "uShadowMap[%zu]", i);
    backend.pbrShadowMapLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uShadowMatrix[%zu]", i);
    backend.pbrShadowMatrixLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uCascadeSplit[%zu]", i);
    backend.pbrCascadeSplitLocs[i] = dev->uniform_location(prog, name);
  }

  backend.pbrSpotShadowEnabledLoc =
      dev->uniform_location(prog, "uSpotShadowEnabled");
  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    std::snprintf(name, sizeof(name), "uSpotShadowMap[%zu]", i);
    backend.pbrSpotShadowMapLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uSpotShadowMatrix[%zu]", i);
    backend.pbrSpotShadowMatrixLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uSpotShadowLightIdx[%zu]", i);
    backend.pbrSpotShadowLightIdxLocs[i] = dev->uniform_location(prog, name);
  }

  backend.pbrPointShadowEnabledLoc =
      dev->uniform_location(prog, "uPointShadowEnabled");
  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    std::snprintf(name, sizeof(name), "uPointShadowMap[%zu]", i);
    backend.pbrPointShadowMapLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uPointShadowLightPos[%zu]", i);
    backend.pbrPointShadowLightPosLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uPointShadowFarPlane[%zu]", i);
    backend.pbrPointShadowFarPlaneLocs[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "uPointShadowLightIdx[%zu]", i);
    backend.pbrPointShadowLightIdxLocs[i] = dev->uniform_location(prog, name);
  }
}

} // namespace

/// Initializes the owning system for backend.
bool initialize_backend() noexcept {
  BackendState &backend = backend_state();
  if (backend.initialized) {
    return true;
  }

  if (backend.failed) {
    return false;
  }

  if (!initialize_render_device()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to initialize render device");
    reset_backend_on_failure();
    return false;
  }

  if (!initialize_shader_system()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to initialize shader system");
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const RenderDevice *dev = render_device();

  // Load default fallback shader.
  const ShaderProgramHandle defaultShaderHandle = load_configured_shader_program(
      "default.vert", "default.frag");
  if (defaultShaderHandle == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load default shader program");
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t defaultProgram = shader_gpu_program(defaultShaderHandle);
  if (defaultProgram == 0U) {
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  backend.defaultShaderHandle = defaultShaderHandle;
  backend.defaultProgram = defaultProgram;

  // Load PBR shader.
  const ShaderProgramHandle pbrShaderHandle =
      load_configured_shader_program("pbr.vert", "pbr.frag");
  if (pbrShaderHandle == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load PBR shader program");
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t pbrProgram = shader_gpu_program(pbrShaderHandle);
  if (pbrProgram == 0U) {
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  backend.pbrShaderHandle = pbrShaderHandle;
  backend.pbrProgram = pbrProgram;

  // Resolve PBR uniform locations.
  backend.pbrModelLocation = dev->uniform_location(pbrProgram, "u_model");
  backend.pbrMvpLocation = dev->uniform_location(pbrProgram, "u_mvp");
  backend.pbrNormalMatrixLocation =
      dev->uniform_location(pbrProgram, "u_normalMatrix");
  backend.pbrAlbedoLocation = dev->uniform_location(pbrProgram, "u_albedo");
  backend.pbrRoughnessLocation =
      dev->uniform_location(pbrProgram, "u_roughness");
  backend.pbrMetallicLocation = dev->uniform_location(pbrProgram, "u_metallic");
  backend.pbrTimeLocation = dev->uniform_location(pbrProgram, "u_time");
  backend.pbrCameraPosLocation =
      dev->uniform_location(pbrProgram, "u_cameraPos");
  backend.pbrHasAlbedoTextureLocation =
      dev->uniform_location(pbrProgram, "u_hasAlbedoTexture");
  backend.pbrAlbedoMapLocation =
      dev->uniform_location(pbrProgram, "u_albedoMap");
  backend.pbrOpacityLocation = dev->uniform_location(pbrProgram, "u_opacity");
  backend.pbrViewLocation = dev->uniform_location(pbrProgram, "u_viewMatrix");
  backend.pbrViewProjectionLocation =
      dev->uniform_location(pbrProgram, "u_viewProjection");
  backend.pbrUseInstancingLocation =
      dev->uniform_location(pbrProgram, "uUseInstancing");
  backend.pbrFoliageWindStrengthLocation =
      dev->uniform_location(pbrProgram, "uFoliageWindStrength");
  backend.pbrFoliageWindFrequencyLocation =
      dev->uniform_location(pbrProgram, "uFoliageWindFrequency");
  backend.pbrFoliagePhaseLocation =
      dev->uniform_location(pbrProgram, "uFoliagePhase");
  backend.pbrFogModeLocation = dev->uniform_location(pbrProgram, "uFogMode");
  backend.pbrFogStartLocation = dev->uniform_location(pbrProgram, "uFogStart");
  backend.pbrFogEndLocation = dev->uniform_location(pbrProgram, "uFogEnd");
  backend.pbrFogDensityLocation =
      dev->uniform_location(pbrProgram, "uFogDensity");
  backend.pbrFogColorLocation = dev->uniform_location(pbrProgram, "uFogColor");
  backend.pbrHeightFogEnabledLocation =
      dev->uniform_location(pbrProgram, "uHeightFogEnabled");
  backend.pbrHeightFogBaseHeightLocation =
      dev->uniform_location(pbrProgram, "uHeightFogBaseHeight");
  backend.pbrHeightFogDensityLocation =
      dev->uniform_location(pbrProgram, "uHeightFogDensity");
  backend.pbrHeightFogFalloffLocation =
      dev->uniform_location(pbrProgram, "uHeightFogFalloff");
  backend.pbrHeightFogStepCountLocation =
      dev->uniform_location(pbrProgram, "uHeightFogStepCount");

  if ((backend.pbrMvpLocation < 0) || (backend.pbrNormalMatrixLocation < 0) ||
      (backend.pbrAlbedoLocation < 0) || (backend.pbrOpacityLocation < 0) ||
      (backend.pbrViewLocation < 0) ||
      (backend.pbrViewProjectionLocation < 0) ||
      (backend.pbrUseInstancingLocation < 0)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to locate required PBR shader uniforms");
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  resolve_pbr_light_uniforms(backend, dev);
  resolve_pbr_shadow_uniforms(backend, dev);

  // Load tonemap shader.
  const ShaderProgramHandle tonemapShaderHandle = load_configured_shader_program(
      "fullscreen.vert", "tonemap.frag");
  if (tonemapShaderHandle == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load tonemap shader program");
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t tonemapProgram = shader_gpu_program(tonemapShaderHandle);
  if (tonemapProgram == 0U) {
    destroy_shader_program(tonemapShaderHandle);
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  backend.tonemapShaderHandle = tonemapShaderHandle;
  backend.tonemapProgram = tonemapProgram;
  backend.tonemapSceneColorLocation =
      dev->uniform_location(tonemapProgram, "u_sceneColor");
  backend.tonemapExposureLocation =
      dev->uniform_location(tonemapProgram, "u_exposure");
  backend.tonemapOperatorLocation =
      dev->uniform_location(tonemapProgram, "u_tonemapOperator");

  core::cvar_register_int(
      "r_tonemap_operator", 1,
      "Tonemap operator (0=Reinhard, 1=ACES, 2=Uncharted2)");

  // Empty VAO for fullscreen triangle (required by core profile).
  backend.emptyVao = dev->create_vertex_array();
  if (backend.emptyVao == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to create empty VAO for fullscreen pass");
    destroy_shader_program(tonemapShaderHandle);
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  // Skybox shader and cube geometry (soft-fail: clear color remains visible).
  core::cvar_register_string("r_sky_model", "hosek",
                             "Sky model: hosek, preetham, cubemap, or none");
  const ShaderProgramHandle skyboxShader = load_configured_shader_program(
      "skybox.vert", "skybox.frag");
  if (skyboxShader != kInvalidShaderProgram) {
    const std::uint32_t skyboxProgram = shader_gpu_program(skyboxShader);
    if (skyboxProgram != 0U) {
      backend.skyboxShaderHandle = skyboxShader;
      backend.skyboxProgram = skyboxProgram;
      backend.skyboxViewLoc = dev->uniform_location(skyboxProgram, "u_view");
      backend.skyboxProjectionLoc =
          dev->uniform_location(skyboxProgram, "u_projection");
      backend.skyboxTextureLoc =
          dev->uniform_location(skyboxProgram, "u_skybox");

      if ((backend.skyboxViewLoc >= 0) && (backend.skyboxProjectionLoc >= 0) &&
          (backend.skyboxTextureLoc >= 0) &&
          create_skybox_geometry(backend, dev)) {
        backend.skyboxAvailable = true;
      } else {
        core::log_message(core::LogLevel::Warning, "renderer",
                          "skybox setup failed — skybox disabled");
        destroy_skybox_resources(backend);
      }
    } else {
      destroy_shader_program(skyboxShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "skybox shader not available — skybox disabled");
  }

  // Preetham procedural sky (soft-fail: cubemap skybox or clear color remains).
  core::cvar_register_float("r_sky_turbidity", 3.0F,
                            "Preetham sky turbidity (1.7 clear, 10 hazy)");
  const ShaderProgramHandle preethamShader = load_configured_shader_program(
      "skybox.vert", "preetham_sky.frag");
  if (preethamShader != kInvalidShaderProgram) {
    const std::uint32_t preethamProgram = shader_gpu_program(preethamShader);
    if (preethamProgram != 0U) {
      backend.preethamSkyShaderHandle = preethamShader;
      backend.preethamSkyProgram = preethamProgram;
      backend.preethamSkyViewLoc =
          dev->uniform_location(preethamProgram, "u_view");
      backend.preethamSkyProjectionLoc =
          dev->uniform_location(preethamProgram, "u_projection");
      backend.preethamSkySunDirectionLoc =
          dev->uniform_location(preethamProgram, "u_sunDirection");
      backend.preethamSkyTurbidityLoc =
          dev->uniform_location(preethamProgram, "u_turbidity");

      if ((backend.preethamSkyViewLoc >= 0) &&
          (backend.preethamSkyProjectionLoc >= 0) &&
          (backend.preethamSkySunDirectionLoc >= 0) &&
          (backend.preethamSkyTurbidityLoc >= 0) &&
          create_skybox_geometry(backend, dev)) {
        backend.preethamSkyAvailable = true;
      } else {
        core::log_message(
            core::LogLevel::Warning, "renderer",
            "Preetham sky setup failed — procedural sky disabled");
        destroy_preetham_sky_resources(backend);
      }
    } else {
      destroy_shader_program(preethamShader);
    }
  } else {
    core::log_message(
        core::LogLevel::Warning, "renderer",
        "Preetham sky shader not available — procedural sky disabled");
  }

  // Hosek-Wilkie procedural sky (preferred over Preetham when available).
  core::cvar_register_float("r_sky_ground_albedo", 0.1F,
                            "Procedural sky ground albedo");
  const ShaderProgramHandle hosekShader = load_configured_shader_program(
      "skybox.vert", "hosek_wilkie_sky.frag");
  if (hosekShader != kInvalidShaderProgram) {
    const std::uint32_t hosekProgram = shader_gpu_program(hosekShader);
    if (hosekProgram != 0U) {
      backend.hosekSkyShaderHandle = hosekShader;
      backend.hosekSkyProgram = hosekProgram;
      backend.hosekSkyViewLoc = dev->uniform_location(hosekProgram, "u_view");
      backend.hosekSkyProjectionLoc =
          dev->uniform_location(hosekProgram, "u_projection");
      backend.hosekSkySunDirectionLoc =
          dev->uniform_location(hosekProgram, "u_sunDirection");
      backend.hosekSkyTurbidityLoc =
          dev->uniform_location(hosekProgram, "u_turbidity");
      backend.hosekSkyGroundAlbedoLoc =
          dev->uniform_location(hosekProgram, "u_groundAlbedo");

      if ((backend.hosekSkyViewLoc >= 0) &&
          (backend.hosekSkyProjectionLoc >= 0) &&
          (backend.hosekSkySunDirectionLoc >= 0) &&
          (backend.hosekSkyTurbidityLoc >= 0) &&
          (backend.hosekSkyGroundAlbedoLoc >= 0) &&
          create_skybox_geometry(backend, dev)) {
        backend.hosekSkyAvailable = true;
      } else {
        core::log_message(
            core::LogLevel::Warning, "renderer",
            "Hosek-Wilkie sky setup failed — falling back to Preetham");
        destroy_hosek_sky_resources(backend);
      }
    } else {
      destroy_shader_program(hosekShader);
    }
  } else {
    core::log_message(
        core::LogLevel::Warning, "renderer",
        "Hosek-Wilkie sky shader not available — falling back to Preetham");
  }

  // Specular environment prefilter for cubemap IBL.
  core::cvar_register_bool("r_env_prefilter", true,
                           "Bake prefiltered specular cubemap radiance");
  core::cvar_register_int("r_env_prefilter_size", 128,
                          "Prefiltered environment cubemap face size");
  core::cvar_register_int("r_env_prefilter_mips", 5,
                          "Prefiltered environment cubemap mip levels");
  const ShaderProgramHandle prefilterShader =
      load_configured_shader_program("skybox.vert",
                          "prefilter_environment.frag");
  if (prefilterShader != kInvalidShaderProgram) {
    const std::uint32_t prefilterProgram = shader_gpu_program(prefilterShader);
    if (prefilterProgram != 0U) {
      backend.environmentPrefilterShaderHandle = prefilterShader;
      backend.environmentPrefilterProgram = prefilterProgram;
      backend.environmentPrefilterViewLoc =
          dev->uniform_location(prefilterProgram, "u_view");
      backend.environmentPrefilterProjectionLoc =
          dev->uniform_location(prefilterProgram, "u_projection");
      backend.environmentPrefilterTextureLoc =
          dev->uniform_location(prefilterProgram, "u_environmentMap");
      backend.environmentPrefilterRoughnessLoc =
          dev->uniform_location(prefilterProgram, "u_roughness");

      if ((backend.environmentPrefilterViewLoc >= 0) &&
          (backend.environmentPrefilterProjectionLoc >= 0) &&
          (backend.environmentPrefilterTextureLoc >= 0) &&
          (backend.environmentPrefilterRoughnessLoc >= 0) &&
          create_skybox_geometry(backend, dev)) {
        backend.environmentPrefilterAvailable = true;
      } else {
        core::log_message(
            core::LogLevel::Warning, "renderer",
            "environment prefilter setup failed — IBL prefilter disabled");
        destroy_environment_prefilter_resources(backend);
      }
    } else {
      destroy_shader_program(prefilterShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "environment prefilter shader not available");
  }

  // Diffuse irradiance convolution for cubemap IBL.
  core::cvar_register_bool("r_env_irradiance", true,
                           "Bake diffuse irradiance cubemap");
  core::cvar_register_int("r_env_irradiance_size", 32,
                          "Diffuse irradiance cubemap face size");
  const ShaderProgramHandle irradianceShader =
      load_configured_shader_program("skybox.vert",
                          "irradiance_convolution.frag");
  if (irradianceShader != kInvalidShaderProgram) {
    const std::uint32_t irradianceProgram =
        shader_gpu_program(irradianceShader);
    if (irradianceProgram != 0U) {
      backend.environmentIrradianceShaderHandle = irradianceShader;
      backend.environmentIrradianceProgram = irradianceProgram;
      backend.environmentIrradianceViewLoc =
          dev->uniform_location(irradianceProgram, "u_view");
      backend.environmentIrradianceProjectionLoc =
          dev->uniform_location(irradianceProgram, "u_projection");
      backend.environmentIrradianceTextureLoc =
          dev->uniform_location(irradianceProgram, "u_environmentMap");

      if ((backend.environmentIrradianceViewLoc >= 0) &&
          (backend.environmentIrradianceProjectionLoc >= 0) &&
          (backend.environmentIrradianceTextureLoc >= 0) &&
          create_skybox_geometry(backend, dev)) {
        backend.environmentIrradianceAvailable = true;
      } else {
        core::log_message(
            core::LogLevel::Warning, "renderer",
            "environment irradiance setup failed — IBL irradiance disabled");
        destroy_environment_irradiance_resources(backend);
      }
    } else {
      destroy_shader_program(irradianceShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "environment irradiance shader not available");
  }

  // Split-sum BRDF LUT for image-based lighting.
  core::cvar_register_bool("r_env_brdf_lut", true,
                           "Bake split-sum BRDF lookup texture");
  core::cvar_register_int("r_env_brdf_lut_size", 512,
                          "Split-sum BRDF LUT resolution");
  const ShaderProgramHandle brdfLutShader = load_configured_shader_program(
      "fullscreen.vert", "brdf_lut.frag");
  if (brdfLutShader != kInvalidShaderProgram) {
    const std::uint32_t brdfLutProgram = shader_gpu_program(brdfLutShader);
    if (brdfLutProgram != 0U) {
      backend.environmentBrdfLutShaderHandle = brdfLutShader;
      backend.environmentBrdfLutProgram = brdfLutProgram;
      backend.environmentBrdfLutAvailable = true;
    } else {
      destroy_shader_program(brdfLutShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "BRDF LUT shader not available");
  }

  // Register CVars for deferred rendering.
  core::cvar_register_bool("r_deferred", true, "Enable deferred rendering");
  core::cvar_register_int(
      "r_gbuffer_debug", 0,
      "G-Buffer debug mode (0=off, 1=albedo, 2=normals, "
      "3=metallic, 4=roughness, 5=emissive, 6=AO, 7=depth)");
  core::cvar_register_string("r_fog_mode", "exp2",
                             "Distance fog mode: off, linear, exp, exp2");
  core::cvar_register_float("r_fog_start", 25.0F,
                            "Linear distance fog start");
  core::cvar_register_float("r_fog_end", 150.0F,
                            "Linear distance fog end");
  core::cvar_register_float("r_fog_density", 0.01F,
                            "Exponential distance fog density");
  core::cvar_register_string("r_fog_color", "0.55 0.65 0.75",
                             "Distance fog RGB color");
  core::cvar_register_bool("r_height_fog", true, "Enable height fog");
  core::cvar_register_float("r_height_fog_base", 0.0F,
                            "Height fog base world Y");
  core::cvar_register_float("r_height_fog_density", 0.015F,
                            "Height fog density at base height");
  core::cvar_register_float("r_height_fog_falloff", 0.08F,
                            "Height fog exponential falloff above base");
  core::cvar_register_int("r_height_fog_steps", 8,
                          "Height fog ray-march step count");

  // FXAA shader (soft-fail: AA simply disabled if shader unavailable).
  core::cvar_register_bool("r_fxaa", true, "Enable FXAA anti-aliasing");
  const ShaderProgramHandle fxaaShader = load_configured_shader_program(
      "fullscreen.vert", "fxaa.frag");
  if (fxaaShader != kInvalidShaderProgram) {
    const std::uint32_t fxaaProg = shader_gpu_program(fxaaShader);
    if (fxaaProg != 0U) {
      backend.fxaaShaderHandle = fxaaShader;
      backend.fxaaProgram = fxaaProg;
      backend.fxaaInputTextureLocation =
          dev->uniform_location(fxaaProg, "u_inputTexture");
      backend.fxaaTexelSizeLocation =
          dev->uniform_location(fxaaProg, "u_texelSize");
    } else {
      destroy_shader_program(fxaaShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "FXAA shader not available — anti-aliasing disabled");
  }

  // Bloom shaders (soft-fail: bloom simply disabled if shaders unavailable).
  core::cvar_register_bool("r_bloom", true, "Enable bloom");
  core::cvar_register_float("r_bloom_threshold", 1.0F,
                            "Bloom brightness threshold");
  core::cvar_register_float("r_bloom_intensity", 0.3F, "Bloom intensity");
  {
    const ShaderProgramHandle threshShader =
        load_configured_shader_program("fullscreen.vert",
                            "bloom_threshold.frag");
    if (threshShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(threshShader);
      if (prog != 0U) {
        backend.bloomThresholdShaderHandle = threshShader;
        backend.bloomThresholdProgram = prog;
        backend.bloomThreshSceneColorLoc =
            dev->uniform_location(prog, "u_sceneColor");
        backend.bloomThreshThresholdLoc =
            dev->uniform_location(prog, "u_threshold");
      } else {
        destroy_shader_program(threshShader);
      }
    }

    const ShaderProgramHandle downShader =
        load_configured_shader_program("fullscreen.vert",
                            "bloom_downsample.frag");
    if (downShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(downShader);
      if (prog != 0U) {
        backend.bloomDownsampleShaderHandle = downShader;
        backend.bloomDownsampleProgram = prog;
        backend.bloomDownInputLoc = dev->uniform_location(prog, "u_input");
        backend.bloomDownTexelSizeLoc =
            dev->uniform_location(prog, "u_texelSize");
      } else {
        destroy_shader_program(downShader);
      }
    }

    const ShaderProgramHandle upShader = load_configured_shader_program(
        "fullscreen.vert", "bloom_upsample.frag");
    if (upShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(upShader);
      if (prog != 0U) {
        backend.bloomUpsampleShaderHandle = upShader;
        backend.bloomUpsampleProgram = prog;
        backend.bloomUpInputLoc = dev->uniform_location(prog, "u_input");
        backend.bloomUpTexelSizeLoc =
            dev->uniform_location(prog, "u_texelSize");
      } else {
        destroy_shader_program(upShader);
      }
    }

    if (backend.bloomThresholdProgram == 0U ||
        backend.bloomDownsampleProgram == 0U ||
        backend.bloomUpsampleProgram == 0U) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "bloom shaders not fully available — bloom disabled");
    }
  }

  // Resolve tonemap bloom-integration uniforms (tonemap shader already loaded).
  backend.tonemapBloomTextureLoc =
      dev->uniform_location(tonemapProgram, "u_bloomTexture");
  backend.tonemapBloomIntensityLoc =
      dev->uniform_location(tonemapProgram, "u_bloomIntensity");
  backend.tonemapBloomEnabledLoc =
      dev->uniform_location(tonemapProgram, "u_bloomEnabled");

  // SSAO shaders (soft-fail: SSAO simply disabled if shaders unavailable).
  core::cvar_register_bool("r_ssao", true, "Enable SSAO");
  core::cvar_register_float("r_ssao_radius", 0.5F, "SSAO sample radius");
  core::cvar_register_float("r_ssao_bias", 0.025F, "SSAO depth bias");
  {
    const ShaderProgramHandle ssaoShader = load_configured_shader_program(
        "fullscreen.vert", "ssao.frag");
    if (ssaoShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(ssaoShader);
      if (prog != 0U) {
        backend.ssaoShaderHandle = ssaoShader;
        backend.ssaoProgram = prog;
        backend.ssaoDepthLoc = dev->uniform_location(prog, "u_gBufferDepth");
        backend.ssaoNormalLoc = dev->uniform_location(prog, "u_gBufferNormal");
        backend.ssaoNoiseLoc = dev->uniform_location(prog, "u_noiseTexture");
        backend.ssaoProjectionLoc = dev->uniform_location(prog, "u_projection");
        backend.ssaoNoiseScaleLoc = dev->uniform_location(prog, "u_noiseScale");
        backend.ssaoRadiusLoc = dev->uniform_location(prog, "u_radius");
        backend.ssaoBiasLoc = dev->uniform_location(prog, "u_bias");
        for (int i = 0; i < 32; ++i) {
          char nm[64] = {};
          std::snprintf(nm, sizeof(nm), "u_samples[%d]", i);
          backend.ssaoSampleLocs[static_cast<std::size_t>(i)] =
              dev->uniform_location(prog, nm);
        }
      } else {
        destroy_shader_program(ssaoShader);
      }
    }

    const ShaderProgramHandle ssaoBlurShader = load_configured_shader_program(
        "fullscreen.vert", "ssao_blur.frag");
    if (ssaoBlurShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(ssaoBlurShader);
      if (prog != 0U) {
        backend.ssaoBlurShaderHandle = ssaoBlurShader;
        backend.ssaoBlurProgram = prog;
        backend.ssaoBlurInputLoc = dev->uniform_location(prog, "u_ssaoInput");
        backend.ssaoBlurTexelSizeLoc =
            dev->uniform_location(prog, "u_texelSize");
      } else {
        destroy_shader_program(ssaoBlurShader);
      }
    }

    if (backend.ssaoProgram != 0U && backend.ssaoBlurProgram != 0U) {
      backend.ssaoAvailable = true;
      generate_ssao_kernel(backend.ssaoKernel, 32);
      backend.ssaoNoiseTexture = create_ssao_noise_texture();
      if (backend.ssaoNoiseTexture == 0U) {
        core::log_message(core::LogLevel::Warning, "renderer",
                          "SSAO noise texture creation failed — SSAO disabled");
        backend.ssaoAvailable = false;
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "SSAO shaders not fully available — SSAO disabled");
    }
  }

  // Load deferred rendering shaders (soft-fail: falls back to forward).
  bool deferredOk = true;

  const ShaderProgramHandle gbufferShader = load_configured_shader_program(
      "gbuffer.vert", "gbuffer.frag");
  if (gbufferShader == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "G-Buffer shader not available — deferred path disabled");
    deferredOk = false;
  }

  ShaderProgramHandle deferredLightShader{};
  ShaderProgramHandle gbufferDebugShader{};

  if (deferredOk) {
    deferredLightShader =
        load_configured_shader_program("fullscreen.vert",
                            "deferred_lighting.frag");
    if (deferredLightShader == kInvalidShaderProgram) {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "deferred lighting shader not available — deferred path disabled");
      destroy_shader_program(gbufferShader);
      deferredOk = false;
    }
  }

  if (deferredOk) {
    gbufferDebugShader = load_configured_shader_program(
        "fullscreen.vert", "gbuffer_debug.frag");
    if (gbufferDebugShader == kInvalidShaderProgram) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "G-Buffer debug shader not available");
      // Non-fatal: deferred still works without debug viz.
    }
  }

  if (deferredOk) {
    backend.deferredAvailable = true;

    // --- G-Buffer shader uniforms ---
    const auto gbufProg = shader_gpu_program(gbufferShader);
    backend.gbufferShaderHandle = gbufferShader;
    backend.gbufferProgram = gbufProg;
    backend.gbufModelLoc = dev->uniform_location(gbufProg, "uModel");
    backend.gbufViewLoc = dev->uniform_location(gbufProg, "uView");
    backend.gbufProjectionLoc = dev->uniform_location(gbufProg, "uProjection");
    backend.gbufNormalMatrixLoc =
        dev->uniform_location(gbufProg, "uNormalMatrix");
    backend.gbufUseInstancingLoc =
        dev->uniform_location(gbufProg, "uUseInstancing");
    backend.gbufTimeLoc = dev->uniform_location(gbufProg, "uTime");
    backend.gbufFoliageWindStrengthLoc =
        dev->uniform_location(gbufProg, "uFoliageWindStrength");
    backend.gbufFoliageWindFrequencyLoc =
        dev->uniform_location(gbufProg, "uFoliageWindFrequency");
    backend.gbufFoliagePhaseLoc =
        dev->uniform_location(gbufProg, "uFoliagePhase");
    backend.gbufAlbedoLoc = dev->uniform_location(gbufProg, "uAlbedo");
    backend.gbufMetallicLoc = dev->uniform_location(gbufProg, "uMetallic");
    backend.gbufRoughnessLoc = dev->uniform_location(gbufProg, "uRoughness");
    backend.gbufAOLoc = dev->uniform_location(gbufProg, "uAO");
    backend.gbufEmissiveLoc = dev->uniform_location(gbufProg, "uEmissive");

    // --- Deferred lighting shader uniforms ---
    const auto dlProg = shader_gpu_program(deferredLightShader);
    backend.deferredLightShaderHandle = deferredLightShader;
    backend.deferredLightProgram = dlProg;
    backend.dlGBufAlbedoLoc = dev->uniform_location(dlProg, "uGBufferAlbedo");
    backend.dlGBufNormalLoc = dev->uniform_location(dlProg, "uGBufferNormal");
    backend.dlGBufEmissiveLoc =
        dev->uniform_location(dlProg, "uGBufferEmissive");
    backend.dlGBufDepthLoc = dev->uniform_location(dlProg, "uGBufferDepth");
    backend.dlTileLightTexLoc = dev->uniform_location(dlProg, "uTileLightTex");
    backend.dlTileCountXLoc = dev->uniform_location(dlProg, "uTileCountX");
    backend.dlTileCountYLoc = dev->uniform_location(dlProg, "uTileCountY");
    backend.dlInvProjectionLoc =
        dev->uniform_location(dlProg, "uInvProjection");
    backend.dlInvViewLoc = dev->uniform_location(dlProg, "uInvView");
    backend.dlDirLightDirLoc =
        dev->uniform_location(dlProg, "uDirLightDirection");
    backend.dlDirLightColorLoc =
        dev->uniform_location(dlProg, "uDirLightColor");
    backend.dlCameraPosLoc = dev->uniform_location(dlProg, "uCameraPos");
    backend.dlScreenSizeLoc = dev->uniform_location(dlProg, "uScreenSize");
    backend.dlFogModeLoc = dev->uniform_location(dlProg, "uFogMode");
    backend.dlFogStartLoc = dev->uniform_location(dlProg, "uFogStart");
    backend.dlFogEndLoc = dev->uniform_location(dlProg, "uFogEnd");
    backend.dlFogDensityLoc = dev->uniform_location(dlProg, "uFogDensity");
    backend.dlFogColorLoc = dev->uniform_location(dlProg, "uFogColor");
    backend.dlHeightFogEnabledLoc =
        dev->uniform_location(dlProg, "uHeightFogEnabled");
    backend.dlHeightFogBaseHeightLoc =
        dev->uniform_location(dlProg, "uHeightFogBaseHeight");
    backend.dlHeightFogDensityLoc =
        dev->uniform_location(dlProg, "uHeightFogDensity");
    backend.dlHeightFogFalloffLoc =
        dev->uniform_location(dlProg, "uHeightFogFalloff");
    backend.dlHeightFogStepCountLoc =
        dev->uniform_location(dlProg, "uHeightFogStepCount");
    backend.dlPointLightCountLoc =
        dev->uniform_location(dlProg, "uPointLightCount");
    backend.dlSpotLightCountLoc =
        dev->uniform_location(dlProg, "uSpotLightCount");

    // Point light uniform arrays.
    for (std::size_t i = 0U; i < kMaxPointLights; ++i) {
      char nm[80] = {};
      std::snprintf(nm, sizeof(nm), "uPointLightPositions[%zu]", i);
      backend.dlPointPosLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uPointLightColors[%zu]", i);
      backend.dlPointColorLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uPointLightIntensities[%zu]", i);
      backend.dlPointIntensityLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uPointLightRadii[%zu]", i);
      backend.dlPointRadiusLocs[i] = dev->uniform_location(dlProg, nm);
    }

    // Spot light uniform arrays.
    for (std::size_t i = 0U; i < kMaxSpotLights; ++i) {
      char nm[80] = {};
      std::snprintf(nm, sizeof(nm), "uSpotLightPositions[%zu]", i);
      backend.dlSpotPosLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotLightDirections[%zu]", i);
      backend.dlSpotDirLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotLightColors[%zu]", i);
      backend.dlSpotColorLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotLightIntensities[%zu]", i);
      backend.dlSpotIntensityLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotLightRadii[%zu]", i);
      backend.dlSpotRadiusLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotLightInnerCones[%zu]", i);
      backend.dlSpotInnerConeLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotLightOuterCones[%zu]", i);
      backend.dlSpotOuterConeLocs[i] = dev->uniform_location(dlProg, nm);
    }

    // SSAO uniforms in deferred lighting shader.
    backend.dlSsaoTextureLoc = dev->uniform_location(dlProg, "uSsaoTexture");
    backend.dlSsaoEnabledLoc = dev->uniform_location(dlProg, "uSsaoEnabled");

    // Shadow map uniforms in deferred lighting shader.
    backend.dlShadowEnabledLoc =
        dev->uniform_location(dlProg, "uShadowEnabled");
    for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
      char nm[80] = {};
      std::snprintf(nm, sizeof(nm), "uShadowMap[%zu]", i);
      backend.dlShadowMapLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uShadowMatrix[%zu]", i);
      backend.dlShadowMatrixLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uCascadeSplit[%zu]", i);
      backend.dlCascadeSplitLocs[i] = dev->uniform_location(dlProg, nm);
    }

    // Spot shadow uniforms in deferred lighting shader.
    backend.dlSpotShadowEnabledLoc =
        dev->uniform_location(dlProg, "uSpotShadowEnabled");
    for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
      char nm[80] = {};
      std::snprintf(nm, sizeof(nm), "uSpotShadowMap[%zu]", i);
      backend.dlSpotShadowMapLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotShadowMatrix[%zu]", i);
      backend.dlSpotShadowMatrixLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uSpotShadowLightIdx[%zu]", i);
      backend.dlSpotShadowLightIdxLocs[i] = dev->uniform_location(dlProg, nm);
    }

    // Point shadow uniforms in deferred lighting shader.
    backend.dlPointShadowEnabledLoc =
        dev->uniform_location(dlProg, "uPointShadowEnabled");
    for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
      char nm[80] = {};
      std::snprintf(nm, sizeof(nm), "uPointShadowMap[%zu]", i);
      backend.dlPointShadowMapLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uPointShadowLightPos[%zu]", i);
      backend.dlPointShadowLightPosLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uPointShadowFarPlane[%zu]", i);
      backend.dlPointShadowFarPlaneLocs[i] = dev->uniform_location(dlProg, nm);
      std::snprintf(nm, sizeof(nm), "uPointShadowLightIdx[%zu]", i);
      backend.dlPointShadowLightIdxLocs[i] = dev->uniform_location(dlProg, nm);
    }

    // --- G-Buffer debug shader uniforms ---
    if (gbufferDebugShader != kInvalidShaderProgram) {
      const auto dbgProg = shader_gpu_program(gbufferDebugShader);
      backend.gbufferDebugShaderHandle = gbufferDebugShader;
      backend.gbufferDebugProgram = dbgProg;
      backend.dbgGBufAlbedoLoc =
          dev->uniform_location(dbgProg, "uGBufferAlbedo");
      backend.dbgGBufNormalLoc =
          dev->uniform_location(dbgProg, "uGBufferNormal");
      backend.dbgGBufEmissiveLoc =
          dev->uniform_location(dbgProg, "uGBufferEmissive");
      backend.dbgGBufDepthLoc = dev->uniform_location(dbgProg, "uGBufferDepth");
      backend.dbgModeLoc = dev->uniform_location(dbgProg, "uDebugMode");
    }
  }

  // Shadow depth shader (soft-fail: shadows simply disabled).
  core::cvar_register_bool("r_shadows", true, "Enable cascaded shadow maps");
  core::cvar_register_float("r_shadow_lambda", 0.75F,
                            "CSM cascade split blend (0=uniform, 1=log)");
  core::cvar_register_bool("r_shadow_cache", true,
                           "Reuse directional shadow maps when unchanged");
  core::cvar_register_bool(
      "r_shadow_debug", false,
      "Log when shadow casters are dropped due to slot limits");
  {
    const ShaderProgramHandle shadowShader = load_configured_shader_program(
        "shadow_depth.vert", "shadow_depth.frag");
    if (shadowShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(shadowShader);
      if (prog != 0U) {
        backend.shadowDepthShaderHandle = shadowShader;
        backend.shadowDepthProgram = prog;
        backend.shadowLightMvpLoc = dev->uniform_location(prog, "u_lightMVP");
        backend.shadowModelLoc = dev->uniform_location(prog, "u_model");

        if (initialize_shadow_maps(backend.shadowState)) {
          backend.shadowAvailable = true;
        } else {
          core::log_message(
              core::LogLevel::Warning, "renderer",
              "shadow map FBO creation failed — shadows disabled");
        }
      } else {
        destroy_shader_program(shadowShader);
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "shadow depth shader not available — shadows disabled");
    }
  }

  // Spot light shadow maps (soft-fail: spot shadows simply disabled).
  core::cvar_register_bool("r_spot_shadows", true,
                           "Enable spot light shadow maps");
  if (backend.shadowAvailable) {
    if (initialize_spot_shadow_maps(backend.spotShadowState)) {
      backend.spotShadowAvailable = true;
    } else {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "spot shadow FBO creation failed — spot shadows disabled");
    }
  }

  // Point light cubemap shadow maps (soft-fail).
  core::cvar_register_bool("r_point_shadows", true,
                           "Enable point light cubemap shadow maps");
  {
    const ShaderProgramHandle pointShader =
        load_configured_shader_program("shadow_depth_point.vert",
                            "shadow_depth_point.frag");
    if (pointShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(pointShader);
      if (prog != 0U) {
        backend.shadowDepthPointShaderHandle = pointShader;
        backend.shadowDepthPointProgram = prog;
        backend.shadowPointLightMvpLoc =
            dev->uniform_location(prog, "u_lightMVP");
        backend.shadowPointModelLoc = dev->uniform_location(prog, "u_model");
        backend.shadowPointLightPosLoc =
            dev->uniform_location(prog, "u_lightPos");
        backend.shadowPointFarPlaneLoc =
            dev->uniform_location(prog, "u_farPlane");

        if (initialize_point_shadow_maps(backend.pointShadowState)) {
          backend.pointShadowAvailable = true;
        } else {
          core::log_message(core::LogLevel::Warning, "renderer",
                            "point shadow cubemap creation failed — disabled");
        }
      } else {
        destroy_shader_program(pointShader);
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "point shadow shader not available — disabled");
    }
  }

  // Auto-exposure luminance shader (soft-fail: uses manual exposure).
  core::cvar_register_bool("r_auto_exposure", true,
                           "Enable automatic exposure adaptation");
  core::cvar_register_float("r_exposure", 1.0F, "Manual exposure value");
  core::cvar_register_float("r_auto_exposure_speed", 1.5F,
                            "Auto-exposure adaptation speed");
  core::cvar_register_float("r_auto_exposure_min", 0.1F,
                            "Minimum auto-exposure value");
  core::cvar_register_float("r_auto_exposure_max", 10.0F,
                            "Maximum auto-exposure value");
  {
    const ShaderProgramHandle lumShader = load_configured_shader_program(
        "fullscreen.vert", "luminance.frag");
    if (lumShader != kInvalidShaderProgram) {
      const std::uint32_t prog = shader_gpu_program(lumShader);
      if (prog != 0U) {
        backend.luminanceShaderHandle = lumShader;
        backend.luminanceProgram = prog;
        backend.lumSceneColorLoc = dev->uniform_location(prog, "u_sceneColor");
        backend.autoExposureAvailable = true;
      } else {
        destroy_shader_program(lumShader);
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "luminance shader not available — "
                        "auto-exposure disabled");
    }
  }

  initialize_post_process_stack();

  static_cast<void>(initialize_gpu_profiler());
  backend.initialized = true;
  return true;
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
  backend->tileBuffer.clear();

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
