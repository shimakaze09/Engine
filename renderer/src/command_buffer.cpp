#include "engine/renderer/command_buffer.h"

#include <algorithm>
#include <array>
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
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

namespace {

constexpr float kDefaultFovRadians = 1.0471975512F;
constexpr float kNearClip = 0.1F;
constexpr float kFarClip = 100.0F;
constexpr float kClearRed = 0.18F;
constexpr float kClearGreen = 0.28F;
constexpr float kClearBlue = 0.60F;

CameraState g_activeCamera{};
int g_sceneViewportWidth = 0;
int g_sceneViewportHeight = 0;
RendererFrameStats g_lastFrameStats{};

struct BackendState final {
  bool initialized = false;
  bool failed = false;

  // Fallback shader (kept for compatibility).
  ShaderProgramHandle defaultShaderHandle{};
  std::uint32_t defaultProgram = 0U;

  // PBR shader.
  ShaderProgramHandle pbrShaderHandle{};
  std::uint32_t pbrProgram = 0U;

  // PBR uniform locations.
  std::int32_t pbrModelLocation = -1;
  std::int32_t pbrMvpLocation = -1;
  std::int32_t pbrNormalMatrixLocation = -1;
  std::int32_t pbrAlbedoLocation = -1;
  std::int32_t pbrRoughnessLocation = -1;
  std::int32_t pbrMetallicLocation = -1;
  std::int32_t pbrTimeLocation = -1;
  std::int32_t pbrCameraPosLocation = -1;
  std::int32_t pbrHasAlbedoTextureLocation = -1;
  std::int32_t pbrAlbedoMapLocation = -1;

  // Directional lights.
  std::int32_t pbrDirLightCountLocation = -1;
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightDir{};
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightColor{};
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightIntensity{};

  // Point lights.
  std::int32_t pbrPointLightCountLocation = -1;
  std::array<std::int32_t, kMaxPointLights> pbrPointLightPos{};
  std::array<std::int32_t, kMaxPointLights> pbrPointLightColor{};
  std::array<std::int32_t, kMaxPointLights> pbrPointLightIntensity{};

  // Tonemap shader.
  ShaderProgramHandle tonemapShaderHandle{};
  std::uint32_t tonemapProgram = 0U;
  std::int32_t tonemapSceneColorLocation = -1;
  std::int32_t tonemapExposureLocation = -1;

  // Empty VAO for fullscreen triangle.
  std::uint32_t emptyVao = 0U;

  // Tracked drawable dimensions for pass resource resize.
  int lastWidth = 0;
  int lastHeight = 0;

  // ---- Deferred rendering state ----
  bool deferredAvailable = false;

  // G-Buffer shader.
  ShaderProgramHandle gbufferShaderHandle{};
  std::uint32_t gbufferProgram = 0U;
  std::int32_t gbufModelLoc = -1;
  std::int32_t gbufViewLoc = -1;
  std::int32_t gbufProjectionLoc = -1;
  std::int32_t gbufNormalMatrixLoc = -1;
  std::int32_t gbufAlbedoLoc = -1;
  std::int32_t gbufMetallicLoc = -1;
  std::int32_t gbufRoughnessLoc = -1;
  std::int32_t gbufAOLoc = -1;
  std::int32_t gbufEmissiveLoc = -1;

  // Deferred lighting shader.
  ShaderProgramHandle deferredLightShaderHandle{};
  std::uint32_t deferredLightProgram = 0U;
  std::int32_t dlGBufAlbedoLoc = -1;
  std::int32_t dlGBufNormalLoc = -1;
  std::int32_t dlGBufEmissiveLoc = -1;
  std::int32_t dlGBufDepthLoc = -1;
  std::int32_t dlTileLightTexLoc = -1;
  std::int32_t dlTileCountXLoc = -1;
  std::int32_t dlTileCountYLoc = -1;
  std::int32_t dlInvProjectionLoc = -1;
  std::int32_t dlInvViewLoc = -1;
  std::int32_t dlDirLightDirLoc = -1;
  std::int32_t dlDirLightColorLoc = -1;
  std::int32_t dlCameraPosLoc = -1;
  std::int32_t dlScreenSizeLoc = -1;
  std::int32_t dlPointLightCountLoc = -1;
  std::int32_t dlSpotLightCountLoc = -1;

  // Deferred point light uniform arrays.
  std::array<std::int32_t, kMaxPointLights> dlPointPosLocs{};
  std::array<std::int32_t, kMaxPointLights> dlPointColorLocs{};
  std::array<std::int32_t, kMaxPointLights> dlPointIntensityLocs{};
  std::array<std::int32_t, kMaxPointLights> dlPointRadiusLocs{};

