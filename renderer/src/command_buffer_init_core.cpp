// Implements the hard-fail backend core initialization: render device,
// shader system, the default/PBR/tonemap programs with their required
// shader parameters, and the fullscreen attribute-less geometry.
#include "command_buffer_flush_internal.h"
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

  // #138 flat array vocabulary: packed vec4 element arrays shared by the
  // GLSL and bgfx shader ports, uploaded via set_param_vec4_array.
  backend.pbrDirLightCountLocation =
      dev->shader_param(prog, "u_dirLightCount");
  backend.pbrDirLightDirectionParam =
      dev->shader_param(prog, "u_dirLightDirection");
  backend.pbrDirLightColorParam =
      dev->shader_param(prog, "u_dirLightColorIntensity");
  if (!backend.pbrDirLightDirectionParam.valid() ||
      !backend.pbrDirLightColorParam.valid()) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "PBR shader missing directional light arrays — "
                      "lights will be invisible");
  }

  backend.pbrPointLightCountLocation =
      dev->shader_param(prog, "u_pointLightCount");
  backend.pbrPointLightPosRadiusParam =
      dev->shader_param(prog, "u_pointLightPosRadius");
  backend.pbrPointLightColorParam =
      dev->shader_param(prog, "u_pointLightColorIntensity");
  if (!backend.pbrPointLightPosRadiusParam.valid() ||
      !backend.pbrPointLightColorParam.valid()) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "PBR shader missing point light arrays — lights "
                      "will be invisible");
  }

  backend.pbrSpotLightCountLocation =
      dev->shader_param(prog, "u_spotLightCount");
  backend.pbrSpotLightPosRadiusParam =
      dev->shader_param(prog, "u_spotLightPosRadius");
  backend.pbrSpotLightDirInnerParam =
      dev->shader_param(prog, "u_spotLightDirInner");
  backend.pbrSpotLightColorParam =
      dev->shader_param(prog, "u_spotLightColorIntensity");
  backend.pbrSpotLightParamsParam =
      dev->shader_param(prog, "u_spotLightParams");
}

