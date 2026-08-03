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
  backend.gbufferProgram = shader_gpu_program(backend.gbufferShaderHandle);
  const std::uint32_t gbufProg = backend.gbufferProgram;
  if (gbufProg == 0U) {
    return false;
  }
  bool ok = true;
  backend.gbufModelLoc = required_location(&ok, dev, gbufProg, "uModel");
  backend.gbufViewLoc = required_location(&ok, dev, gbufProg, "uView");
  backend.gbufProjectionLoc =
      required_location(&ok, dev, gbufProg, "uProjection");
  backend.gbufNormalMatrixLoc =
      required_location(&ok, dev, gbufProg, "uNormalMatrix");
  backend.gbufUseInstancingLoc =
      required_location(&ok, dev, gbufProg, "uUseInstancing");
  backend.gbufTimeLoc = dev->uniform_location(gbufProg, "uTime");
  backend.gbufFoliageWindStrengthLoc =
      dev->uniform_location(gbufProg, "uFoliageWindStrength");
  backend.gbufFoliageWindFrequencyLoc =
      dev->uniform_location(gbufProg, "uFoliageWindFrequency");
  backend.gbufFoliagePhaseLoc =
      dev->uniform_location(gbufProg, "uFoliagePhase");
  backend.gbufAlbedoLoc = required_location(&ok, dev, gbufProg, "uAlbedo");
  backend.gbufHasAlbedoTextureLoc =
      dev->uniform_location(gbufProg, "uHasAlbedoTexture");
  backend.gbufAlbedoTextureLoc =
      dev->uniform_location(gbufProg, "uAlbedoTexture");
  backend.gbufMetallicLoc = dev->uniform_location(gbufProg, "uMetallic");
  backend.gbufRoughnessLoc = dev->uniform_location(gbufProg, "uRoughness");
  backend.gbufAOLoc = dev->uniform_location(gbufProg, "uAO");
  backend.gbufEmissiveLoc = dev->uniform_location(gbufProg, "uEmissive");
  return ok;
}

// REQUIRED: the four G-Buffer samplers (samplers default to unit 0, so a
// dropped binding silently reads the wrong texture), the tile/light-data
// textures with their counts, the inverse view/projection
// reconstruction matrices, the directional light, camera position, and
// screen size. OPTIONAL: fog and height fog (uFogMode=0 / enable flag
// off), IBL, SSAO, and every shadow family behind their enable flags.
bool resolve_deferred_light_program_state(BackendState &backend,
                                          const RenderDevice *dev) noexcept {
  backend.deferredLightProgram =
      shader_gpu_program(backend.deferredLightShaderHandle);
  const std::uint32_t dlProg = backend.deferredLightProgram;
  if (dlProg == 0U) {
    return false;
  }
  bool ok = true;
  backend.dlGBufAlbedoLoc = required_location(&ok, dev, dlProg, "uGBufferAlbedo");
  backend.dlGBufNormalLoc = required_location(&ok, dev, dlProg, "uGBufferNormal");
  backend.dlGBufEmissiveLoc =
      required_location(&ok, dev, dlProg, "uGBufferEmissive");
  backend.dlGBufDepthLoc = required_location(&ok, dev, dlProg, "uGBufferDepth");
  backend.dlTileLightTexLoc =
      required_location(&ok, dev, dlProg, "uTileLightTex");
  backend.dlTileCountXLoc = required_location(&ok, dev, dlProg, "uTileCountX");
  backend.dlTileCountYLoc = required_location(&ok, dev, dlProg, "uTileCountY");
  backend.dlInvProjectionLoc =
      required_location(&ok, dev, dlProg, "uInvProjection");
  backend.dlInvViewLoc = required_location(&ok, dev, dlProg, "uInvView");
  backend.dlDirLightDirLoc =
      required_location(&ok, dev, dlProg, "uDirLightDirection");
  backend.dlDirLightColorLoc =
      required_location(&ok, dev, dlProg, "uDirLightColor");
  backend.dlCameraPosLoc = required_location(&ok, dev, dlProg, "uCameraPos");
  backend.dlScreenSizeLoc = required_location(&ok, dev, dlProg, "uScreenSize");
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
      required_location(&ok, dev, dlProg, "uPointLightCount");
  backend.dlSpotLightCountLoc =
      required_location(&ok, dev, dlProg, "uSpotLightCount");
  backend.dlLightDataTexLoc =
      required_location(&ok, dev, dlProg, "uLightDataTex");
  backend.dlIblEnabledLoc = dev->uniform_location(dlProg, "uIblEnabled");
  backend.dlIrradianceMapLoc =
      dev->uniform_location(dlProg, "uIrradianceMap");
  backend.dlPrefilteredMapLoc =
      dev->uniform_location(dlProg, "uPrefilteredMap");
  backend.dlBrdfLutLoc = dev->uniform_location(dlProg, "uBrdfLut");
  backend.dlPrefilteredMipsLoc =
      dev->uniform_location(dlProg, "uPrefilteredMips");
  backend.dlSsaoTextureLoc = dev->uniform_location(dlProg, "uSsaoTexture");
  backend.dlSsaoEnabledLoc = dev->uniform_location(dlProg, "uSsaoEnabled");

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
  return ok;
}