  // Deferred spot light uniform arrays.
  std::array<std::int32_t, kMaxSpotLights> dlSpotPosLocs{};
  std::array<std::int32_t, kMaxSpotLights> dlSpotDirLocs{};
  std::array<std::int32_t, kMaxSpotLights> dlSpotColorLocs{};
  std::array<std::int32_t, kMaxSpotLights> dlSpotIntensityLocs{};
  std::array<std::int32_t, kMaxSpotLights> dlSpotRadiusLocs{};
  std::array<std::int32_t, kMaxSpotLights> dlSpotInnerConeLocs{};
  std::array<std::int32_t, kMaxSpotLights> dlSpotOuterConeLocs{};

  // G-Buffer debug shader.
  ShaderProgramHandle gbufferDebugShaderHandle{};
  std::uint32_t gbufferDebugProgram = 0U;
  std::int32_t dbgGBufAlbedoLoc = -1;
  std::int32_t dbgGBufNormalLoc = -1;
  std::int32_t dbgGBufEmissiveLoc = -1;
  std::int32_t dbgGBufDepthLoc = -1;
  std::int32_t dbgModeLoc = -1;

  // Tile light texture (uploaded each frame by CPU culling).
  std::uint32_t tileLightTex = 0U;
  std::vector<float> tileBuffer;
};

BackendState &backend_state() noexcept {
  static BackendState state{};
  return state;
}

void reset_backend_on_failure() noexcept {
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.failed = true;
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
  for (std::size_t i = 0U; i < kMaxPointLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].position", i);
    backend.pbrPointLightPos[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].color", i);
    backend.pbrPointLightColor[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].intensity", i);
    backend.pbrPointLightIntensity[i] = dev->uniform_location(prog, name);
    if ((backend.pbrPointLightPos[i] < 0) ||
        (backend.pbrPointLightColor[i] < 0) ||
        (backend.pbrPointLightIntensity[i] < 0)) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing point light uniforms at "
                        "index — lights will be invisible");
    }
  }
}

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
  const ShaderProgramHandle defaultShaderHandle = load_shader_program(
      "assets/shaders/default.vert", "assets/shaders/default.frag");
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
      load_shader_program("assets/shaders/pbr.vert", "assets/shaders/pbr.frag");
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

  if ((backend.pbrMvpLocation < 0) || (backend.pbrNormalMatrixLocation < 0) ||
      (backend.pbrAlbedoLocation < 0)) {
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

  // Load tonemap shader.
  const ShaderProgramHandle tonemapShaderHandle = load_shader_program(
      "assets/shaders/fullscreen.vert", "assets/shaders/tonemap.frag");
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

  // Register CVars for deferred rendering.
  core::cvar_register_bool("r_deferred", true, "Enable deferred rendering");
  core::cvar_register_int("r_gbuffer_debug", 0,
                          "G-Buffer debug mode (0=off, 1=albedo, 2=normals, "
                          "3=metallic, 4=roughness, 5=emissive, 6=AO, 7=depth)");

  // Load deferred rendering shaders (soft-fail: falls back to forward).
  bool deferredOk = true;

  const ShaderProgramHandle gbufferShader =
      load_shader_program("assets/shaders/gbuffer.vert",
                          "assets/shaders/gbuffer.frag");
  if (gbufferShader == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "G-Buffer shader not available — deferred path disabled");
    deferredOk = false;
  }

  ShaderProgramHandle deferredLightShader{};
  ShaderProgramHandle gbufferDebugShader{};

  if (deferredOk) {
    deferredLightShader = load_shader_program(
        "assets/shaders/fullscreen.vert",
        "assets/shaders/deferred_lighting.frag");
    if (deferredLightShader == kInvalidShaderProgram) {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "deferred lighting shader not available — deferred path disabled");
      destroy_shader_program(gbufferShader);
      deferredOk = false;
    }
  }

  if (deferredOk) {
    gbufferDebugShader = load_shader_program(
        "assets/shaders/fullscreen.vert",
        "assets/shaders/gbuffer_debug.frag");
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
    backend.gbufAlbedoLoc = dev->uniform_location(gbufProg, "uAlbedo");
    backend.gbufMetallicLoc = dev->uniform_location(gbufProg, "uMetallic");
    backend.gbufRoughnessLoc = dev->uniform_location(gbufProg, "uRoughness");
    backend.gbufAOLoc = dev->uniform_location(gbufProg, "uAO");
    backend.gbufEmissiveLoc = dev->uniform_location(gbufProg, "uEmissive");

    // --- Deferred lighting shader uniforms ---
    const auto dlProg = shader_gpu_program(deferredLightShader);
    backend.deferredLightShaderHandle = deferredLightShader;
    backend.deferredLightProgram = dlProg;
    backend.dlGBufAlbedoLoc =
        dev->uniform_location(dlProg, "uGBufferAlbedo");
    backend.dlGBufNormalLoc =
        dev->uniform_location(dlProg, "uGBufferNormal");
    backend.dlGBufEmissiveLoc =
        dev->uniform_location(dlProg, "uGBufferEmissive");
    backend.dlGBufDepthLoc = dev->uniform_location(dlProg, "uGBufferDepth");
    backend.dlTileLightTexLoc =
        dev->uniform_location(dlProg, "uTileLightTex");
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
      backend.dbgGBufDepthLoc =
          dev->uniform_location(dbgProg, "uGBufferDepth");
      backend.dbgModeLoc = dev->uniform_location(dbgProg, "uDebugMode");
    }
  }

  static_cast<void>(initialize_gpu_profiler());
  backend.initialized = true;
  return true;
}

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

  if (backend->emptyVao != 0U && dev != nullptr) {
    dev->destroy_vertex_array(backend->emptyVao);
    backend->emptyVao = 0U;
  }

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