void resolve_pbr_shadow_uniforms(BackendState &backend,
                                 const RenderDevice *dev) noexcept {
  const DeviceProgramHandle prog = backend.pbrProgram;
  char name[64] = {};

  backend.pbrShadowEnabledLoc = dev->shader_param(prog, "uShadowEnabled");
  for (std::size_t i = 0U; i < kShadowCascadeCount; ++i) {
    std::snprintf(name, sizeof(name), "uShadowMap%zu", i);
    backend.pbrShadowMapLocs[i] = dev->shader_param(prog, name);
  }
  backend.pbrShadowMatrixParam = dev->shader_param(prog, "uShadowMatrix");
  backend.pbrCascadeSplitsParam = dev->shader_param(prog, "uCascadeSplits");

  backend.pbrSpotShadowEnabledLoc =
      dev->shader_param(prog, "uSpotShadowEnabled");
  for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
    std::snprintf(name, sizeof(name), "uSpotShadowMap%zu", i);
    backend.pbrSpotShadowMapLocs[i] = dev->shader_param(prog, name);
  }
  backend.pbrSpotShadowMatrixParam =
      dev->shader_param(prog, "uSpotShadowMatrix");
  backend.pbrSpotShadowLightIdxParam =
      dev->shader_param(prog, "uSpotShadowLightIdxVec");

  backend.pbrPointShadowEnabledLoc =
      dev->shader_param(prog, "uPointShadowEnabled");
  for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
    std::snprintf(name, sizeof(name), "uPointShadowMap%zu", i);
    backend.pbrPointShadowMapLocs[i] = dev->shader_param(prog, name);
  }
  backend.pbrPointShadowPosFarParam =
      dev->shader_param(prog, "uPointShadowPosFar");
  backend.pbrPointShadowLightIdxParam =
      dev->shader_param(prog, "uPointShadowLightIdxVec");
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
      // u_modelMatrix, not u_model: bgfx reserves u_model as a
      // predefined uniform, so the shared pbr vocabulary avoids it.
      required_param(&ok, dev, pbrProgram, "u_modelMatrix");
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
  backend.pbrCameraForwardOrthoLocation =
      dev->shader_param(pbrProgram, "u_cameraForwardOrtho");
  backend.pbrHasAlbedoTextureLocation =
      dev->shader_param(pbrProgram, "u_hasAlbedoTexture");
  backend.pbrAlbedoMapLocation =
      dev->shader_param(pbrProgram, "u_albedoMap");
  backend.pbrOpacityLocation =
      required_param(&ok, dev, pbrProgram, "u_opacity");
  // issue #160: texture-backed PBR material slots — all optional, same
  // fallback contract as u_hasAlbedoTexture above.
  backend.pbrEmissiveLocation = dev->shader_param(pbrProgram, "u_emissive");
  backend.pbrHasMetallicRoughnessTextureLocation =
      dev->shader_param(pbrProgram, "u_hasMetallicRoughnessTexture");
  backend.pbrMetallicRoughnessMapLocation =
      dev->shader_param(pbrProgram, "u_metallicRoughnessMap");
  backend.pbrHasEmissiveTextureLocation =
      dev->shader_param(pbrProgram, "u_hasEmissiveTexture");
  backend.pbrEmissiveMapLocation =
      dev->shader_param(pbrProgram, "u_emissiveMap");
  backend.pbrHasOcclusionTextureLocation =
      dev->shader_param(pbrProgram, "u_hasOcclusionTexture");
  backend.pbrOcclusionMapLocation =
      dev->shader_param(pbrProgram, "u_occlusionMap");
  backend.pbrHasOpacityTextureLocation =
      dev->shader_param(pbrProgram, "u_hasOpacityTexture");
  backend.pbrOpacityMapLocation =
      dev->shader_param(pbrProgram, "u_opacityMap");
  backend.pbrAlphaModeLocation =
      dev->shader_param(pbrProgram, "u_alphaMode");
  backend.pbrAlphaCutoffLocation =
      dev->shader_param(pbrProgram, "u_alphaCutoff");
  backend.pbrUvTilingLocation = dev->shader_param(pbrProgram, "u_uvTiling");
  backend.pbrUvOffsetLocation = dev->shader_param(pbrProgram, "u_uvOffset");
  // Optional: only the shadow cascade selection reads the view matrix,
  // and the bgfx pbr port omits shadow sampling until the shadows unit
  // fits the sampler budget.
  backend.pbrViewLocation = dev->shader_param(pbrProgram, "u_viewMatrix");
  backend.pbrViewProjectionLocation =
      required_param(&ok, dev, pbrProgram, "u_viewProjection");
  // Optional: the bgfx pbr port has no runtime instancing toggle (its
  // instanced foliage path lands with the deferred/instancing unit).
  backend.pbrUseInstancingLocation =
      dev->shader_param(pbrProgram, "uUseInstancing");
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

  // Load PBR shader. The PBR_FULL variant carries forward shadow and
  // IBL sampling on the GL unit map (tops out at kIblBrdfLutUnit, 21),
  // so it needs the same unit budget as the deferred pass; a device
  // under that budget (WebGL2's 16-unit floor) takes the reduced
  // default, whose shadow=1 / constant-ambient paths stay correct.
  // On GL the variant define is inert — its GLSL always carries the
  // full sampling — so both backends load one canonical program.
  const bool forwardFullSamplers =
      dev->caps.maxTextureSamplers >
      static_cast<std::uint16_t>(kIblBrdfLutUnit);
  const ShaderDefine pbrFullDefine{"PBR_FULL", "1"};
  const ShaderProgramHandle pbrShaderHandle =
      forwardFullSamplers
          ? load_configured_shader_variant("pbr.vert", "pbr.frag",
                                           &pbrFullDefine, 1U)
          : load_configured_shader_program("pbr.vert", "pbr.frag");
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

  // Instanced forward sibling (soft-fail: batches fall back to
  // per-command draws). Only backends without a runtime instancing
  // toggle load it — the bgfx vertex ports carry no uUseInstancing, so
  // instanced batches bind this program instead; its uniforms resolve
  // through the backend's global-by-name registry, so no separate
  // parameter family exists.
  if (!backend.pbrUseInstancingLocation.valid() && dev->caps.instancing) {
    const ShaderDefine instancedDefines[2] = {{"INSTANCED", "1"},
                                              {"PBR_FULL", "1"}};
    const std::size_t instancedDefineCount = forwardFullSamplers ? 2U : 1U;
    const ShaderProgramHandle instanced = load_configured_shader_variant(
        "pbr.vert", "pbr.frag", instancedDefines, instancedDefineCount);
    if (instanced != kInvalidShaderProgram) {
      backend.pbrInstancedShaderHandle = instanced;
      backend.pbrInstancedProgram = shader_device_program(instanced);
    } else {
      core::log_message(core::LogLevel::Info, "renderer",
                        "instanced PBR program unavailable — batches "
                        "draw per command");
    }
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

  // Fallback textures for disabled sampler slots (soft-fail: without
  // them, Vulkan-family backends render undefined output whenever a
  // declared shadow/IBL sampler has no live texture).
  {
    const std::uint8_t whiteTexel[4] = {255U, 255U, 255U, 255U};
    TextureDesc fallback2D{};
    fallback2D.width = 1;
    fallback2D.height = 1;
    fallback2D.pixels = whiteTexel;
    backend.fallbackTexture2D = dev->create_texture(fallback2D);

    const void *facePixels[6] = {whiteTexel, whiteTexel, whiteTexel,
                                 whiteTexel, whiteTexel, whiteTexel};
    TextureDesc fallbackCube{};
    fallbackCube.kind = TextureKind::Cube;
    fallbackCube.width = 1;
    fallbackCube.height = 1;
    fallbackCube.facePixels = facePixels;
    backend.fallbackCubemap = dev->create_texture(fallbackCube);
    if ((backend.fallbackTexture2D == kInvalidDeviceTexture) ||
        (backend.fallbackCubemap == kInvalidDeviceTexture)) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "fallback sampler textures unavailable — disabled "
                        "shadow/IBL slots may render undefined on "
                        "Vulkan-family backends");
    }
  }
  return true;
}

} // namespace engine::renderer