// REQUIRED: the four G-Buffer samplers and the mode selector — the
// visualization is meaningless without any of them. On failure the
// cached program id is zeroed because the debug pass gates on it rather
// than on an availability flag.
bool resolve_gbuffer_debug_program_state(BackendState &backend,
                                         const RenderDevice *dev) noexcept {
  backend.gbufferDebugProgram =
      shader_gpu_program(backend.gbufferDebugShaderHandle);
  const std::uint32_t dbgProg = backend.gbufferDebugProgram;
  if (dbgProg == 0U) {
    return false;
  }
  bool ok = true;
  backend.dbgGBufAlbedoLoc =
      required_location(&ok, dev, dbgProg, "uGBufferAlbedo");
  backend.dbgGBufNormalLoc =
      required_location(&ok, dev, dbgProg, "uGBufferNormal");
  backend.dbgGBufEmissiveLoc =
      required_location(&ok, dev, dbgProg, "uGBufferEmissive");
  backend.dbgGBufDepthLoc =
      required_location(&ok, dev, dbgProg, "uGBufferDepth");
  backend.dbgModeLoc = required_location(&ok, dev, dbgProg, "uDebugMode");
  if (!ok) {
    backend.gbufferDebugProgram = 0U;
  }
  return ok;
}

// REQUIRED: u_lightMVP (the CPU pre-multiplies the model matrix into
// it). OPTIONAL: u_model, a compatibility lookup shadow_depth.vert does
// not declare today; the upload is guarded on a resolved location.
bool resolve_shadow_depth_program_state(BackendState &backend,
                                        const RenderDevice *dev) noexcept {
  backend.shadowDepthProgram =
      shader_gpu_program(backend.shadowDepthShaderHandle);
  const std::uint32_t prog = backend.shadowDepthProgram;
  if (prog == 0U) {
    return false;
  }
  bool ok = true;
  backend.shadowLightMvpLoc = required_location(&ok, dev, prog, "u_lightMVP");
  backend.shadowModelLoc = dev->uniform_location(prog, "u_model");
  return ok;
}

// REQUIRED: all four uniforms — the cubemap face MVP, the model matrix
// the fragment distance needs, and the light position/far plane the
// depth normalization divides by. No OPTIONAL uniforms.
bool resolve_shadow_depth_point_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept {
  backend.shadowDepthPointProgram =
      shader_gpu_program(backend.shadowDepthPointShaderHandle);
  const std::uint32_t prog = backend.shadowDepthPointProgram;
  if (prog == 0U) {
    return false;
  }
  bool ok = true;
  backend.shadowPointLightMvpLoc =
      required_location(&ok, dev, prog, "u_lightMVP");
  backend.shadowPointModelLoc = required_location(&ok, dev, prog, "u_model");
  backend.shadowPointLightPosLoc =
      required_location(&ok, dev, prog, "u_lightPos");
  backend.shadowPointFarPlaneLoc =
      required_location(&ok, dev, prog, "u_farPlane");
  return ok;
}

