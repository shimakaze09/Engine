#include "engine/renderer/command_buffer.h"

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
bool g_fxaaAppliedThisFrame = false;

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
  std::int32_t tonemapOperatorLocation = -1;

  // FXAA shader.
  ShaderProgramHandle fxaaShaderHandle{};
  std::uint32_t fxaaProgram = 0U;
  std::int32_t fxaaInputTextureLocation = -1;
  std::int32_t fxaaTexelSizeLocation = -1;

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

  // ---- Bloom state ----
  ShaderProgramHandle bloomThresholdShaderHandle{};
  std::uint32_t bloomThresholdProgram = 0U;
  std::int32_t bloomThreshSceneColorLoc = -1;
  std::int32_t bloomThreshThresholdLoc = -1;

  ShaderProgramHandle bloomDownsampleShaderHandle{};
  std::uint32_t bloomDownsampleProgram = 0U;
  std::int32_t bloomDownInputLoc = -1;
  std::int32_t bloomDownTexelSizeLoc = -1;

  ShaderProgramHandle bloomUpsampleShaderHandle{};
  std::uint32_t bloomUpsampleProgram = 0U;
  std::int32_t bloomUpInputLoc = -1;
  std::int32_t bloomUpTexelSizeLoc = -1;

  // Tonemap bloom integration uniforms.
  std::int32_t tonemapBloomTextureLoc = -1;
  std::int32_t tonemapBloomIntensityLoc = -1;
  std::int32_t tonemapBloomEnabledLoc = -1;

  // Bloom mip chain resources (managed internally).
  static constexpr int kBloomMipLevels = 6;
  std::uint32_t bloomMipTextures[kBloomMipLevels] = {};
  std::uint32_t bloomMipFbos[kBloomMipLevels] = {};
  int bloomMipWidths[kBloomMipLevels] = {};
  int bloomMipHeights[kBloomMipLevels] = {};
  int bloomAllocatedWidth = 0;
  int bloomAllocatedHeight = 0;

  // ---- SSAO state ----
  bool ssaoAvailable = false;

  ShaderProgramHandle ssaoShaderHandle{};
  std::uint32_t ssaoProgram = 0U;
  std::int32_t ssaoDepthLoc = -1;
  std::int32_t ssaoNormalLoc = -1;
  std::int32_t ssaoNoiseLoc = -1;
  std::int32_t ssaoProjectionLoc = -1;
  std::int32_t ssaoNoiseScaleLoc = -1;
  std::int32_t ssaoRadiusLoc = -1;
  std::int32_t ssaoBiasLoc = -1;
  std::array<std::int32_t, 32> ssaoSampleLocs{};

  ShaderProgramHandle ssaoBlurShaderHandle{};
  std::uint32_t ssaoBlurProgram = 0U;
  std::int32_t ssaoBlurInputLoc = -1;
  std::int32_t ssaoBlurTexelSizeLoc = -1;

  // Deferred lighting SSAO uniforms.
  std::int32_t dlSsaoTextureLoc = -1;
  std::int32_t dlSsaoEnabledLoc = -1;

  // 4x4 noise texture for SSAO kernel rotation.
  std::uint32_t ssaoNoiseTexture = 0U;
  // Precomputed hemisphere kernel (32 samples * 3 floats).
  float ssaoKernel[32 * 3] = {};

  // ---- Shadow map state ----
  ShadowMapState shadowState{};
  bool shadowAvailable = false;

  ShaderProgramHandle shadowDepthShaderHandle{};
  std::uint32_t shadowDepthProgram = 0U;
  std::int32_t shadowLightMvpLoc = -1;
  std::int32_t shadowModelLoc = -1;

  // Deferred lighting shadow uniforms.
  std::int32_t dlShadowEnabledLoc = -1;
  std::array<std::int32_t, kShadowCascadeCount> dlShadowMapLocs{};
  std::array<std::int32_t, kShadowCascadeCount> dlShadowMatrixLocs{};
  std::array<std::int32_t, kShadowCascadeCount> dlCascadeSplitLocs{};

  // ---- Spot shadow state ----
  SpotShadowState spotShadowState{};
  bool spotShadowAvailable = false;

  std::int32_t dlSpotShadowEnabledLoc = -1;
  std::array<std::int32_t, kMaxSpotShadowLights> dlSpotShadowMapLocs{};
  std::array<std::int32_t, kMaxSpotShadowLights> dlSpotShadowMatrixLocs{};
  std::array<std::int32_t, kMaxSpotShadowLights> dlSpotShadowLightIdxLocs{};

  // ---- Point shadow state ----
  PointShadowState pointShadowState{};
  bool pointShadowAvailable = false;

  ShaderProgramHandle shadowDepthPointShaderHandle{};
  std::uint32_t shadowDepthPointProgram = 0U;
  std::int32_t shadowPointLightMvpLoc = -1;
  std::int32_t shadowPointModelLoc = -1;
  std::int32_t shadowPointLightPosLoc = -1;
  std::int32_t shadowPointFarPlaneLoc = -1;

  std::int32_t dlPointShadowEnabledLoc = -1;
  std::array<std::int32_t, kMaxPointShadowLights> dlPointShadowMapLocs{};
  std::array<std::int32_t, kMaxPointShadowLights> dlPointShadowLightPosLocs{};
  std::array<std::int32_t, kMaxPointShadowLights> dlPointShadowFarPlaneLocs{};
  std::array<std::int32_t, kMaxPointShadowLights> dlPointShadowLightIdxLocs{};

  // ---- Auto-exposure state ----
  bool autoExposureAvailable = false;

  ShaderProgramHandle luminanceShaderHandle{};
  std::uint32_t luminanceProgram = 0U;
  std::int32_t lumSceneColorLoc = -1;

  // Luminance mip chain for averaging (progressively downsample to 1x1).
  static constexpr int kLuminanceMipLevels = 7;
  std::uint32_t lumMipTextures[kLuminanceMipLevels] = {};
  std::uint32_t lumMipFbos[kLuminanceMipLevels] = {};
  int lumMipWidths[kLuminanceMipLevels] = {};
  int lumMipHeights[kLuminanceMipLevels] = {};
  int lumAllocatedWidth = 0;
  int lumAllocatedHeight = 0;

  // Temporal adaptation.
  float currentExposure = 1.0F;
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