math::Mat4 compute_model_matrix(const DrawCommand &command) noexcept {
  return command.modelMatrix;
}

math::Mat4 compute_mvp(const math::Mat4 &model,
                       const math::Mat4 &viewProjection) noexcept {
  return math::mul(viewProjection, model);
}

void extract_normal_matrix(const math::Mat4 &model,
                           float *normalMatrixOut) noexcept {
  if (normalMatrixOut == nullptr) {
    return;
  }

  math::Mat4 invModel{};
  const math::Mat4 normalSource =
      math::inverse(model, &invModel) ? math::transpose(invModel) : model;

  normalMatrixOut[0] = normalSource.columns[0].x;
  normalMatrixOut[1] = normalSource.columns[0].y;
  normalMatrixOut[2] = normalSource.columns[0].z;

  normalMatrixOut[3] = normalSource.columns[1].x;
  normalMatrixOut[4] = normalSource.columns[1].y;
  normalMatrixOut[5] = normalSource.columns[1].z;

  normalMatrixOut[6] = normalSource.columns[2].x;
  normalMatrixOut[7] = normalSource.columns[2].y;
  normalMatrixOut[8] = normalSource.columns[2].z;
}

} // namespace

void CommandBufferBuilder::reset() noexcept { m_commandCount = 0U; }

bool CommandBufferBuilder::submit(const DrawCommand &command) noexcept {
  if (m_commandCount >= kMaxDrawCommands) {
    return false;
  }

  m_commands[m_commandCount] = command;
  ++m_commandCount;
  return true;
}

bool CommandBufferBuilder::append_from(
    const CommandBufferBuilder &other) noexcept {
  if (other.m_commandCount == 0U) {
    return true;
  }

  if ((m_commandCount + other.m_commandCount) > kMaxDrawCommands) {
    return false;
  }

  std::memcpy(m_commands.data() + m_commandCount, other.m_commands.data(),
              sizeof(DrawCommand) * other.m_commandCount);
  m_commandCount += other.m_commandCount;

  return true;
}

void CommandBufferBuilder::sort_by_key() noexcept {
  std::sort(m_commands.begin(),
            m_commands.begin() + static_cast<std::ptrdiff_t>(m_commandCount),
            [](const DrawCommand &lhs, const DrawCommand &rhs) {
              return lhs.sortKey.value < rhs.sortKey.value;
            });
}

std::size_t CommandBufferBuilder::command_count() const noexcept {
  return m_commandCount;
}

CommandBufferView CommandBufferBuilder::view() const noexcept {
  CommandBufferView commandBufferView{};
  commandBufferView.data = m_commands.data();
  commandBufferView.count = static_cast<std::uint32_t>(m_commandCount);
  return commandBufferView;
}