// REQUIRED: the same transform/instancing/base-color set as the static
// G-Buffer resolver plus the BonePalette block binding (skinning is only
// initialized on UBO-capable devices). OPTIONAL: the same animation,
// texture-path, and material-tuning uniforms.
bool resolve_gbuffer_skinned_program_state(BackendState &backend,
                                           const RenderDevice *dev) noexcept {
  backend.gbufferSkinnedProgram =
      shader_gpu_program(backend.gbufferSkinnedShaderHandle);
  const std::uint32_t skinnedProg = backend.gbufferSkinnedProgram;
  if (skinnedProg == 0U) {
    return false;
  }
  bool ok = true;
  backend.gbufSkinnedModelLoc =
      required_location(&ok, dev, skinnedProg, "uModel");
  backend.gbufSkinnedViewLoc =
      required_location(&ok, dev, skinnedProg, "uView");
  backend.gbufSkinnedProjectionLoc =
      required_location(&ok, dev, skinnedProg, "uProjection");
  backend.gbufSkinnedNormalMatrixLoc =
      required_location(&ok, dev, skinnedProg, "uNormalMatrix");
  backend.gbufSkinnedUseInstancingLoc =
      required_location(&ok, dev, skinnedProg, "uUseInstancing");
  backend.gbufSkinnedTimeLoc = dev->uniform_location(skinnedProg, "uTime");
  backend.gbufSkinnedAlbedoLoc =
      required_location(&ok, dev, skinnedProg, "uAlbedo");
  backend.gbufSkinnedHasAlbedoTextureLoc =
      dev->uniform_location(skinnedProg, "uHasAlbedoTexture");
  backend.gbufSkinnedAlbedoTextureLoc =
      dev->uniform_location(skinnedProg, "uAlbedoTexture");
  backend.gbufSkinnedMetallicLoc =
      dev->uniform_location(skinnedProg, "uMetallic");
  backend.gbufSkinnedRoughnessLoc =
      dev->uniform_location(skinnedProg, "uRoughness");
  backend.gbufSkinnedAOLoc = dev->uniform_location(skinnedProg, "uAO");
  backend.gbufSkinnedEmissiveLoc =
      dev->uniform_location(skinnedProg, "uEmissive");
  if ((dev->bind_uniform_block == nullptr) ||
      !dev->bind_uniform_block(skinnedProg, "BonePalette",
                               kBonePaletteUboBinding)) {
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
      shader_gpu_program(backend.shadowDepthSkinnedShaderHandle);
  const std::uint32_t skinnedShadowProg = backend.shadowDepthSkinnedProgram;
  if (skinnedShadowProg == 0U) {
    return false;
  }
  bool ok = true;
  backend.shadowSkinnedLightMvpLoc =
      required_location(&ok, dev, skinnedShadowProg, "u_lightMVP");
  if ((dev->bind_uniform_block == nullptr) ||
      !dev->bind_uniform_block(skinnedShadowProg, "BonePalette",
                               kBonePaletteUboBinding)) {
    ok = false;
  }
  if (!ok) {
    backend.shadowDepthSkinnedProgram = 0U;
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
      backend.gbufferProgram = 0U;
      backend.deferredLightProgram = 0U;
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
  // skinned G-buffer and shadow-depth variants share one bone-palette
  // uniform buffer rebound per skinned draw.
  {
    const bool uboSupported = (dev->bind_uniform_buffer != nullptr) &&
                              (dev->buffer_data_uniform != nullptr) &&
                              (dev->buffer_sub_data_uniform != nullptr) &&
                              (dev->bind_uniform_buffer_base != nullptr) &&
                              (dev->bind_uniform_block != nullptr);
    if (backend.deferredAvailable && uboSupported) {
      const ShaderDefine skinnedDefine{"SKINNED", "1"};
      const ShaderProgramHandle skinnedGbufferShader =
          load_configured_shader_variant("gbuffer.vert", "gbuffer.frag",
                                         &skinnedDefine, 1U);
      backend.gbufferSkinnedShaderHandle = skinnedGbufferShader;
      if (resolve_gbuffer_skinned_program_state(backend, dev)) {
        backend.bonePaletteUbo = dev->create_buffer();
        if (backend.bonePaletteUbo != 0U) {
          dev->bind_uniform_buffer(backend.bonePaletteUbo);
          dev->buffer_data_uniform(
              nullptr, static_cast<std::ptrdiff_t>(kMaxSkinPaletteJoints *
                                                   sizeof(math::Mat4)));
          dev->bind_uniform_buffer(0U);
          dev->bind_uniform_buffer_base(kBonePaletteUboBinding,
                                        backend.bonePaletteUbo);
          backend.skinningAvailable = true;
        } else {
          core::log_message(
              core::LogLevel::Warning, "renderer",
              "bone palette buffer creation failed — GPU skinning disabled");
        }

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
            backend.shadowDepthSkinnedProgram = 0U;
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
        backend.gbufferSkinnedProgram = 0U;
        core::log_message(core::LogLevel::Warning, "renderer",
                          "skinned G-buffer shader not available — GPU "
                          "skinning disabled");
      }
    }
  }

}

} // namespace engine::renderer
