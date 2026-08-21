// Implements the soft-fail deferred-pipeline initialization (cvars,
// G-Buffer, lighting and debug programs with their uniforms) and the
// cascade/spot/point shadow depth programs.
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

// REQUIRED: the transform set, the instancing switch, and the base
// material color. OPTIONAL: time/foliage wind animation, the albedo
// texture path (uHasAlbedoTexture=0 falls back to flat color), and the
// scalar material tuning channels.
bool resolve_gbuffer_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept {
  backend.gbufferProgram = shader_device_program(backend.gbufferShaderHandle);
  const DeviceProgramHandle gbufProg = backend.gbufferProgram;
  if (gbufProg == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.gbufModelLoc = required_param(&ok, dev, gbufProg, "uModel");
  backend.gbufViewLoc = required_param(&ok, dev, gbufProg, "uView");
  backend.gbufProjectionLoc =
      required_param(&ok, dev, gbufProg, "uProjection");
  backend.gbufNormalMatrixLoc =
      required_param(&ok, dev, gbufProg, "uNormalMatrix");
  // Optional: the bgfx shader set omits the instancing toggle until the
  // instancing unit lands, and both backends' flushes gate instanced
  // submission on this param's validity.
  backend.gbufUseInstancingLoc = dev->shader_param(gbufProg, "uUseInstancing");
  backend.gbufTimeLoc = dev->shader_param(gbufProg, "uTime");
  backend.gbufFoliageWindStrengthLoc =
      dev->shader_param(gbufProg, "uFoliageWindStrength");
  backend.gbufFoliageWindFrequencyLoc =
      dev->shader_param(gbufProg, "uFoliageWindFrequency");
  backend.gbufFoliagePhaseLoc =
      dev->shader_param(gbufProg, "uFoliagePhase");
  backend.gbufAlbedoLoc = required_param(&ok, dev, gbufProg, "uAlbedo");
  backend.gbufHasAlbedoTextureLoc =
      dev->shader_param(gbufProg, "uHasAlbedoTexture");
  backend.gbufAlbedoTextureLoc =
      dev->shader_param(gbufProg, "uAlbedoTexture");
  backend.gbufMetallicLoc = dev->shader_param(gbufProg, "uMetallic");
  backend.gbufRoughnessLoc = dev->shader_param(gbufProg, "uRoughness");
  backend.gbufAOLoc = dev->shader_param(gbufProg, "uAO");
  backend.gbufEmissiveLoc = dev->shader_param(gbufProg, "uEmissive");
  // issue #160: texture-backed PBR material slots — all optional, like the
  // albedo texture above (a dropped uniform just means uHasXTexture stays
  // unset and the fragment shader falls back to scalar-only).
  backend.gbufHasMetallicRoughnessTextureLoc =
      dev->shader_param(gbufProg, "uHasMetallicRoughnessTexture");
  backend.gbufMetallicRoughnessTextureLoc =
      dev->shader_param(gbufProg, "uMetallicRoughnessTexture");
  backend.gbufHasEmissiveTextureLoc =
      dev->shader_param(gbufProg, "uHasEmissiveTexture");
  backend.gbufEmissiveTextureLoc =
      dev->shader_param(gbufProg, "uEmissiveTexture");
  backend.gbufHasOcclusionTextureLoc =
      dev->shader_param(gbufProg, "uHasOcclusionTexture");
  backend.gbufOcclusionTextureLoc =
      dev->shader_param(gbufProg, "uOcclusionTexture");
  backend.gbufHasOpacityTextureLoc =
      dev->shader_param(gbufProg, "uHasOpacityTexture");
  backend.gbufOpacityTextureLoc =
      dev->shader_param(gbufProg, "uOpacityTexture");
  backend.gbufAlphaModeLoc = dev->shader_param(gbufProg, "uAlphaMode");
  backend.gbufAlphaCutoffLoc =
      dev->shader_param(gbufProg, "uAlphaCutoff");
  backend.gbufUvTilingLoc = dev->shader_param(gbufProg, "uUvTiling");
  backend.gbufUvOffsetLoc = dev->shader_param(gbufProg, "uUvOffset");
  return ok;
}

// REQUIRED: the four G-Buffer samplers (samplers default to unit 0, so a
// dropped binding silently reads the wrong texture), the tile/light-data
// textures with the X tile count and light counts, the inverse
// view/projection reconstruction matrices, the directional light, and the
// camera position. OPTIONAL: fog and height fog (uFogMode=0 / enable flag
// off), IBL, SSAO, and every shadow family behind their enable flags, plus
// uTileCountY/uScreenSize, which the shader does not read today — a
// conforming compiler may strip them, so requiring them let real drivers
// disable the whole deferred path (issue #95).
bool resolve_deferred_light_program_state(BackendState &backend,
                                          const RenderDevice *dev) noexcept {
  backend.deferredLightProgram =
      shader_device_program(backend.deferredLightShaderHandle);
  const DeviceProgramHandle dlProg = backend.deferredLightProgram;
  if (dlProg == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.dlGBufAlbedoLoc = required_param(&ok, dev, dlProg, "uGBufferAlbedo");
  backend.dlGBufNormalLoc = required_param(&ok, dev, dlProg, "uGBufferNormal");
  backend.dlGBufEmissiveLoc =
      required_param(&ok, dev, dlProg, "uGBufferEmissive");
  backend.dlGBufDepthLoc = required_param(&ok, dev, dlProg, "uGBufferDepth");
  backend.dlTileLightTexLoc =
      required_param(&ok, dev, dlProg, "uTileLightTex");
  backend.dlTileCountXLoc = required_param(&ok, dev, dlProg, "uTileCountX");
  backend.dlTileCountYLoc = dev->shader_param(dlProg, "uTileCountY");
  backend.dlInvProjectionLoc =
      required_param(&ok, dev, dlProg, "uInvProjection");
  backend.dlInvViewLoc = required_param(&ok, dev, dlProg, "uInvView");
  backend.dlDirLightDirLoc =
      required_param(&ok, dev, dlProg, "uDirLightDirection");
  backend.dlDirLightColorLoc =
      required_param(&ok, dev, dlProg, "uDirLightColor");
  backend.dlCameraPosLoc = required_param(&ok, dev, dlProg, "uCameraPos");
  backend.dlCameraForwardOrthoLoc =
      dev->shader_param(dlProg, "uCameraForwardOrtho");
  backend.dlScreenSizeLoc = dev->shader_param(dlProg, "uScreenSize");
  backend.dlFogModeLoc = dev->shader_param(dlProg, "uFogMode");
  backend.dlFogStartLoc = dev->shader_param(dlProg, "uFogStart");
  backend.dlFogEndLoc = dev->shader_param(dlProg, "uFogEnd");
  backend.dlFogDensityLoc = dev->shader_param(dlProg, "uFogDensity");
  backend.dlFogColorLoc = dev->shader_param(dlProg, "uFogColor");
  backend.dlHeightFogEnabledLoc =
      dev->shader_param(dlProg, "uHeightFogEnabled");
  backend.dlHeightFogBaseHeightLoc =
      dev->shader_param(dlProg, "uHeightFogBaseHeight");
  backend.dlHeightFogDensityLoc =
      dev->shader_param(dlProg, "uHeightFogDensity");
  backend.dlHeightFogFalloffLoc =
      dev->shader_param(dlProg, "uHeightFogFalloff");
  backend.dlHeightFogStepCountLoc =
      dev->shader_param(dlProg, "uHeightFogStepCount");
  backend.dlPointLightCountLoc =
      required_param(&ok, dev, dlProg, "uPointLightCount");
  backend.dlSpotLightCountLoc =
      required_param(&ok, dev, dlProg, "uSpotLightCount");
  backend.dlLightDataTexLoc =
      required_param(&ok, dev, dlProg, "uLightDataTex");
  backend.dlIblEnabledLoc = dev->shader_param(dlProg, "uIblEnabled");
  backend.dlIrradianceMapLoc =
      dev->shader_param(dlProg, "uIrradianceMap");
  backend.dlPrefilteredMapLoc =
      dev->shader_param(dlProg, "uPrefilteredMap");
  backend.dlBrdfLutLoc = dev->shader_param(dlProg, "uBrdfLut");
  backend.dlPrefilteredMipsLoc =
      dev->shader_param(dlProg, "uPrefilteredMips");
  backend.dlSsaoTextureLoc = dev->shader_param(dlProg, "uSsaoTexture");
  backend.dlSsaoEnabledLoc = dev->shader_param(dlProg, "uSsaoEnabled");

  backend.dlShadowEnabledLoc =
      dev->shader_param(dlProg, "uShadowEnabled");
  // #138 flat vocabulary (shared with the pbr forward path): per-slot
  // samplers, one mat4 array per shadow kind, packed vec4 payloads.
  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    char nm[80] = {};
    std::snprintf(nm, sizeof(nm), "uShadowMap%zu", i);
    backend.dlShadowMapLocs[i] = dev->shader_param(dlProg, nm);
  }
  backend.dlShadowMatrixParam = dev->shader_param(dlProg, "uShadowMatrix");
  backend.dlCascadeSplitsParam = dev->shader_param(dlProg, "uCascadeSplits");

  backend.dlSpotShadowEnabledLoc =
      dev->shader_param(dlProg, "uSpotShadowEnabled");
  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    char nm[80] = {};
    std::snprintf(nm, sizeof(nm), "uSpotShadowMap%zu", i);
    backend.dlSpotShadowMapLocs[i] = dev->shader_param(dlProg, nm);
  }
  backend.dlSpotShadowMatrixParam =
      dev->shader_param(dlProg, "uSpotShadowMatrix");
  backend.dlSpotShadowLightIdxParam =
      dev->shader_param(dlProg, "uSpotShadowLightIdxVec");

  backend.dlPointShadowEnabledLoc =
      dev->shader_param(dlProg, "uPointShadowEnabled");
  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    char nm[80] = {};
    std::snprintf(nm, sizeof(nm), "uPointShadowMap%zu", i);
    backend.dlPointShadowMapLocs[i] = dev->shader_param(dlProg, nm);
  }
  backend.dlPointShadowPosFarParam =
      dev->shader_param(dlProg, "uPointShadowPosFar");
  backend.dlPointShadowLightIdxParam =
      dev->shader_param(dlProg, "uPointShadowLightIdxVec");
  return ok;
}

