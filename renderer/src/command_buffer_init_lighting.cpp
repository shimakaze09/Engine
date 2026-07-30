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
    backend.gbufHasAlbedoTextureLoc =
        dev->uniform_location(gbufProg, "uHasAlbedoTexture");
    backend.gbufAlbedoTextureLoc =
        dev->uniform_location(gbufProg, "uAlbedoTexture");
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

    // Per-light data texture sampler (light parameters are fetched by index
    // instead of per-light uniform arrays).
    backend.dlLightDataTexLoc = dev->uniform_location(dlProg, "uLightDataTex");

    // Environment IBL samplers in deferred lighting shader.
    backend.dlIblEnabledLoc = dev->uniform_location(dlProg, "uIblEnabled");
    backend.dlIrradianceMapLoc =
        dev->uniform_location(dlProg, "uIrradianceMap");
    backend.dlPrefilteredMapLoc =
        dev->uniform_location(dlProg, "uPrefilteredMap");
    backend.dlBrdfLutLoc = dev->uniform_location(dlProg, "uBrdfLut");
    backend.dlPrefilteredMipsLoc =
        dev->uniform_location(dlProg, "uPrefilteredMips");

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

}

} // namespace engine::renderer