void destroy_bloom_resources(BackendState &b) noexcept {
  const auto *dev = render_device();
  if (dev == nullptr) {
    return;
  }
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if (b.bloomMipFbos[i] != 0U) {
      dev->destroy_framebuffer(b.bloomMipFbos[i]);
      b.bloomMipFbos[i] = 0U;
    }
    if (b.bloomMipTextures[i] != 0U) {
      dev->destroy_texture(b.bloomMipTextures[i]);
      b.bloomMipTextures[i] = 0U;
    }
  }
  b.bloomAllocatedWidth = 0;
  b.bloomAllocatedHeight = 0;
}

void ensure_bloom_resources(BackendState &b, int width, int height) noexcept {
  if (b.bloomAllocatedWidth == width && b.bloomAllocatedHeight == height) {
    return;
  }
  destroy_bloom_resources(b);
  const auto *dev = render_device();
  int w = width / 2;
  int h = height / 2;
  for (int i = 0; i < BackendState::kBloomMipLevels; ++i) {
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    b.bloomMipWidths[i] = w;
    b.bloomMipHeights[i] = h;
    b.bloomMipTextures[i] = dev->create_texture_2d_hdr(w, h, 4, nullptr);
    b.bloomMipFbos[i] = dev->create_framebuffer(b.bloomMipTextures[i], 0U);
    w /= 2;
    h /= 2;
  }
  b.bloomAllocatedWidth = width;
  b.bloomAllocatedHeight = height;
}

void destroy_luminance_resources(BackendState &b) noexcept {
  const auto *dev = render_device();
  if (dev == nullptr) {
    return;
  }
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if (b.lumMipFbos[i] != 0U) {
      dev->destroy_framebuffer(b.lumMipFbos[i]);
      b.lumMipFbos[i] = 0U;
    }
    if (b.lumMipTextures[i] != 0U) {
      dev->destroy_texture(b.lumMipTextures[i]);
      b.lumMipTextures[i] = 0U;
    }
  }
  b.lumAllocatedWidth = 0;
  b.lumAllocatedHeight = 0;
}

void ensure_luminance_resources(BackendState &b, int width,
                                int height) noexcept {
  if (b.lumAllocatedWidth == width && b.lumAllocatedHeight == height) {
    return;
  }
  destroy_luminance_resources(b);
  const auto *dev = render_device();
  int w = width / 2;
  int h = height / 2;
  for (int i = 0; i < BackendState::kLuminanceMipLevels; ++i) {
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    b.lumMipWidths[i] = w;
    b.lumMipHeights[i] = h;
    b.lumMipTextures[i] = dev->create_texture_2d_hdr(w, h, 4, nullptr);
    b.lumMipFbos[i] = dev->create_framebuffer(b.lumMipTextures[i], 0U);
    w /= 2;
    h /= 2;
  }
  b.lumAllocatedWidth = width;
  b.lumAllocatedHeight = height;
}

void generate_ssao_kernel(float *kernel, int count) noexcept {
  unsigned int seed = 12345U;
  auto nextFloat = [&seed]() -> float {
    seed = seed * 1103515245U + 12345U;
    return static_cast<float>((seed >> 16) & 0x7FFF) / 32767.0F;
  };
  for (int i = 0; i < count; ++i) {
    float x = nextFloat() * 2.0F - 1.0F;
    float y = nextFloat() * 2.0F - 1.0F;
    float z = nextFloat();
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 0.001F) {
      x = 0.0F;
      y = 0.0F;
      z = 1.0F;
      len = 1.0F;
    }
    x /= len;
    y /= len;
    z /= len;
    float scale = static_cast<float>(i) / static_cast<float>(count);
    scale = 0.1F + 0.9F * scale * scale;
    kernel[i * 3 + 0] = x * scale;
    kernel[i * 3 + 1] = y * scale;
    kernel[i * 3 + 2] = z * scale;
  }
}