// REQUIRED: the four G-Buffer samplers and the mode selector — the
// visualization is meaningless without any of them. On failure the
// cached program id is zeroed because the debug pass gates on it rather
// than on an availability flag.
bool resolve_gbuffer_debug_program_state(BackendState &backend,
                                         const RenderDevice *dev) noexcept {
  backend.gbufferDebugProgram =
      shader_device_program(backend.gbufferDebugShaderHandle);
  const DeviceProgramHandle dbgProg = backend.gbufferDebugProgram;
  if (dbgProg == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.dbgGBufAlbedoLoc =
      required_param(&ok, dev, dbgProg, "uGBufferAlbedo");
  backend.dbgGBufNormalLoc =
      required_param(&ok, dev, dbgProg, "uGBufferNormal");
  backend.dbgGBufEmissiveLoc =
      required_param(&ok, dev, dbgProg, "uGBufferEmissive");
  backend.dbgGBufDepthLoc =
      required_param(&ok, dev, dbgProg, "uGBufferDepth");
  backend.dbgModeLoc = required_param(&ok, dev, dbgProg, "uDebugMode");
  if (!ok) {
    backend.gbufferDebugProgram = kInvalidDeviceProgram;
  }
  return ok;
}

// REQUIRED: u_lightMVP (the CPU pre-multiplies the model matrix into
// it). OPTIONAL: u_model, a compatibility lookup shadow_depth.vert does
// not declare today; the upload is guarded on a resolved location.
bool resolve_shadow_depth_program_state(BackendState &backend,
                                        const RenderDevice *dev) noexcept {
  backend.shadowDepthProgram =
      shader_device_program(backend.shadowDepthShaderHandle);
  const DeviceProgramHandle prog = backend.shadowDepthProgram;
  if (prog == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.shadowLightMvpLoc = required_param(&ok, dev, prog, "u_lightMVP");
  // u_modelMatrix, not u_model: bgfx reserves the latter.
  backend.shadowModelLoc = dev->shader_param(prog, "u_modelMatrix");
  return ok;
}

// REQUIRED: all four uniforms — the cubemap face MVP, the model matrix
// the fragment distance needs, and the light position/far plane the
// depth normalization divides by. No OPTIONAL uniforms.
bool resolve_shadow_depth_point_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept {
  backend.shadowDepthPointProgram =
      shader_device_program(backend.shadowDepthPointShaderHandle);
  const DeviceProgramHandle prog = backend.shadowDepthPointProgram;
  if (prog == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.shadowPointLightMvpLoc =
      required_param(&ok, dev, prog, "u_lightMVP");
  backend.shadowPointModelLoc =
      required_param(&ok, dev, prog, "u_modelMatrix");
  backend.shadowPointLightPosLoc =
      required_param(&ok, dev, prog, "u_lightPos");
  backend.shadowPointFarPlaneLoc =
      required_param(&ok, dev, prog, "u_farPlane");
  return ok;
}

// REQUIRED: the same transform/instancing/base-color set as the static
// G-Buffer resolver plus the BonePalette block binding (skinning is only
// initialized on UBO-capable devices). OPTIONAL: the same animation,
// texture-path, and material-tuning uniforms.
bool resolve_gbuffer_skinned_program_state(BackendState &backend,
                                           const RenderDevice *dev) noexcept {
  backend.gbufferSkinnedProgram =
      shader_device_program(backend.gbufferSkinnedShaderHandle);
  const DeviceProgramHandle skinnedProg = backend.gbufferSkinnedProgram;
  if (skinnedProg == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.gbufSkinnedModelLoc =
      required_param(&ok, dev, skinnedProg, "uModel");
  backend.gbufSkinnedViewLoc =
      required_param(&ok, dev, skinnedProg, "uView");
  backend.gbufSkinnedProjectionLoc =
      required_param(&ok, dev, skinnedProg, "uProjection");
  backend.gbufSkinnedNormalMatrixLoc =
      required_param(&ok, dev, skinnedProg, "uNormalMatrix");
  // Optional for the same reason as the static G-buffer resolver: the
  // flushes gate instanced submission on this param's validity.
  backend.gbufSkinnedUseInstancingLoc =
      dev->shader_param(skinnedProg, "uUseInstancing");
  backend.gbufSkinnedTimeLoc = dev->shader_param(skinnedProg, "uTime");
  backend.gbufSkinnedAlbedoLoc =
      required_param(&ok, dev, skinnedProg, "uAlbedo");
  backend.gbufSkinnedHasAlbedoTextureLoc =
      dev->shader_param(skinnedProg, "uHasAlbedoTexture");
  backend.gbufSkinnedAlbedoTextureLoc =
      dev->shader_param(skinnedProg, "uAlbedoTexture");
  backend.gbufSkinnedMetallicLoc =
      dev->shader_param(skinnedProg, "uMetallic");
  backend.gbufSkinnedRoughnessLoc =
      dev->shader_param(skinnedProg, "uRoughness");
  backend.gbufSkinnedAOLoc = dev->shader_param(skinnedProg, "uAO");
  backend.gbufSkinnedEmissiveLoc =
      dev->shader_param(skinnedProg, "uEmissive");
  // issue #160: texture-backed PBR material slots, same optional-uniform
  // contract as the static G-buffer program above.
  backend.gbufSkinnedHasMetallicRoughnessTextureLoc =
      dev->shader_param(skinnedProg, "uHasMetallicRoughnessTexture");
  backend.gbufSkinnedMetallicRoughnessTextureLoc =
      dev->shader_param(skinnedProg, "uMetallicRoughnessTexture");
  backend.gbufSkinnedHasEmissiveTextureLoc =
      dev->shader_param(skinnedProg, "uHasEmissiveTexture");
  backend.gbufSkinnedEmissiveTextureLoc =
      dev->shader_param(skinnedProg, "uEmissiveTexture");
  backend.gbufSkinnedHasOcclusionTextureLoc =
      dev->shader_param(skinnedProg, "uHasOcclusionTexture");
  backend.gbufSkinnedOcclusionTextureLoc =
      dev->shader_param(skinnedProg, "uOcclusionTexture");
  backend.gbufSkinnedHasOpacityTextureLoc =
      dev->shader_param(skinnedProg, "uHasOpacityTexture");
  backend.gbufSkinnedOpacityTextureLoc =
      dev->shader_param(skinnedProg, "uOpacityTexture");
  backend.gbufSkinnedAlphaModeLoc =
      dev->shader_param(skinnedProg, "uAlphaMode");
  backend.gbufSkinnedAlphaCutoffLoc =
      dev->shader_param(skinnedProg, "uAlphaCutoff");
  backend.gbufSkinnedUvTilingLoc =
      dev->shader_param(skinnedProg, "uUvTiling");
  backend.gbufSkinnedUvOffsetLoc =
      dev->shader_param(skinnedProg, "uUvOffset");
  backend.gbufSkinnedBonesParam = dev->shader_param(skinnedProg, "uBones");
  if (!backend.gbufSkinnedBonesParam.valid()) {
    ok = false;
  }
  return ok;
}

// REQUIRED: u_lightMVP and the BonePalette block binding — without the
// palette the variant renders bind-pose depth, which the static program
// already provides. On failure the cached program id is zeroed because
// skinned shadow draws gate on it rather than on an availability flag.
bool resolve_shadow_depth_skinned_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept {
  backend.shadowDepthSkinnedProgram =
      shader_device_program(backend.shadowDepthSkinnedShaderHandle);
  const DeviceProgramHandle skinnedShadowProg = backend.shadowDepthSkinnedProgram;
  if (skinnedShadowProg == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.shadowSkinnedLightMvpLoc =
      required_param(&ok, dev, skinnedShadowProg, "u_lightMVP");
  backend.shadowSkinnedBonesParam =
      dev->shader_param(skinnedShadowProg, "uBones");
  if (!backend.shadowSkinnedBonesParam.valid()) {
    ok = false;
  }
  if (!ok) {
    backend.shadowDepthSkinnedProgram = kInvalidDeviceProgram;
  }
  return ok;
}

void init_backend_lighting(BackendState &backend,
                           const RenderDevice *dev) noexcept {
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
    backend.gbufferShaderHandle = gbufferShader;
    const bool gbufferUniformsOk = resolve_gbuffer_program_state(backend, dev);

    backend.deferredLightShaderHandle = deferredLightShader;
    const bool deferredLightUniformsOk =
        resolve_deferred_light_program_state(backend, dev);

    if (gbufferUniformsOk && deferredLightUniformsOk) {
      backend.deferredAvailable = true;
      if (gbufferDebugShader != kInvalidShaderProgram) {
        backend.gbufferDebugShaderHandle = gbufferDebugShader;
        static_cast<void>(resolve_gbuffer_debug_program_state(backend, dev));
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "deferred shaders missing required uniforms — "
                        "deferred path disabled");
      if (gbufferDebugShader != kInvalidShaderProgram) {
        destroy_shader_program(gbufferDebugShader);
      }
      destroy_shader_program(deferredLightShader);
      destroy_shader_program(gbufferShader);
      backend.gbufferShaderHandle = ShaderProgramHandle{};
      backend.deferredLightShaderHandle = ShaderProgramHandle{};
      backend.gbufferProgram = kInvalidDeviceProgram;
      backend.deferredLightProgram = kInvalidDeviceProgram;
      deferredOk = false;
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
      backend.shadowDepthShaderHandle = shadowShader;
      if (resolve_shadow_depth_program_state(backend, dev)) {
        if (initialize_shadow_maps(backend.shadowState)) {
          backend.shadowAvailable = true;
        } else {
          core::log_message(
              core::LogLevel::Warning, "renderer",
              "shadow map FBO creation failed — shadows disabled");
        }
      } else {
        backend.shadowDepthShaderHandle = ShaderProgramHandle{};
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
      backend.shadowDepthPointShaderHandle = pointShader;
      if (resolve_shadow_depth_point_program_state(backend, dev)) {
        if (initialize_point_shadow_maps(backend.pointShadowState)) {
          backend.pointShadowAvailable = true;
        } else {
          core::log_message(core::LogLevel::Warning, "renderer",
                            "point shadow cubemap creation failed — disabled");
        }
      } else {
        backend.shadowDepthPointShaderHandle = ShaderProgramHandle{};
        destroy_shader_program(pointShader);
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "point shadow shader not available — disabled");
    }
  }

  // GPU skinning (soft-fail: skinned meshes render in bind pose). The
  // #138 shared vocabulary uploads palettes as plain mat4 arrays into
  // each skinned program, so no uniform buffer (and no
  // caps.uniformBlocks dependency) remains.
  {
    if (backend.deferredAvailable && (dev->set_param_mat4_array != nullptr)) {
      const ShaderDefine skinnedDefine{"SKINNED", "1"};
      const ShaderProgramHandle skinnedGbufferShader =
          load_configured_shader_variant("gbuffer.vert", "gbuffer.frag",
                                         &skinnedDefine, 1U);
      backend.gbufferSkinnedShaderHandle = skinnedGbufferShader;
      if (resolve_gbuffer_skinned_program_state(backend, dev)) {
        backend.skinningAvailable = true;

        if (backend.skinningAvailable && backend.shadowAvailable) {
          const ShaderProgramHandle skinnedShadowShader =
              load_configured_shader_variant("shadow_depth.vert",
                                             "shadow_depth.frag",
                                             &skinnedDefine, 1U);
          backend.shadowDepthSkinnedShaderHandle = skinnedShadowShader;
          if (!resolve_shadow_depth_skinned_program_state(backend, dev)) {
            if (skinnedShadowShader != kInvalidShaderProgram) {
              destroy_shader_program(skinnedShadowShader);
            }
            backend.shadowDepthSkinnedShaderHandle = ShaderProgramHandle{};
            backend.shadowDepthSkinnedProgram = kInvalidDeviceProgram;
            core::log_message(core::LogLevel::Warning, "renderer",
                              "skinned shadow shader not available — skinned "
                              "meshes cast bind-pose shadows");
          }
        }
      } else {
        if (skinnedGbufferShader != kInvalidShaderProgram) {
          destroy_shader_program(skinnedGbufferShader);
        }
        backend.gbufferSkinnedShaderHandle = ShaderProgramHandle{};
        backend.gbufferSkinnedProgram = kInvalidDeviceProgram;
        core::log_message(core::LogLevel::Warning, "renderer",
                          "skinned G-buffer shader not available — GPU "
                          "skinning disabled");
      }
    }
  }

}

} // namespace engine::renderer
