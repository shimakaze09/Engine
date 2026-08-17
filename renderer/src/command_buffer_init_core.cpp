// Implements the hard-fail backend core initialization: render device,
// shader system, the default/PBR/tonemap programs with their required
// shader parameters, and the fullscreen attribute-less geometry.
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
  const DeviceProgramHandle prog = backend.pbrProgram;

  backend.pbrDirLightCountLocation =
      dev->shader_param(prog, "u_dirLightCount");
  for (std::size_t i = 0U; i < kMaxDirectionalLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].direction", i);
    backend.pbrDirLightDir[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].color", i);
    backend.pbrDirLightColor[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].intensity", i);
    backend.pbrDirLightIntensity[i] = dev->shader_param(prog, name);
    if ((!backend.pbrDirLightDir[i].valid()) || (!backend.pbrDirLightColor[i].valid()) ||
        (!backend.pbrDirLightIntensity[i].valid())) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing directional light uniforms at "
                        "index — lights will be invisible");
    }
  }

  backend.pbrPointLightCountLocation =
      dev->shader_param(prog, "u_pointLightCount");
  for (std::size_t i = 0U; i < kForwardMaxPointLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].position", i);
    backend.pbrPointLightPos[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].color", i);
    backend.pbrPointLightColor[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].intensity", i);
    backend.pbrPointLightIntensity[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].radius", i);
    backend.pbrPointLightRadius[i] = dev->shader_param(prog, name);
    if ((!backend.pbrPointLightPos[i].valid()) ||
        (!backend.pbrPointLightColor[i].valid()) ||
        (!backend.pbrPointLightIntensity[i].valid()) ||
        (!backend.pbrPointLightRadius[i].valid())) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing point light uniforms at "
                        "index — lights will be invisible");
    }
  }

  backend.pbrSpotLightCountLocation =
      dev->shader_param(prog, "u_spotLightCount");
  for (std::size_t i = 0U; i < kForwardMaxSpotLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].position", i);
    backend.pbrSpotLightPos[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].direction", i);
    backend.pbrSpotLightDir[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].color", i);
    backend.pbrSpotLightColor[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].intensity", i);
    backend.pbrSpotLightIntensity[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].radius", i);
    backend.pbrSpotLightRadius[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].innerCone", i);
    backend.pbrSpotLightInnerCone[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "u_spotLights[%zu].outerCone", i);
    backend.pbrSpotLightOuterCone[i] = dev->shader_param(prog, name);
  }
}

void resolve_pbr_shadow_uniforms(BackendState &backend,
                                 const RenderDevice *dev) noexcept {
  const DeviceProgramHandle prog = backend.pbrProgram;
  char name[64] = {};

  backend.pbrShadowEnabledLoc = dev->shader_param(prog, "uShadowEnabled");
  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    std::snprintf(name, sizeof(name), "uShadowMap[%zu]", i);
    backend.pbrShadowMapLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uShadowMatrix[%zu]", i);
    backend.pbrShadowMatrixLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uCascadeSplit[%zu]", i);
    backend.pbrCascadeSplitLocs[i] = dev->shader_param(prog, name);
  }

  backend.pbrSpotShadowEnabledLoc =
      dev->shader_param(prog, "uSpotShadowEnabled");
  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    std::snprintf(name, sizeof(name), "uSpotShadowMap[%zu]", i);
    backend.pbrSpotShadowMapLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uSpotShadowMatrix[%zu]", i);
    backend.pbrSpotShadowMatrixLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uSpotShadowLightIdx[%zu]", i);
    backend.pbrSpotShadowLightIdxLocs[i] = dev->shader_param(prog, name);
  }

  backend.pbrPointShadowEnabledLoc =
      dev->shader_param(prog, "uPointShadowEnabled");
  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    std::snprintf(name, sizeof(name), "uPointShadowMap[%zu]", i);
    backend.pbrPointShadowMapLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uPointShadowLightPos[%zu]", i);
    backend.pbrPointShadowLightPosLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uPointShadowFarPlane[%zu]", i);
    backend.pbrPointShadowFarPlaneLocs[i] = dev->shader_param(prog, name);
    std::snprintf(name, sizeof(name), "uPointShadowLightIdx[%zu]", i);
    backend.pbrPointShadowLightIdxLocs[i] = dev->shader_param(prog, name);
  }
}

} // namespace

bool resolve_default_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  static_cast<void>(dev);
  backend.defaultProgram = shader_device_program(backend.defaultShaderHandle);
  return backend.defaultProgram != kInvalidDeviceProgram;
}

