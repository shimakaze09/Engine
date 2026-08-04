// Implements the hard-fail backend core initialization: render device,
// shader system, the default/PBR/tonemap programs with their required
// uniforms, and the fullscreen empty VAO.
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

bool resolve_default_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  static_cast<void>(dev);
  backend.defaultProgram = shader_gpu_program(backend.defaultShaderHandle);
  return backend.defaultProgram != 0U;
}

// REQUIRED: transforms, instancing switch, core material color/opacity,
// and the camera position the specular terms need. OPTIONAL: textures
// (flat-color fallback via u_hasAlbedoTexture=0), material tuning, time/
// foliage animation, fog (uFogMode=0 disables), lights (warned per slot),
// IBL and every shadow family (their enable flags default to off).
bool resolve_pbr_program_state(BackendState &backend,
                               const RenderDevice *dev) noexcept {
  backend.pbrProgram = shader_gpu_program(backend.pbrShaderHandle);
  const std::uint32_t pbrProgram = backend.pbrProgram;
  if (pbrProgram == 0U) {
    return false;
  }

  bool ok = true;
  backend.pbrModelLocation =
      required_location(&ok, dev, pbrProgram, "u_model");
  backend.pbrMvpLocation = required_location(&ok, dev, pbrProgram, "u_mvp");
  backend.pbrNormalMatrixLocation =
      required_location(&ok, dev, pbrProgram, "u_normalMatrix");
  backend.pbrAlbedoLocation =
      required_location(&ok, dev, pbrProgram, "u_albedo");
  backend.pbrRoughnessLocation =
      dev->uniform_location(pbrProgram, "u_roughness");
  backend.pbrMetallicLocation = dev->uniform_location(pbrProgram, "u_metallic");
  backend.pbrTimeLocation = dev->uniform_location(pbrProgram, "u_time");
  backend.pbrCameraPosLocation =
      required_location(&ok, dev, pbrProgram, "u_cameraPos");
  backend.pbrHasAlbedoTextureLocation =
      dev->uniform_location(pbrProgram, "u_hasAlbedoTexture");
  backend.pbrAlbedoMapLocation =
      dev->uniform_location(pbrProgram, "u_albedoMap");
  backend.pbrOpacityLocation =
      required_location(&ok, dev, pbrProgram, "u_opacity");
  backend.pbrViewLocation =
      required_location(&ok, dev, pbrProgram, "u_viewMatrix");
  backend.pbrViewProjectionLocation =
      required_location(&ok, dev, pbrProgram, "u_viewProjection");
  backend.pbrUseInstancingLocation =
      required_location(&ok, dev, pbrProgram, "uUseInstancing");
  backend.pbrIblEnabledLoc = dev->uniform_location(pbrProgram, "uIblEnabled");
  backend.pbrIrradianceMapLoc =
      dev->uniform_location(pbrProgram, "uIrradianceMap");
  backend.pbrPrefilteredMapLoc =
      dev->uniform_location(pbrProgram, "uPrefilteredMap");
  backend.pbrBrdfLutLoc = dev->uniform_location(pbrProgram, "uBrdfLut");
  backend.pbrPrefilteredMipsLoc =
      dev->uniform_location(pbrProgram, "uPrefilteredMips");
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

  resolve_pbr_light_uniforms(backend, dev);
  resolve_pbr_shadow_uniforms(backend, dev);

  return ok;
}

// REQUIRED: the scene-color sampler and exposure (a dropped u_exposure
// upload reads as 0 and blacks the frame). OPTIONAL: the operator
// (0 = Reinhard is valid) and the bloom-composite trio, which
// u_bloomEnabled=0 keeps inert.
bool resolve_tonemap_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  backend.tonemapProgram = shader_gpu_program(backend.tonemapShaderHandle);
  const std::uint32_t tonemapProgram = backend.tonemapProgram;
  if (tonemapProgram == 0U) {
    return false;
  }
  bool ok = true;
  backend.tonemapSceneColorLocation =
      required_location(&ok, dev, tonemapProgram, "u_sceneColor");
  backend.tonemapExposureLocation =
      required_location(&ok, dev, tonemapProgram, "u_exposure");
  backend.tonemapOperatorLocation =
      dev->uniform_location(tonemapProgram, "u_tonemapOperator");
  backend.tonemapBloomTextureLoc =
      dev->uniform_location(tonemapProgram, "u_bloomTexture");
  backend.tonemapBloomIntensityLoc =
      dev->uniform_location(tonemapProgram, "u_bloomIntensity");
  backend.tonemapBloomEnabledLoc =
      dev->uniform_location(tonemapProgram, "u_bloomEnabled");
  return ok;
}

bool init_backend_core(BackendState &backend) noexcept {
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

  backend.defaultShaderHandle = defaultShaderHandle;
  if (!resolve_default_program_state(backend, dev)) {
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

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

  backend.pbrShaderHandle = pbrShaderHandle;
  if (!resolve_pbr_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to locate required PBR shader uniforms");
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

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

  backend.tonemapShaderHandle = tonemapShaderHandle;
  if (!resolve_tonemap_program_state(backend, dev)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to locate required tonemap shader uniforms");
    destroy_shader_program(tonemapShaderHandle);
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

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
  return true;
}

} // namespace engine::renderer