void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &lights) noexcept {
  if (!initialize_backend()) {
    return;
  }

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();
  RendererFrameStats frameStats{};
  gpu_profiler_begin_frame();

  int drawableWidth = 1280;
  int drawableHeight = 720;
  if ((g_sceneViewportWidth > 0) && (g_sceneViewportHeight > 0)) {
    drawableWidth = g_sceneViewportWidth;
    drawableHeight = g_sceneViewportHeight;
  } else {
    core::render_drawable_size(&drawableWidth, &drawableHeight);
  }
  if (drawableWidth <= 0) {
    drawableWidth = 1;
  }
  if (drawableHeight <= 0) {
    drawableHeight = 1;
  }

  // Initialize or resize pass resources when dimensions change.
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

  // Check if deferred rendering is enabled.
  const bool useDeferred =
      backend.deferredAvailable &&
      core::cvar_get_bool("r_deferred", true);
  const int gbufferDebugMode =
      core::cvar_get_int("r_gbuffer_debug", 0);

  // Camera setup (shared by both paths).
  const float aspect =
      static_cast<float>(drawableWidth) / static_cast<float>(drawableHeight);
  const math::Mat4 viewMat = math::look_at(
      g_activeCamera.position, g_activeCamera.target, g_activeCamera.up);
  const float fov = (g_activeCamera.fovRadians > 0.0F)
                        ? g_activeCamera.fovRadians
                        : kDefaultFovRadians;
  const float nearP =
      (g_activeCamera.nearPlane > 0.0F) ? g_activeCamera.nearPlane : kNearClip;
  const float farP =
      (g_activeCamera.farPlane > nearP) ? g_activeCamera.farPlane : kFarClip;
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

  // Determine opaque / transparent partition.
  constexpr std::uint64_t kTransparentBit = 1ULL << 63;

  std::size_t opaqueCount = 0U;
  std::size_t totalCount = 0U;

  if ((commandBufferView.data != nullptr) && (commandBufferView.count > 0U)) {
    totalCount = static_cast<std::size_t>(commandBufferView.count);
    for (std::size_t i = 0U; i < totalCount; ++i) {
      if ((commandBufferView.data[i].sortKey.value & kTransparentBit) != 0U) {
        break;
      }
      opaqueCount = i + 1U;
    }
  }

  // ====================================================================
  // DEFERRED PATH
  // ====================================================================
  if (useDeferred) {
    // --- G-Buffer pass: render geometry into MRT ---
    gpu_profiler_begin_pass(GpuPassId::GBuffer);
    const std::uint32_t gbufferFbo =
        pass_resource_framebuffer(passRes.gbufferAlbedo);
    dev->bind_framebuffer(gbufferFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->enable_depth_test();
    dev->set_clear_color(0.0F, 0.0F, 0.0F, 0.0F);
    dev->clear_color_depth();

    dev->bind_program(backend.gbufferProgram);

    // Per-frame camera uniforms.
    if (backend.gbufViewLoc >= 0) {
      dev->set_uniform_mat4(backend.gbufViewLoc, &viewMat.columns[0].x);
    }
    if (backend.gbufProjectionLoc >= 0) {
      dev->set_uniform_mat4(backend.gbufProjectionLoc, &projMat.columns[0].x);
    }

    // Helper: draw a range in G-Buffer pass.
    auto drawGBuffer = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      for (std::size_t i = start; i < end; ++i) {
        const DrawCommand &command = commandBufferView.data[i];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (mesh->vertexArray != boundVertexArray) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVertexArray = mesh->vertexArray;
        }

        // Material uniforms.
        if (backend.gbufAlbedoLoc >= 0) {
          dev->set_uniform_vec3(backend.gbufAlbedoLoc,
                                &command.material.albedo.x);
        }
        if (backend.gbufMetallicLoc >= 0) {
          dev->set_uniform_float(backend.gbufMetallicLoc,
                                 command.material.metallic);
        }
        if (backend.gbufRoughnessLoc >= 0) {
          dev->set_uniform_float(backend.gbufRoughnessLoc,
                                 command.material.roughness);
        }
        if (backend.gbufAOLoc >= 0) {
          dev->set_uniform_float(backend.gbufAOLoc, 1.0F);
        }
        if (backend.gbufEmissiveLoc >= 0) {
          dev->set_uniform_vec3(backend.gbufEmissiveLoc,
                                &command.material.emissive.x);
        }

        const math::Mat4 model = compute_model_matrix(command);
        float normalMatrix[9] = {};
        extract_normal_matrix(model, normalMatrix);

        if (backend.gbufModelLoc >= 0) {
          dev->set_uniform_mat4(backend.gbufModelLoc, &model.columns[0].x);
        }
        if (backend.gbufNormalMatrixLoc >= 0) {
          dev->set_uniform_mat3(backend.gbufNormalMatrixLoc, normalMatrix);
        }

        if (mesh->indexCount > 0U) {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->indexCount / 3U);
          dev->draw_elements_triangles_u32(
              static_cast<std::int32_t>(mesh->indexCount));
        } else {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->vertexCount / 3U);
          dev->draw_arrays_triangles(
              0, static_cast<std::int32_t>(mesh->vertexCount));
        }
      }
    };

    // Opaque geometry only for G-Buffer.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawGBuffer(0U, opaqueCount);

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::GBuffer);

    // --- CPU tiled light culling ---
    const std::size_t tileBufferSize =
        compute_tile_buffer_size(drawableWidth, drawableHeight);
    if (backend.tileBuffer.size() < tileBufferSize) {
      backend.tileBuffer.resize(tileBufferSize, 0.0F);
    }

    TileLightData tileData{};
    tileData.data = backend.tileBuffer.data();
    tileData.dataSize = backend.tileBuffer.size();

    cull_lights_tiled(lights, &viewMat.columns[0].x, &projMat.columns[0].x,
                      drawableWidth, drawableHeight, tileData);

    // Upload tile texture.
    if (backend.tileLightTex == 0U) {
      backend.tileLightTex = dev->create_texture_2d_r32f(
          kTileDataWidth, tileData.totalTiles, backend.tileBuffer.data());
    } else {
      dev->update_texture_2d_r32f(backend.tileLightTex, kTileDataWidth,
                                  tileData.totalTiles,
                                  backend.tileBuffer.data());
    }

    // --- G-Buffer debug visualization (overrides lighting pass) ---
    if (gbufferDebugMode > 0 && backend.gbufferDebugProgram != 0U) {
      gpu_profiler_begin_pass(GpuPassId::GBufferDebug);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.gbufferDebugProgram);

      // Bind G-Buffer textures.
      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));

      if (backend.dbgGBufAlbedoLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufAlbedoLoc, 0);
      if (backend.dbgGBufNormalLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufNormalLoc, 1);
      if (backend.dbgGBufEmissiveLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufEmissiveLoc, 2);
      if (backend.dbgGBufDepthLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufDepthLoc, 3);
      // Debug mode: 0=albedo,1=normals,2=metallic,3=roughness,4=emissive,5=AO,6=depth
      // CVar value 1..7 maps to shader 0..6.
      if (backend.dbgModeLoc >= 0)
        dev->set_uniform_int(backend.dbgModeLoc, gbufferDebugMode - 1);

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_texture(3, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::GBufferDebug);
    } else {
      // --- Deferred lighting pass: fullscreen, reads G-Buffer → HDR scene ---
      gpu_profiler_begin_pass(GpuPassId::DeferredLighting);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.deferredLightProgram);

      // Bind G-Buffer textures on units 0-3 and tile on unit 4.
      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(4, backend.tileLightTex);

      if (backend.dlGBufAlbedoLoc >= 0)
        dev->set_uniform_int(backend.dlGBufAlbedoLoc, 0);
      if (backend.dlGBufNormalLoc >= 0)
        dev->set_uniform_int(backend.dlGBufNormalLoc, 1);
      if (backend.dlGBufEmissiveLoc >= 0)
        dev->set_uniform_int(backend.dlGBufEmissiveLoc, 2);
      if (backend.dlGBufDepthLoc >= 0)
        dev->set_uniform_int(backend.dlGBufDepthLoc, 3);
      if (backend.dlTileLightTexLoc >= 0)
        dev->set_uniform_int(backend.dlTileLightTexLoc, 4);
      if (backend.dlTileCountXLoc >= 0)
        dev->set_uniform_int(backend.dlTileCountXLoc, tileData.tileCountX);
      if (backend.dlTileCountYLoc >= 0)
        dev->set_uniform_int(backend.dlTileCountYLoc, tileData.tileCountY);

      // Inverse matrices for depth reconstruction.
      math::Mat4 invProj{};
      if (math::inverse(projMat, &invProj)) {
        if (backend.dlInvProjectionLoc >= 0)
          dev->set_uniform_mat4(backend.dlInvProjectionLoc,
                                &invProj.columns[0].x);
      }
      math::Mat4 invView{};
      if (math::inverse(viewMat, &invView)) {
        if (backend.dlInvViewLoc >= 0)
          dev->set_uniform_mat4(backend.dlInvViewLoc,
                                &invView.columns[0].x);
      }

      // Directional light (use first if available).
      if (backend.dlDirLightDirLoc >= 0 &&
          lights.directionalLightCount > 0U) {
        dev->set_uniform_vec3(backend.dlDirLightDirLoc,
                              &lights.directionalLights[0].direction.x);
      }
      if (backend.dlDirLightColorLoc >= 0 &&
          lights.directionalLightCount > 0U) {
        dev->set_uniform_vec3(backend.dlDirLightColorLoc,
                              &lights.directionalLights[0].color.x);
      }

      if (backend.dlCameraPosLoc >= 0) {
        dev->set_uniform_vec3(backend.dlCameraPosLoc,
                              &g_activeCamera.position.x);
      }
      if (backend.dlScreenSizeLoc >= 0) {
        const float screenSize[2] = {static_cast<float>(drawableWidth),
                                     static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.dlScreenSizeLoc, screenSize);
      }

      // Upload point light data.
      const auto plCount =
          static_cast<int>(std::min(lights.pointLightCount,
                                    static_cast<std::size_t>(kMaxPointLights)));
      if (backend.dlPointLightCountLoc >= 0)
        dev->set_uniform_int(backend.dlPointLightCountLoc, plCount);
      for (int pi = 0; pi < plCount; ++pi) {
        const auto &pl = lights.pointLights[pi];
        const auto idx = static_cast<std::size_t>(pi);
        if (backend.dlPointPosLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlPointPosLocs[idx], &pl.position.x);
        if (backend.dlPointColorLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlPointColorLocs[idx], &pl.color.x);
        if (backend.dlPointIntensityLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlPointIntensityLocs[idx],
                                 pl.intensity);
        if (backend.dlPointRadiusLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlPointRadiusLocs[idx], pl.radius);
      }

      // Upload spot light data.
      const auto slCount =
          static_cast<int>(std::min(lights.spotLightCount,
                                    static_cast<std::size_t>(kMaxSpotLights)));
      if (backend.dlSpotLightCountLoc >= 0)
        dev->set_uniform_int(backend.dlSpotLightCountLoc, slCount);
      for (int si = 0; si < slCount; ++si) {
        const auto &sl = lights.spotLights[si];
        const auto idx = static_cast<std::size_t>(si);
        if (backend.dlSpotPosLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlSpotPosLocs[idx], &sl.position.x);
        if (backend.dlSpotDirLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlSpotDirLocs[idx], &sl.direction.x);
        if (backend.dlSpotColorLocs[idx] >= 0)
          dev->set_uniform_vec3(backend.dlSpotColorLocs[idx], &sl.color.x);
        if (backend.dlSpotIntensityLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotIntensityLocs[idx],
                                 sl.intensity);
        if (backend.dlSpotRadiusLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotRadiusLocs[idx], sl.radius);
        if (backend.dlSpotInnerConeLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotInnerConeLocs[idx],
                                 sl.innerConeAngle);
        if (backend.dlSpotOuterConeLocs[idx] >= 0)
          dev->set_uniform_float(backend.dlSpotOuterConeLocs[idx],
                                 sl.outerConeAngle);
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_texture(3, 0U);
      dev->bind_texture(4, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::DeferredLighting);
    }

    // Forward-render transparent geometry into the scene HDR FBO.
    if (opaqueCount < totalCount) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->enable_depth_test();

      // Copy G-Buffer depth to scene FBO depth (they share same dimensions).
      // For now, transparent objects render without depth test against opaques
      // in the deferred path (limitation: no G-Buffer depth blit available).
      dev->bind_program(backend.pbrProgram);

      // Re-upload forward PBR camera/lights for transparent pass.
      if (backend.pbrTimeLocation >= 0) {
        dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
      }
      if (backend.pbrCameraPosLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                              &g_activeCamera.position.x);
      }
      if (backend.pbrDirLightCountLocation >= 0) {
        dev->set_uniform_int(
            backend.pbrDirLightCountLocation,
            static_cast<std::int32_t>(lights.directionalLightCount));
      }
      for (std::size_t i = 0U; i < lights.directionalLightCount; ++i) {
        const auto &dl = lights.directionalLights[i];
        if (backend.pbrDirLightDir[i] >= 0)
          dev->set_uniform_vec3(backend.pbrDirLightDir[i], &dl.direction.x);
        if (backend.pbrDirLightColor[i] >= 0)
          dev->set_uniform_vec3(backend.pbrDirLightColor[i], &dl.color.x);
        if (backend.pbrDirLightIntensity[i] >= 0)
          dev->set_uniform_float(backend.pbrDirLightIntensity[i], dl.intensity);
      }
      if (backend.pbrPointLightCountLocation >= 0) {
        dev->set_uniform_int(
            backend.pbrPointLightCountLocation,
            static_cast<std::int32_t>(lights.pointLightCount));
      }
      for (std::size_t i = 0U; i < lights.pointLightCount; ++i) {
        const auto &pl = lights.pointLights[i];
        if (backend.pbrPointLightPos[i] >= 0)
          dev->set_uniform_vec3(backend.pbrPointLightPos[i], &pl.position.x);
        if (backend.pbrPointLightColor[i] >= 0)
          dev->set_uniform_vec3(backend.pbrPointLightColor[i], &pl.color.x);
        if (backend.pbrPointLightIntensity[i] >= 0)
          dev->set_uniform_float(backend.pbrPointLightIntensity[i],
                                 pl.intensity);
      }
      if (backend.pbrAlbedoMapLocation >= 0)
        dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);

      const math::Mat4 &vp = viewProjection;

      auto drawForwardTransparent = [&](std::size_t start, std::size_t end) {
        std::uint32_t boundVA = 0U;
        std::uint32_t boundAlbedoTex = 0U;
        for (std::size_t i = start; i < end; ++i) {
          const DrawCommand &cmd = commandBufferView.data[i];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U))
            continue;
          if (mesh->vertexArray != boundVA) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVA = mesh->vertexArray;
          }
          if (backend.pbrAlbedoLocation >= 0)
            dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                                  &cmd.material.albedo.x);
          if (backend.pbrRoughnessLocation >= 0)
            dev->set_uniform_float(backend.pbrRoughnessLocation,
                                   cmd.material.roughness);
          if (backend.pbrMetallicLocation >= 0)
            dev->set_uniform_float(backend.pbrMetallicLocation,
                                   cmd.material.metallic);
          const std::uint32_t albedoGpu =
              texture_gpu_id(cmd.material.albedoTexture);
          const bool hasTex =
              (cmd.material.albedoTexture != kInvalidTextureHandle) &&
              (albedoGpu != 0U);
          if (backend.pbrHasAlbedoTextureLocation >= 0)
            dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                                 hasTex ? 1 : 0);
          if (hasTex && albedoGpu != boundAlbedoTex) {
            dev->bind_texture(0, albedoGpu);
            boundAlbedoTex = albedoGpu;
          } else if (!hasTex && boundAlbedoTex != 0U) {
            dev->bind_texture(0, 0U);
            boundAlbedoTex = 0U;
          }
          const math::Mat4 model = compute_model_matrix(cmd);
          const math::Mat4 mvp = compute_mvp(model, vp);
          float nm[9] = {};
          extract_normal_matrix(model, nm);
          if (backend.pbrModelLocation >= 0)
            dev->set_uniform_mat4(backend.pbrModelLocation,
                                  &model.columns[0].x);
          dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
          dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, nm);
          if (mesh->indexCount > 0U) {
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U);
            dev->draw_elements_triangles_u32(
                static_cast<std::int32_t>(mesh->indexCount));
          } else {
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->vertexCount / 3U);
            dev->draw_arrays_triangles(
                0, static_cast<std::int32_t>(mesh->vertexCount));
          }
        }
      };

      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawForwardTransparent(opaqueCount, totalCount);
      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
      dev->bind_texture(0, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
    }

    frameStats.gpuGBufferMs = gpu_profiler_pass_ms(GpuPassId::GBuffer);
    frameStats.gpuDeferredLightMs =
        gpu_profiler_pass_ms(GpuPassId::DeferredLighting);

  } else {
    // ====================================================================
    // FORWARD PATH (original)
    // ====================================================================
    const std::uint32_t sceneFbo =
        pass_resource_framebuffer(passRes.sceneColor);

    gpu_profiler_begin_pass(GpuPassId::Scene);
    dev->bind_framebuffer(sceneFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->enable_depth_test();
    dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
    dev->clear_color_depth();

    dev->bind_program(backend.pbrProgram);

    // Per-frame uniforms.
    if (backend.pbrTimeLocation >= 0) {
      dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation >= 0) {
      dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                            &g_activeCamera.position.x);
    }

    // Upload directional lights.
    if (backend.pbrDirLightCountLocation >= 0) {
      dev->set_uniform_int(
          backend.pbrDirLightCountLocation,
          static_cast<std::int32_t>(lights.directionalLightCount));
    }
    for (std::size_t i = 0U; i < lights.directionalLightCount; ++i) {
      const auto &dl = lights.directionalLights[i];
      if (backend.pbrDirLightDir[i] >= 0) {
        dev->set_uniform_vec3(backend.pbrDirLightDir[i], &dl.direction.x);
      }
      if (backend.pbrDirLightColor[i] >= 0) {
        dev->set_uniform_vec3(backend.pbrDirLightColor[i], &dl.color.x);
      }
      if (backend.pbrDirLightIntensity[i] >= 0) {
        dev->set_uniform_float(backend.pbrDirLightIntensity[i], dl.intensity);
      }
    }

    // Upload point lights.
    if (backend.pbrPointLightCountLocation >= 0) {
      dev->set_uniform_int(backend.pbrPointLightCountLocation,
                           static_cast<std::int32_t>(lights.pointLightCount));
    }
    for (std::size_t i = 0U; i < lights.pointLightCount; ++i) {
      const auto &pl = lights.pointLights[i];
      if (backend.pbrPointLightPos[i] >= 0) {
        dev->set_uniform_vec3(backend.pbrPointLightPos[i], &pl.position.x);
      }
      if (backend.pbrPointLightColor[i] >= 0) {
        dev->set_uniform_vec3(backend.pbrPointLightColor[i], &pl.color.x);
      }
      if (backend.pbrPointLightIntensity[i] >= 0) {
        dev->set_uniform_float(backend.pbrPointLightIntensity[i], pl.intensity);
      }
    }

    // Set albedo map sampler to texture unit 0.
    if (backend.pbrAlbedoMapLocation >= 0) {
      dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
    }

    // Helper: draw a range of draw commands.
    auto drawRange = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTexture = 0U;

      for (std::size_t i = start; i < end; ++i) {
        const DrawCommand &command = commandBufferView.data[i];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (mesh->vertexArray != boundVertexArray) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVertexArray = mesh->vertexArray;
        }

        // Material uniforms.
        if (backend.pbrAlbedoLocation >= 0) {
          dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                                &command.material.albedo.x);
        }
        if (backend.pbrRoughnessLocation >= 0) {
          dev->set_uniform_float(backend.pbrRoughnessLocation,
                                 command.material.roughness);
        }
        if (backend.pbrMetallicLocation >= 0) {
          dev->set_uniform_float(backend.pbrMetallicLocation,
                                 command.material.metallic);
        }

        // Albedo texture binding with state tracking.
        const std::uint32_t albedoGpuId =
            texture_gpu_id(command.material.albedoTexture);
        const bool hasAlbedoTex =
            (command.material.albedoTexture != kInvalidTextureHandle) &&
            (albedoGpuId != 0U);
        if (backend.pbrHasAlbedoTextureLocation >= 0) {
          dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                               hasAlbedoTex ? 1 : 0);
        }
        if (hasAlbedoTex && (albedoGpuId != boundAlbedoTexture)) {
          dev->bind_texture(0, albedoGpuId);
          boundAlbedoTexture = albedoGpuId;
        } else if (!hasAlbedoTex && (boundAlbedoTexture != 0U)) {
          dev->bind_texture(0, 0U);
          boundAlbedoTexture = 0U;
        }

        const math::Mat4 model = compute_model_matrix(command);
        const math::Mat4 mvp = compute_mvp(model, viewProjection);
        float normalMatrix[9] = {};
        extract_normal_matrix(model, normalMatrix);

        if (backend.pbrModelLocation >= 0) {
          dev->set_uniform_mat4(backend.pbrModelLocation,
                                &model.columns[0].x);
        }
        dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
        dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

        if (mesh->indexCount > 0U) {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->indexCount / 3U);
          dev->draw_elements_triangles_u32(
              static_cast<std::int32_t>(mesh->indexCount));
        } else {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->vertexCount / 3U);
          dev->draw_arrays_triangles(
              0, static_cast<std::int32_t>(mesh->vertexCount));
        }
      }
    };

    // Pass 1: Opaque.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawRange(0U, opaqueCount);

    // Pass 2: Transparent.
    if (opaqueCount < totalCount) {
      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawRange(opaqueCount, totalCount);

      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
    }

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::Scene);
  }

  // --- Tonemap pass: HDR scene → LDR final FBO ---
  gpu_profiler_begin_pass(GpuPassId::Tonemap);
  const std::uint32_t finalFbo = pass_resource_framebuffer(passRes.finalColor);
  dev->bind_framebuffer(finalFbo);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->disable_depth_test();

  dev->bind_program(backend.tonemapProgram);

  const std::uint32_t sceneColorTex =
      pass_resource_gpu_texture(passRes.sceneColor);
  dev->bind_texture(0, sceneColorTex);
  if (backend.tonemapSceneColorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapSceneColorLocation, 0);
  }
  if (backend.tonemapExposureLocation >= 0) {
    dev->set_uniform_float(backend.tonemapExposureLocation, 1.0F);
  }

  dev->bind_vertex_array(backend.emptyVao);
  dev->draw_arrays_triangles(0, 3);

  dev->bind_texture(0, 0U);
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  gpu_profiler_end_pass(GpuPassId::Tonemap);

  // --- Prepare back buffer for editor overlay (ImGui) ---
  dev->bind_framebuffer(0U);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->set_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
  dev->clear_color_depth();
  dev->enable_depth_test();

  frameStats.gpuSceneMs = gpu_profiler_pass_ms(GpuPassId::Scene);
  frameStats.gpuTonemapMs = gpu_profiler_pass_ms(GpuPassId::Tonemap);
  g_lastFrameStats = frameStats;
}

void shutdown_renderer() noexcept {
  BackendState &backend = backend_state();
  if (!backend.initialized && !backend.failed) {
    return;
  }

  if (core::make_render_context_current()) {
    destroy_backend_resources(&backend);
    shutdown_shader_system();
    shutdown_render_device();
    core::release_render_context();
  }

  backend = BackendState{};
}

void set_active_camera(const CameraState &camera) noexcept {
  g_activeCamera = camera;
}

void set_scene_viewport_size(int width, int height) noexcept {
  g_sceneViewportWidth = (width > 0) ? width : 0;
  g_sceneViewportHeight = (height > 0) ? height : 0;
}

CameraState get_active_camera() noexcept { return g_activeCamera; }

std::uint32_t get_scene_viewport_texture() noexcept {
  const PassResources &passRes = get_pass_resources();
  return pass_resource_gpu_texture(passRes.finalColor);
}

RendererFrameStats renderer_get_last_frame_stats() noexcept {
  return g_lastFrameStats;
}

} // namespace engine::renderer