// REQUIRED: transforms, instancing switch, core material color/opacity,
// and the camera position the specular terms need. OPTIONAL: textures
// (flat-color fallback via u_hasAlbedoTexture=0), material tuning, time/
// foliage animation, fog (uFogMode=0 disables), lights (warned per slot),
// IBL and every shadow family (their enable flags default to off).
bool resolve_pbr_program_state(BackendState &backend,
                               const RenderDevice *dev) noexcept {
  backend.pbrProgram = shader_device_program(backend.pbrShaderHandle);
  const DeviceProgramHandle pbrProgram = backend.pbrProgram;
  if (pbrProgram == kInvalidDeviceProgram) {
    return false;
  }

  bool ok = true;
  backend.pbrModelLocation =
      required_param(&ok, dev, pbrProgram, "u_model");
  backend.pbrMvpLocation = required_param(&ok, dev, pbrProgram, "u_mvp");
  backend.pbrNormalMatrixLocation =
      required_param(&ok, dev, pbrProgram, "u_normalMatrix");
  backend.pbrAlbedoLocation =
      required_param(&ok, dev, pbrProgram, "u_albedo");
  backend.pbrRoughnessLocation =
      dev->shader_param(pbrProgram, "u_roughness");
  backend.pbrMetallicLocation = dev->shader_param(pbrProgram, "u_metallic");
  backend.pbrTimeLocation = dev->shader_param(pbrProgram, "u_time");
  backend.pbrCameraPosLocation =
      required_param(&ok, dev, pbrProgram, "u_cameraPos");
  backend.pbrHasAlbedoTextureLocation =
      dev->shader_param(pbrProgram, "u_hasAlbedoTexture");
  backend.pbrAlbedoMapLocation =
      dev->shader_param(pbrProgram, "u_albedoMap");
  backend.pbrOpacityLocation =
      required_param(&ok, dev, pbrProgram, "u_opacity");
  backend.pbrViewLocation =
      required_param(&ok, dev, pbrProgram, "u_viewMatrix");
  backend.pbrViewProjectionLocation =
      required_param(&ok, dev, pbrProgram, "u_viewProjection");
  backend.pbrUseInstancingLocation =
      required_param(&ok, dev, pbrProgram, "uUseInstancing");
  backend.pbrIblEnabledLoc = dev->shader_param(pbrProgram, "uIblEnabled");
  backend.pbrIrradianceMapLoc =
      dev->shader_param(pbrProgram, "uIrradianceMap");
  backend.pbrPrefilteredMapLoc =
      dev->shader_param(pbrProgram, "uPrefilteredMap");
  backend.pbrBrdfLutLoc = dev->shader_param(pbrProgram, "uBrdfLut");
  backend.pbrPrefilteredMipsLoc =
      dev->shader_param(pbrProgram, "uPrefilteredMips");
  backend.pbrFoliageWindStrengthLocation =
      dev->shader_param(pbrProgram, "uFoliageWindStrength");
  backend.pbrFoliageWindFrequencyLocation =
      dev->shader_param(pbrProgram, "uFoliageWindFrequency");
  backend.pbrFoliagePhaseLocation =
      dev->shader_param(pbrProgram, "uFoliagePhase");
  backend.pbrFogModeLocation = dev->shader_param(pbrProgram, "uFogMode");
  backend.pbrFogStartLocation = dev->shader_param(pbrProgram, "uFogStart");
  backend.pbrFogEndLocation = dev->shader_param(pbrProgram, "uFogEnd");
  backend.pbrFogDensityLocation =
      dev->shader_param(pbrProgram, "uFogDensity");
  backend.pbrFogColorLocation = dev->shader_param(pbrProgram, "uFogColor");
  backend.pbrHeightFogEnabledLocation =
      dev->shader_param(pbrProgram, "uHeightFogEnabled");
  backend.pbrHeightFogBaseHeightLocation =
      dev->shader_param(pbrProgram, "uHeightFogBaseHeight");
  backend.pbrHeightFogDensityLocation =
      dev->shader_param(pbrProgram, "uHeightFogDensity");
  backend.pbrHeightFogFalloffLocation =
      dev->shader_param(pbrProgram, "uHeightFogFalloff");
  backend.pbrHeightFogStepCountLocation =
      dev->shader_param(pbrProgram, "uHeightFogStepCount");

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
  backend.tonemapProgram = shader_device_program(backend.tonemapShaderHandle);
  const DeviceProgramHandle tonemapProgram = backend.tonemapProgram;
  if (tonemapProgram == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.tonemapSceneColorLocation =
      required_param(&ok, dev, tonemapProgram, "u_sceneColor");
  backend.tonemapExposureLocation =
      required_param(&ok, dev, tonemapProgram, "u_exposure");
  backend.tonemapOperatorLocation =
      dev->shader_param(tonemapProgram, "u_tonemapOperator");
  backend.tonemapBloomTextureLoc =
      dev->shader_param(tonemapProgram, "u_bloomTexture");
  backend.tonemapBloomIntensityLoc =
      dev->shader_param(tonemapProgram, "u_bloomIntensity");
  backend.tonemapBloomEnabledLoc =
      dev->shader_param(tonemapProgram, "u_bloomEnabled");
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

  // Attribute-less geometry for fullscreen triangles (the vertex shader
  // synthesizes positions from gl_VertexID-style indices).
  backend.emptyGeometry = (dev->create_geometry != nullptr)
                              ? dev->create_geometry(GeometryDesc{})
                              : kInvalidDeviceGeometry;
  if (backend.emptyGeometry == kInvalidDeviceGeometry) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to create fullscreen-pass geometry");
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