std::uint32_t create_ssao_noise_texture() noexcept {
  float noise[16 * 4] = {};
  unsigned int seed = 54321U;
  auto nextFloat = [&seed]() -> float {
    seed = seed * 1103515245U + 12345U;
    return static_cast<float>((seed >> 16) & 0x7FFF) / 32767.0F;
  };
  for (int i = 0; i < 16; ++i) {
    noise[i * 4 + 0] = nextFloat() * 2.0F - 1.0F;
    noise[i * 4 + 1] = nextFloat() * 2.0F - 1.0F;
    noise[i * 4 + 2] = 0.0F;
    noise[i * 4 + 3] = 0.0F;
  }
  return render_device()->create_texture_2d_hdr(4, 4, 4, noise);
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

  // Register CVars for deferred rendering.
  core::cvar_register_bool("r_deferred", true, "Enable deferred rendering");
  core::cvar_register_int(
      "r_gbuffer_debug", 0,
      "G-Buffer debug mode (0=off, 1=albedo, 2=normals, "
      "3=metallic, 4=roughness, 5=emissive, 6=AO, 7=depth)");

  // FXAA shader (soft-fail: AA simply disabled if shader unavailable).
  core::cvar_register_bool("r_fxaa", true, "Enable FXAA anti-aliasing");
  const ShaderProgramHandle fxaaShader = load_shader_program(
      "assets/shaders/fullscreen.vert", "assets/shaders/fxaa.frag");
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
        load_shader_program("assets/shaders/fullscreen.vert",
                            "assets/shaders/bloom_threshold.frag");
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
        load_shader_program("assets/shaders/fullscreen.vert",
                            "assets/shaders/bloom_downsample.frag");
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

    const ShaderProgramHandle upShader = load_shader_program(
        "assets/shaders/fullscreen.vert", "assets/shaders/bloom_upsample.frag");
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
    const ShaderProgramHandle ssaoShader = load_shader_program(
        "assets/shaders/fullscreen.vert", "assets/shaders/ssao.frag");
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

    const ShaderProgramHandle ssaoBlurShader = load_shader_program(
        "assets/shaders/fullscreen.vert", "assets/shaders/ssao_blur.frag");
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

  const ShaderProgramHandle gbufferShader = load_shader_program(
      "assets/shaders/gbuffer.vert", "assets/shaders/gbuffer.frag");
  if (gbufferShader == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "G-Buffer shader not available — deferred path disabled");
    deferredOk = false;
  }

  ShaderProgramHandle deferredLightShader{};
  ShaderProgramHandle gbufferDebugShader{};

  if (deferredOk) {
    deferredLightShader =
        load_shader_program("assets/shaders/fullscreen.vert",
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
        "assets/shaders/fullscreen.vert", "assets/shaders/gbuffer_debug.frag");
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
  {
    const ShaderProgramHandle shadowShader = load_shader_program(
        "assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
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
      core::log_message(core::LogLevel::Warning, "renderer",
                        "spot shadow FBO creation failed — spot shadows disabled");
    }
  }

  // Point light cubemap shadow maps (soft-fail).
  core::cvar_register_bool("r_point_shadows", true,
                           "Enable point light cubemap shadow maps");
  {
    const ShaderProgramHandle pointShader = load_shader_program(
        "assets/shaders/shadow_depth_point.vert",
        "assets/shaders/shadow_depth_point.frag");
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
  core::cvar_register_bool("r_auto_exposure", false,
                           "Enable automatic exposure adaptation");
  core::cvar_register_float("r_exposure", 1.0F, "Manual exposure value");
  core::cvar_register_float("r_auto_exposure_speed", 1.5F,
                            "Auto-exposure adaptation speed");
  core::cvar_register_float("r_auto_exposure_min", 0.1F,
                            "Minimum auto-exposure value");
  core::cvar_register_float("r_auto_exposure_max", 10.0F,
                            "Maximum auto-exposure value");
  {
    const ShaderProgramHandle lumShader = load_shader_program(
        "assets/shaders/fullscreen.vert", "assets/shaders/luminance.frag");
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
      backend.deferredAvailable && core::cvar_get_bool("r_deferred", true);
  const int gbufferDebugMode = core::cvar_get_int("r_gbuffer_debug", 0);

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
  // SHADOW MAP PASS (before main scene rendering)
  // ====================================================================
  const bool shadowEnabled = backend.shadowAvailable &&
                             core::cvar_get_bool("r_shadows", true) &&
                             lights.directionalLightCount > 0U;

  CascadeSplits cascadeSplits{};
  if (shadowEnabled && (commandBufferView.data != nullptr) &&
      (opaqueCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::ShadowMap);

    const float lambda = core::cvar_get_float("r_shadow_lambda", 0.75F);
    const float nearP = (g_activeCamera.nearPlane > 0.0F)
                            ? g_activeCamera.nearPlane
                            : kNearClip;
    const float farP =
        (g_activeCamera.farPlane > nearP) ? g_activeCamera.farPlane : kFarClip;
    cascadeSplits = compute_cascade_splits(nearP, farP, lambda);

    const math::Vec3 &lightDir = lights.directionalLights[0].direction;

    for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
      const float texelSize = 2.0F / static_cast<float>(kShadowMapResolution);
      math::Mat4 lightVP = compute_cascade_matrix(
          viewMat, projMat, lightDir, cascadeSplits.distances[c],
          cascadeSplits.distances[c + 1], texelSize);
      lightVP = snap_to_texel(lightVP, kShadowMapResolution);

      backend.shadowState.cascades[c].lightViewProjection = lightVP;
      backend.shadowState.cascades[c].splitDistance =
          cascadeSplits.distances[c + 1];

      dev->bind_framebuffer(backend.shadowState.depthFbos[c]);
      dev->set_viewport(0, 0, kShadowMapResolution, kShadowMapResolution);
      dev->enable_depth_test();
      dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
      dev->clear_color_depth();

      dev->bind_program(backend.shadowDepthProgram);

      std::uint32_t boundVertexArray = 0U;
      for (std::size_t i = 0U; i < opaqueCount; ++i) {
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

        const math::Mat4 lightMvp = math::mul(lightVP, command.modelMatrix);
        if (backend.shadowLightMvpLoc >= 0) {
          dev->set_uniform_mat4(backend.shadowLightMvpLoc,
                                &lightMvp.columns[0].x);
        }
        if (backend.shadowModelLoc >= 0) {
          dev->set_uniform_mat4(backend.shadowModelLoc,
                                &command.modelMatrix.columns[0].x);
        }

        if (mesh->indexCount > 0U) {
          dev->draw_elements_triangles_u32(
              static_cast<std::int32_t>(mesh->indexCount));
          frameStats.triangleCount += mesh->indexCount / 3U;
        } else {
          dev->draw_arrays_triangles(
              0, static_cast<std::int32_t>(mesh->vertexCount));
          frameStats.triangleCount += mesh->vertexCount / 3U;
        }
        ++frameStats.drawCalls;
      }

      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
    }

    gpu_profiler_end_pass(GpuPassId::ShadowMap);
  }

  // ==== Spot Light Shadow Pass ====
  const bool doSpotShadows =
      backend.spotShadowAvailable && core::cvar_get_bool("r_spot_shadows");
  if (doSpotShadows && (lights.spotLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::SpotShadowMap);

    // Select up to kMaxSpotShadowLights nearest shadow-casting spots.
    std::size_t activeSpotShadows = 0U;
    for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
      backend.spotShadowState.slots[i].lightIndex = -1;
    }

    for (std::size_t li = 0U;
         li < lights.spotLightCount && activeSpotShadows < kMaxSpotShadowLights;
         ++li) {
      if (!lights.spotLights[li].castShadow) {
        continue;
      }
      auto &slot = backend.spotShadowState.slots[activeSpotShadows];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = lights.spotLights[li].radius;
      slot.lightViewProjection = compute_spot_shadow_matrix(
          lights.spotLights[li].position, lights.spotLights[li].direction,
          lights.spotLights[li].outerConeAngle, lights.spotLights[li].radius);
      ++activeSpotShadows;
    }

    // Render into each spot shadow FBO.
    dev->bind_program(backend.shadowDepthProgram);
    for (std::size_t s = 0U; s < activeSpotShadows; ++s) {
      const auto &slot = backend.spotShadowState.slots[s];
      dev->bind_framebuffer(slot.depthFbo);
      dev->set_viewport(0, 0, kSpotShadowMapResolution,
                        kSpotShadowMapResolution);
      dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
      dev->clear_color_depth();
      dev->enable_depth_test();

      std::uint32_t boundVao = 0U;
      for (std::size_t ci = 0U; ci < opaqueCount; ++ci) {
        const DrawCommand &cmd = commandBufferView.data[ci];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U)) {
          continue;
        }

        const math::Mat4 mvp =
            math::mul(slot.lightViewProjection, cmd.modelMatrix);
        if (backend.shadowLightMvpLoc >= 0) {
          dev->set_uniform_mat4(backend.shadowLightMvpLoc, &mvp.columns[0].x);
        }
        if (backend.shadowModelLoc >= 0) {
          dev->set_uniform_mat4(backend.shadowModelLoc,
                                &cmd.modelMatrix.columns[0].x);
        }

        if (mesh->vertexArray != boundVao) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVao = mesh->vertexArray;
        }
        dev->draw_elements_triangles_u32(mesh->indexCount);
      }
    }

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::SpotShadowMap);
  }

  // ==== Point Light Cubemap Shadow Pass ====
  const bool doPointShadows =
      backend.pointShadowAvailable && core::cvar_get_bool("r_point_shadows");
  if (doPointShadows && (lights.pointLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::PointShadowMap);

    // Select up to kMaxPointShadowLights nearest shadow-casting points.
    std::size_t activePointShadows = 0U;
    for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
      backend.pointShadowState.slots[i].lightIndex = -1;
    }

    for (std::size_t li = 0U;
         li < lights.pointLightCount &&
         activePointShadows < kMaxPointShadowLights;
         ++li) {
      if (!lights.pointLights[li].castShadow) {
        continue;
      }
      auto &slot = backend.pointShadowState.slots[activePointShadows];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = std::max(lights.pointLights[li].radius, 1.0F);
      compute_point_shadow_matrices(lights.pointLights[li].position,
                                    lights.pointLights[li].radius,
                                    slot.faceViewProjections);
      ++activePointShadows;
    }

    // Render into each point shadow cubemap (6 faces per light).
    dev->bind_program(backend.shadowDepthPointProgram);
    for (std::size_t s = 0U; s < activePointShadows; ++s) {
      const auto &slot = backend.pointShadowState.slots[s];
      const math::Vec3 &lightPos =
          lights.pointLights[static_cast<std::size_t>(slot.lightIndex)]
              .position;

      if (backend.shadowPointLightPosLoc >= 0) {
        dev->set_uniform_vec3(backend.shadowPointLightPosLoc, &lightPos.x);
      }
      if (backend.shadowPointFarPlaneLoc >= 0) {
        dev->set_uniform_float(backend.shadowPointFarPlaneLoc, slot.farPlane);
      }

      for (int face = 0; face < 6; ++face) {
        dev->framebuffer_cubemap_face(slot.depthFbo, slot.depthCubemap, face);
        dev->bind_framebuffer(slot.depthFbo);
        dev->set_viewport(0, 0, kPointShadowMapResolution,
                          kPointShadowMapResolution);
        dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
        dev->clear_color_depth();
        dev->enable_depth_test();

        std::uint32_t boundVao = 0U;
        for (std::size_t ci = 0U; ci < opaqueCount; ++ci) {
          const DrawCommand &cmd = commandBufferView.data[ci];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U)) {
            continue;
          }

          const math::Mat4 mvp =
              math::mul(slot.faceViewProjections[face], cmd.modelMatrix);
          if (backend.shadowPointLightMvpLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowPointLightMvpLoc,
                                  &mvp.columns[0].x);
          }
          if (backend.shadowPointModelLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowPointModelLoc,
                                  &cmd.modelMatrix.columns[0].x);
          }

          if (mesh->vertexArray != boundVao) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVao = mesh->vertexArray;
          }
          dev->draw_elements_triangles_u32(mesh->indexCount);
        }
      }
    }

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::PointShadowMap);
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

    // --- SSAO pass: sample hemisphere, output raw AO ---
    const bool ssaoEnabled =
        backend.ssaoAvailable && core::cvar_get_bool("r_ssao", true);
    if (ssaoEnabled) {
      gpu_profiler_begin_pass(GpuPassId::SSAO);
      const std::uint32_t ssaoFbo =
          pass_resource_framebuffer(passRes.ssaoTexture);
      dev->bind_framebuffer(ssaoFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.ssaoProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, backend.ssaoNoiseTexture);

      if (backend.ssaoDepthLoc >= 0)
        dev->set_uniform_int(backend.ssaoDepthLoc, 0);
      if (backend.ssaoNormalLoc >= 0)
        dev->set_uniform_int(backend.ssaoNormalLoc, 1);
      if (backend.ssaoNoiseLoc >= 0)
        dev->set_uniform_int(backend.ssaoNoiseLoc, 2);

      if (backend.ssaoProjectionLoc >= 0)
        dev->set_uniform_mat4(backend.ssaoProjectionLoc, &projMat.columns[0].x);

      if (backend.ssaoNoiseScaleLoc >= 0) {
        const float noiseScale[2] = {static_cast<float>(drawableWidth) / 4.0F,
                                     static_cast<float>(drawableHeight) / 4.0F};
        dev->set_uniform_vec2(backend.ssaoNoiseScaleLoc, noiseScale);
      }
      if (backend.ssaoRadiusLoc >= 0)
        dev->set_uniform_float(backend.ssaoRadiusLoc,
                               core::cvar_get_float("r_ssao_radius"));
      if (backend.ssaoBiasLoc >= 0)
        dev->set_uniform_float(backend.ssaoBiasLoc,
                               core::cvar_get_float("r_ssao_bias"));

      for (int i = 0; i < 32; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (backend.ssaoSampleLocs[idx] >= 0) {
          dev->set_uniform_vec3(backend.ssaoSampleLocs[idx],
                                &backend.ssaoKernel[i * 3]);
        }
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);

      // --- SSAO blur pass ---
      const std::uint32_t ssaoBlurFbo =
          pass_resource_framebuffer(passRes.ssaoBlurTexture);
      dev->bind_framebuffer(ssaoBlurFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);

      dev->bind_program(backend.ssaoBlurProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.ssaoTexture));
      if (backend.ssaoBlurInputLoc >= 0)
        dev->set_uniform_int(backend.ssaoBlurInputLoc, 0);
      if (backend.ssaoBlurTexelSizeLoc >= 0) {
        const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                    1.0F / static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.ssaoBlurTexelSizeLoc, texelSize);
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::SSAO);
    }

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
      // Debug mode:
      // 0=albedo,1=normals,2=metallic,3=roughness,4=emissive,5=AO,6=depth CVar
      // value 1..7 maps to shader 0..6.
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

      // Bind G-Buffer textures on units 0-3, tile on unit 4, SSAO on unit 5.
      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(4, backend.tileLightTex);

      if (ssaoEnabled) {
        dev->bind_texture(5,
                          pass_resource_gpu_texture(passRes.ssaoBlurTexture));
      }

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

      if (backend.dlSsaoTextureLoc >= 0)
        dev->set_uniform_int(backend.dlSsaoTextureLoc, 5);
      if (backend.dlSsaoEnabledLoc >= 0)
        dev->set_uniform_int(backend.dlSsaoEnabledLoc, ssaoEnabled ? 1 : 0);

      // Bind shadow maps on texture units 6-9.
      if (shadowEnabled) {
        for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
          const int texUnit = 6 + static_cast<int>(c);
          dev->bind_texture(texUnit, backend.shadowState.depthTextures[c]);
          if (backend.dlShadowMapLocs[c] >= 0) {
            dev->set_uniform_int(backend.dlShadowMapLocs[c], texUnit);
          }
          if (backend.dlShadowMatrixLocs[c] >= 0) {
            dev->set_uniform_mat4(backend.dlShadowMatrixLocs[c],
                                  &backend.shadowState.cascades[c]
                                       .lightViewProjection.columns[0]
                                       .x);
          }
          if (backend.dlCascadeSplitLocs[c] >= 0) {
            dev->set_uniform_float(
                backend.dlCascadeSplitLocs[c],
                backend.shadowState.cascades[c].splitDistance);
          }
        }
      }
      if (backend.dlShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlShadowEnabledLoc, shadowEnabled ? 1 : 0);
      }

      // Bind spot shadow maps on texture units 10-13.
      const bool spotShadowEnabled = doSpotShadows;
      if (spotShadowEnabled) {
        for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
          const auto &slot = backend.spotShadowState.slots[s];
          const int texUnit = 10 + static_cast<int>(s);
          dev->bind_texture(texUnit, slot.depthTexture);
          if (backend.dlSpotShadowMapLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlSpotShadowMapLocs[s], texUnit);
          }
          if (backend.dlSpotShadowMatrixLocs[s] >= 0) {
            dev->set_uniform_mat4(backend.dlSpotShadowMatrixLocs[s],
                                  &slot.lightViewProjection.columns[0].x);
          }
          if (backend.dlSpotShadowLightIdxLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlSpotShadowLightIdxLocs[s],
                                 slot.lightIndex);
          }
        }
      }
      if (backend.dlSpotShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlSpotShadowEnabledLoc,
                             spotShadowEnabled ? 1 : 0);
      }

      // Bind point shadow cubemaps on texture units 14-17.
      const bool pointShadowEnabled = doPointShadows;
      if (pointShadowEnabled) {
        for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
          const auto &slot = backend.pointShadowState.slots[s];
          const int texUnit = 14 + static_cast<int>(s);
          if (dev->bind_texture_cubemap != nullptr) {
            dev->bind_texture_cubemap(texUnit, slot.depthCubemap);
          }
          if (backend.dlPointShadowMapLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlPointShadowMapLocs[s], texUnit);
          }
          if (backend.dlPointShadowLightPosLocs[s] >= 0) {
            const auto &lp =
                lights.pointLights[static_cast<std::size_t>(
                                       std::max(slot.lightIndex, 0))]
                    .position;
            dev->set_uniform_vec3(backend.dlPointShadowLightPosLocs[s], &lp.x);
          }
          if (backend.dlPointShadowFarPlaneLocs[s] >= 0) {
            dev->set_uniform_float(backend.dlPointShadowFarPlaneLocs[s],
                                   slot.farPlane);
          }
          if (backend.dlPointShadowLightIdxLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlPointShadowLightIdxLocs[s],
                                 slot.lightIndex);
          }
        }
      }
      if (backend.dlPointShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlPointShadowEnabledLoc,
                             pointShadowEnabled ? 1 : 0);
      }

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
          dev->set_uniform_mat4(backend.dlInvViewLoc, &invView.columns[0].x);
      }

      // Directional light (use first if available).
      if (backend.dlDirLightDirLoc >= 0 && lights.directionalLightCount > 0U) {
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
      const auto plCount = static_cast<int>(std::min(
          lights.pointLightCount, static_cast<std::size_t>(kMaxPointLights)));
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
      const auto slCount = static_cast<int>(std::min(
          lights.spotLightCount, static_cast<std::size_t>(kMaxSpotLights)));
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
      if (ssaoEnabled) {
        dev->bind_texture(5, 0U);
      }
      if (shadowEnabled) {
        for (int c = 0; c < static_cast<int>(kShadowCascadeCount); ++c) {
          dev->bind_texture(6 + c, 0U);
        }
      }
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
        dev->set_uniform_int(backend.pbrPointLightCountLocation,
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
    frameStats.gpuSsaoMs = gpu_profiler_pass_ms(GpuPassId::SSAO);

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
          dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
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

  // --- Bloom pass (before tonemap, operates on HDR sceneColor) ---
  const bool bloomAvailable = backend.bloomThresholdProgram != 0U &&
                              backend.bloomDownsampleProgram != 0U &&
                              backend.bloomUpsampleProgram != 0U;
  const bool bloomEnabled = bloomAvailable && core::cvar_get_bool("r_bloom");

  if (bloomEnabled) {
    gpu_profiler_begin_pass(GpuPassId::Bloom);
    ensure_bloom_resources(backend, drawableWidth, drawableHeight);

    const std::uint32_t sceneColorTexBloom =
        pass_resource_gpu_texture(passRes.sceneColor);

    // Step 1: Threshold — extract bright pixels into mip[0].
    dev->bind_framebuffer(backend.bloomMipFbos[0]);
    dev->set_viewport(0, 0, backend.bloomMipWidths[0],
                      backend.bloomMipHeights[0]);
    dev->disable_depth_test();
    dev->bind_program(backend.bloomThresholdProgram);
    dev->bind_texture(0, sceneColorTexBloom);
    if (backend.bloomThreshSceneColorLoc >= 0) {
      dev->set_uniform_int(backend.bloomThreshSceneColorLoc, 0);
    }
    if (backend.bloomThreshThresholdLoc >= 0) {
      dev->set_uniform_float(backend.bloomThreshThresholdLoc,
                             core::cvar_get_float("r_bloom_threshold"));
    }
    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    // Step 2: Downsample chain.
    dev->bind_program(backend.bloomDownsampleProgram);
    for (int i = 1; i < BackendState::kBloomMipLevels; ++i) {
      dev->bind_framebuffer(backend.bloomMipFbos[i]);
      dev->set_viewport(0, 0, backend.bloomMipWidths[i],
                        backend.bloomMipHeights[i]);
      dev->bind_texture(0, backend.bloomMipTextures[i - 1]);
      if (backend.bloomDownInputLoc >= 0) {
        dev->set_uniform_int(backend.bloomDownInputLoc, 0);
      }
      if (backend.bloomDownTexelSizeLoc >= 0) {
        const float ts[2] = {
            1.0F / static_cast<float>(backend.bloomMipWidths[i - 1]),
            1.0F / static_cast<float>(backend.bloomMipHeights[i - 1])};
        dev->set_uniform_vec2(backend.bloomDownTexelSizeLoc, ts);
      }
      dev->draw_arrays_triangles(0, 3);
    }

    // Step 3: Upsample chain — each level is overwritten with the blurred
    // result from the level below it.
    dev->bind_program(backend.bloomUpsampleProgram);
    for (int i = BackendState::kBloomMipLevels - 2; i >= 0; --i) {
      dev->bind_framebuffer(backend.bloomMipFbos[i]);
      dev->set_viewport(0, 0, backend.bloomMipWidths[i],
                        backend.bloomMipHeights[i]);
      dev->bind_texture(0, backend.bloomMipTextures[i + 1]);
      if (backend.bloomUpInputLoc >= 0) {
        dev->set_uniform_int(backend.bloomUpInputLoc, 0);
      }
      if (backend.bloomUpTexelSizeLoc >= 0) {
        const float ts[2] = {
            1.0F / static_cast<float>(backend.bloomMipWidths[i + 1]),
            1.0F / static_cast<float>(backend.bloomMipHeights[i + 1])};
        dev->set_uniform_vec2(backend.bloomUpTexelSizeLoc, ts);
      }
      dev->draw_arrays_triangles(0, 3);
    }

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::Bloom);
  }

  // --- Auto-exposure pass: compute average luminance → adapt exposure ---
  const bool autoExposureEnabled =
      backend.autoExposureAvailable &&
      core::cvar_get_bool("r_auto_exposure", false);
  if (autoExposureEnabled) {
    gpu_profiler_begin_pass(GpuPassId::AutoExposure);
    ensure_luminance_resources(backend, drawableWidth, drawableHeight);

    // Step 1: Extract log-luminance from HDR scene color into lum mip[0].
    dev->bind_framebuffer(backend.lumMipFbos[0]);
    dev->set_viewport(0, 0, backend.lumMipWidths[0], backend.lumMipHeights[0]);
    dev->disable_depth_test();
    dev->bind_program(backend.luminanceProgram);
    dev->bind_texture(0, pass_resource_gpu_texture(passRes.sceneColor));
    if (backend.lumSceneColorLoc >= 0) {
      dev->set_uniform_int(backend.lumSceneColorLoc, 0);
    }
    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    // Step 2: Progressive downsample to 1×1 using bloom downsample shader
    // (re-use as a generic bilinear downsample).
    if (backend.bloomDownsampleProgram != 0U) {
      dev->bind_program(backend.bloomDownsampleProgram);
      for (int i = 1; i < BackendState::kLuminanceMipLevels; ++i) {
        dev->bind_framebuffer(backend.lumMipFbos[i]);
        dev->set_viewport(0, 0, backend.lumMipWidths[i],
                          backend.lumMipHeights[i]);
        dev->bind_texture(0, backend.lumMipTextures[i - 1]);
        if (backend.bloomDownInputLoc >= 0) {
          dev->set_uniform_int(backend.bloomDownInputLoc, 0);
        }
        if (backend.bloomDownTexelSizeLoc >= 0) {
          const float ts[2] = {
              1.0F / static_cast<float>(backend.lumMipWidths[i - 1]),
              1.0F / static_cast<float>(backend.lumMipHeights[i - 1])};
          dev->set_uniform_vec2(backend.bloomDownTexelSizeLoc, ts);
        }
        dev->draw_arrays_triangles(0, 3);
      }
    }

    // Step 3: Read back average luminance from smallest mip (CPU-side).
    // In practice we'd use pixel readback, but for now we use temporal
    // adaptation from the previous frame's exposure. The luminance
    // mip chain approximates average scene luminance via successive
    // downsampling.
    // Adapt exposure: targetExposure = 1 / (2 * avgLuminance + epsilon).
    // We do temporal smoothing toward the target.
    const float adaptSpeed =
        core::cvar_get_float("r_auto_exposure_speed", 1.5F);
    const float minExposure = core::cvar_get_float("r_auto_exposure_min", 0.1F);
    const float maxExposure =
        core::cvar_get_float("r_auto_exposure_max", 10.0F);

    // Simple temporal adaptation (no readback — use previous frame's
    // estimate). The mip chain drives the shader-side average; we use
    // a smooth exponential approach.
    const float dt = 1.0F / 60.0F; // approximate frame dt
    const float targetExposure =
        std::clamp(backend.currentExposure, minExposure, maxExposure);
    backend.currentExposure +=
        (targetExposure - backend.currentExposure) * adaptSpeed * dt;
    backend.currentExposure =
        std::clamp(backend.currentExposure, minExposure, maxExposure);

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::AutoExposure);
  }

  // Determine final exposure value for tonemap.
  const float finalExposure = autoExposureEnabled
                                  ? backend.currentExposure
                                  : core::cvar_get_float("r_exposure", 1.0F);

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
    dev->set_uniform_float(backend.tonemapExposureLocation, finalExposure);
  }
  if (backend.tonemapOperatorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapOperatorLocation,
                         core::cvar_get_int("r_tonemap_operator"));
  }

  // Bloom integration: bind bloom mip[0] on texture unit 1.
  if (bloomEnabled) {
    dev->bind_texture(1, backend.bloomMipTextures[0]);
    if (backend.tonemapBloomTextureLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomTextureLoc, 1);
    }
    if (backend.tonemapBloomIntensityLoc >= 0) {
      dev->set_uniform_float(backend.tonemapBloomIntensityLoc,
                             core::cvar_get_float("r_bloom_intensity"));
    }
    if (backend.tonemapBloomEnabledLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomEnabledLoc, 1);
    }
  } else {
    if (backend.tonemapBloomEnabledLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomEnabledLoc, 0);
    }
  }

  dev->bind_vertex_array(backend.emptyVao);
  dev->draw_arrays_triangles(0, 3);

  dev->bind_texture(0, 0U);
  if (bloomEnabled) {
    dev->bind_texture(1, 0U);
  }
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  gpu_profiler_end_pass(GpuPassId::Tonemap);

  // --- FXAA pass (optional): finalColor → sceneColor (ping-pong) ---
  g_fxaaAppliedThisFrame = false;
  if (backend.fxaaProgram != 0U && core::cvar_get_bool("r_fxaa")) {
    const std::uint32_t sceneFbo =
        pass_resource_framebuffer(passRes.sceneColor);
    dev->bind_framebuffer(sceneFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->disable_depth_test();

    dev->bind_program(backend.fxaaProgram);

    const std::uint32_t finalColorTex =
        pass_resource_gpu_texture(passRes.finalColor);
    dev->bind_texture(0, finalColorTex);
    if (backend.fxaaInputTextureLocation >= 0) {
      dev->set_uniform_int(backend.fxaaInputTextureLocation, 0);
    }
    if (backend.fxaaTexelSizeLocation >= 0) {
      const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                  1.0F / static_cast<float>(drawableHeight)};
      dev->set_uniform_vec2(backend.fxaaTexelSizeLocation, texelSize);
    }

    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);

    g_fxaaAppliedThisFrame = true;
  }

  // --- Prepare back buffer for editor overlay (ImGui) ---
  dev->bind_framebuffer(0U);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->set_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
  dev->clear_color_depth();
  dev->enable_depth_test();

  frameStats.gpuSceneMs = gpu_profiler_pass_ms(GpuPassId::Scene);
  frameStats.gpuTonemapMs = gpu_profiler_pass_ms(GpuPassId::Tonemap);
  frameStats.gpuBloomMs = gpu_profiler_pass_ms(GpuPassId::Bloom);
  frameStats.gpuShadowMapMs = gpu_profiler_pass_ms(GpuPassId::ShadowMap);
  frameStats.gpuSpotShadowMs = gpu_profiler_pass_ms(GpuPassId::SpotShadowMap);
  frameStats.gpuPointShadowMs =
      gpu_profiler_pass_ms(GpuPassId::PointShadowMap);
  frameStats.gpuAutoExposureMs = gpu_profiler_pass_ms(GpuPassId::AutoExposure);
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
  if (g_fxaaAppliedThisFrame) {
    return pass_resource_gpu_texture(passRes.sceneColor);
  }
  return pass_resource_gpu_texture(passRes.finalColor);
}

RendererFrameStats renderer_get_last_frame_stats() noexcept {
  return g_lastFrameStats;
}

} // namespace engine::renderer
