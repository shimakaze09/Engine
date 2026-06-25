// Implements command buffer behavior for the Engine renderer system.

#include "engine/renderer/command_buffer.h"
#include "command_buffer_math.h"

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
constexpr std::size_t kForwardMaxPointLights = 8U;
constexpr std::size_t kForwardMaxSpotLights = 8U;
constexpr std::uint32_t kInstanceModelAttrib0 = 3U;
constexpr std::uint32_t kInstanceModelAttribCount = 4U;
constexpr std::uint32_t kInstanceFoliageAttrib = 7U;
constexpr std::uint64_t kDrawKeyTransparentBit = 1ULL << 63U;
constexpr float kSkyboxCubeVertices[] = {
    -1.0F, 1.0F,  -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  -1.0F, -1.0F,
    1.0F,  -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, -1.0F, 1.0F,  -1.0F,

    -1.0F, -1.0F, 1.0F,  -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  -1.0F,
    -1.0F, 1.0F,  -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, -1.0F, 1.0F,

    1.0F,  -1.0F, -1.0F, 1.0F,  -1.0F, 1.0F,  1.0F,  1.0F,  1.0F,
    1.0F,  1.0F,  1.0F,  1.0F,  1.0F,  -1.0F, 1.0F,  -1.0F, -1.0F,

    -1.0F, -1.0F, 1.0F,  -1.0F, 1.0F,  1.0F,  1.0F,  1.0F,  1.0F,
    1.0F,  1.0F,  1.0F,  1.0F,  -1.0F, 1.0F,  -1.0F, -1.0F, 1.0F,

    -1.0F, 1.0F,  -1.0F, 1.0F,  1.0F,  -1.0F, 1.0F,  1.0F,  1.0F,
    1.0F,  1.0F,  1.0F,  -1.0F, 1.0F,  1.0F,  -1.0F, 1.0F,  -1.0F,

    -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, -1.0F,
    1.0F,  -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, 1.0F,
};
constexpr std::int32_t kSkyboxVertexCount = static_cast<std::int32_t>(
    sizeof(kSkyboxCubeVertices) / (3U * sizeof(float)));

/// Stores shadow candidate data used by the engine.
struct ShadowCandidate final {
  std::size_t lightIndex = 0U;
  float distSq = 0.0F;
};

/// Stores instance attributes data used by the engine.
struct InstanceAttributes final {
  math::Mat4 model = math::Mat4();
  math::Vec4 foliage = math::Vec4(0.0F, 0.0F, 0.0F, 0.0F);
};

/// Enumerates sky model values used by the engine.
enum class SkyModel : std::uint8_t {
  Hosek = 0,
  Preetham = 1,
  Cubemap = 2,
  None = 3,
};

/// Stores backend state data used by the engine.
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
  std::int32_t pbrOpacityLocation = -1;
  std::int32_t pbrViewLocation = -1;
  std::int32_t pbrViewProjectionLocation = -1;
  std::int32_t pbrUseInstancingLocation = -1;
  std::int32_t pbrFoliageWindStrengthLocation = -1;
  std::int32_t pbrFoliageWindFrequencyLocation = -1;
  std::int32_t pbrFoliagePhaseLocation = -1;
  std::int32_t pbrFogModeLocation = -1;
  std::int32_t pbrFogStartLocation = -1;
  std::int32_t pbrFogEndLocation = -1;
  std::int32_t pbrFogDensityLocation = -1;
  std::int32_t pbrFogColorLocation = -1;
  std::int32_t pbrHeightFogEnabledLocation = -1;
  std::int32_t pbrHeightFogBaseHeightLocation = -1;
  std::int32_t pbrHeightFogDensityLocation = -1;
  std::int32_t pbrHeightFogFalloffLocation = -1;
  std::int32_t pbrHeightFogStepCountLocation = -1;

  // Directional lights.
  std::int32_t pbrDirLightCountLocation = -1;
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightDir{};
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightColor{};
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightIntensity{};

  // Point lights.
  std::int32_t pbrPointLightCountLocation = -1;
  std::array<std::int32_t, kForwardMaxPointLights> pbrPointLightPos{};
  std::array<std::int32_t, kForwardMaxPointLights> pbrPointLightColor{};
  std::array<std::int32_t, kForwardMaxPointLights> pbrPointLightIntensity{};
  std::array<std::int32_t, kForwardMaxPointLights> pbrPointLightRadius{};

  // Spot lights.
  std::int32_t pbrSpotLightCountLocation = -1;
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightPos{};
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightDir{};
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightColor{};
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightIntensity{};
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightRadius{};
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightInnerCone{};
  std::array<std::int32_t, kForwardMaxSpotLights> pbrSpotLightOuterCone{};

  // PBR forward shadow uniforms.
  std::int32_t pbrShadowEnabledLoc = -1;
  std::array<std::int32_t, kShadowCascadeCount> pbrShadowMapLocs{};
  std::array<std::int32_t, kShadowCascadeCount> pbrShadowMatrixLocs{};
  std::array<std::int32_t, kShadowCascadeCount> pbrCascadeSplitLocs{};
  std::int32_t pbrSpotShadowEnabledLoc = -1;
  std::array<std::int32_t, kMaxSpotShadowLights> pbrSpotShadowMapLocs{};
  std::array<std::int32_t, kMaxSpotShadowLights> pbrSpotShadowMatrixLocs{};
  std::array<std::int32_t, kMaxSpotShadowLights> pbrSpotShadowLightIdxLocs{};
  std::int32_t pbrPointShadowEnabledLoc = -1;
  std::array<std::int32_t, kMaxPointShadowLights> pbrPointShadowMapLocs{};
  std::array<std::int32_t, kMaxPointShadowLights> pbrPointShadowLightPosLocs{};
  std::array<std::int32_t, kMaxPointShadowLights> pbrPointShadowFarPlaneLocs{};
  std::array<std::int32_t, kMaxPointShadowLights> pbrPointShadowLightIdxLocs{};

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

  // Skybox shader and cube geometry.
  bool skyboxAvailable = false;
  ShaderProgramHandle skyboxShaderHandle{};
  std::uint32_t skyboxProgram = 0U;
  std::int32_t skyboxViewLoc = -1;
  std::int32_t skyboxProjectionLoc = -1;
  std::int32_t skyboxTextureLoc = -1;
  std::uint32_t skyboxVertexArray = 0U;
  std::uint32_t skyboxVertexBuffer = 0U;

  bool preethamSkyAvailable = false;
  ShaderProgramHandle preethamSkyShaderHandle{};
  std::uint32_t preethamSkyProgram = 0U;
  std::int32_t preethamSkyViewLoc = -1;
  std::int32_t preethamSkyProjectionLoc = -1;
  std::int32_t preethamSkySunDirectionLoc = -1;
  std::int32_t preethamSkyTurbidityLoc = -1;

  bool hosekSkyAvailable = false;
  ShaderProgramHandle hosekSkyShaderHandle{};
  std::uint32_t hosekSkyProgram = 0U;
  std::int32_t hosekSkyViewLoc = -1;
  std::int32_t hosekSkyProjectionLoc = -1;
  std::int32_t hosekSkySunDirectionLoc = -1;
  std::int32_t hosekSkyTurbidityLoc = -1;
  std::int32_t hosekSkyGroundAlbedoLoc = -1;

  bool environmentPrefilterAvailable = false;
  ShaderProgramHandle environmentPrefilterShaderHandle{};
  std::uint32_t environmentPrefilterProgram = 0U;
  std::int32_t environmentPrefilterViewLoc = -1;
  std::int32_t environmentPrefilterProjectionLoc = -1;
  std::int32_t environmentPrefilterTextureLoc = -1;
  std::int32_t environmentPrefilterRoughnessLoc = -1;
  std::uint32_t prefilteredEnvironmentTexture = 0U;
  std::uint32_t environmentPrefilterFbo = 0U;
  std::uint32_t prefilteredEnvironmentSource = 0U;
  int prefilteredEnvironmentFaceSize = 0;
  int prefilteredEnvironmentMipLevels = 0;

  bool environmentIrradianceAvailable = false;
  ShaderProgramHandle environmentIrradianceShaderHandle{};
  std::uint32_t environmentIrradianceProgram = 0U;
  std::int32_t environmentIrradianceViewLoc = -1;
  std::int32_t environmentIrradianceProjectionLoc = -1;
  std::int32_t environmentIrradianceTextureLoc = -1;
  std::uint32_t irradianceEnvironmentTexture = 0U;
  std::uint32_t environmentIrradianceFbo = 0U;
  std::uint32_t irradianceEnvironmentSource = 0U;
  int irradianceEnvironmentFaceSize = 0;

  bool environmentBrdfLutAvailable = false;
  ShaderProgramHandle environmentBrdfLutShaderHandle{};
  std::uint32_t environmentBrdfLutProgram = 0U;
  std::uint32_t brdfLutTexture = 0U;
  std::uint32_t brdfLutFbo = 0U;
  int brdfLutSize = 0;

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
  std::int32_t gbufUseInstancingLoc = -1;
  std::int32_t gbufTimeLoc = -1;
  std::int32_t gbufFoliageWindStrengthLoc = -1;
  std::int32_t gbufFoliageWindFrequencyLoc = -1;
  std::int32_t gbufFoliagePhaseLoc = -1;
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
  std::int32_t dlFogModeLoc = -1;
  std::int32_t dlFogStartLoc = -1;
  std::int32_t dlFogEndLoc = -1;
  std::int32_t dlFogDensityLoc = -1;
  std::int32_t dlFogColorLoc = -1;
  std::int32_t dlHeightFogEnabledLoc = -1;
  std::int32_t dlHeightFogBaseHeightLoc = -1;
  std::int32_t dlHeightFogDensityLoc = -1;
  std::int32_t dlHeightFogFalloffLoc = -1;
  std::int32_t dlHeightFogStepCountLoc = -1;
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
  std::uint32_t instanceMatrixBuffer = 0U;
  std::vector<InstanceAttributes> instanceAttributes;
  std::vector<StaticMeshBatch> staticMeshBatches;

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
  std::uint64_t directionalShadowCacheKey = 0U;
  bool directionalShadowCacheValid = false;

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

/// Owns renderer state for the default renderer context.
struct RendererContext final {
  CameraState activeCamera{};
  int sceneViewportWidth = 0;
  int sceneViewportHeight = 0;
  RendererFrameStats lastFrameStats{};
  bool fxaaAppliedThisFrame = false;
  TextureHandle activeSkyboxTexture = kInvalidTextureHandle;
  char shaderRootPath[260] = "assets/shaders";
  BackendState backend{};
};

/// Returns the default renderer context used by the legacy renderer API.
RendererContext &renderer_context() noexcept {
  static RendererContext context{};
  return context;
}

/// Handles backend state.
BackendState &backend_state() noexcept {
  return renderer_context().backend;
}

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

/// Resets public renderer state that can otherwise leak between runs.
void reset_renderer_public_state() noexcept {
  renderer_context().activeCamera = CameraState{};
  renderer_context().sceneViewportWidth = 0;
  renderer_context().sceneViewportHeight = 0;
  renderer_context().lastFrameStats = RendererFrameStats{};
  renderer_context().fxaaAppliedThisFrame = false;
  renderer_context().activeSkyboxTexture = kInvalidTextureHandle;
}

/// Resets this object back to its reusable empty state for backend on failure.
void reset_backend_on_failure() noexcept {
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.failed = true;
}

/// Handles resolve pbr light uniforms.
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

/// Handles resolve pbr shadow uniforms.
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

/// Handles upload pbr lighting uniforms.
void upload_pbr_lighting_uniforms(const BackendState &backend,
                                  const RenderDevice *dev,
                                  const SceneLightData &lights) noexcept {
  const std::size_t dirCount =
      std::min(lights.directionalLightCount, kMaxDirectionalLights);
  if (backend.pbrDirLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrDirLightCountLocation,
                         static_cast<std::int32_t>(dirCount));
  }
  for (std::size_t i = 0U; i < dirCount; ++i) {
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

  const std::size_t pointCount =
      std::min(lights.pointLightCount, kForwardMaxPointLights);
  if (backend.pbrPointLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrPointLightCountLocation,
                         static_cast<std::int32_t>(pointCount));
  }
  for (std::size_t i = 0U; i < pointCount; ++i) {
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
    if (backend.pbrPointLightRadius[i] >= 0) {
      dev->set_uniform_float(backend.pbrPointLightRadius[i], pl.radius);
    }
  }

  const std::size_t spotCount =
      std::min(lights.spotLightCount, kForwardMaxSpotLights);
  if (backend.pbrSpotLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrSpotLightCountLocation,
                         static_cast<std::int32_t>(spotCount));
  }
  for (std::size_t i = 0U; i < spotCount; ++i) {
    const auto &sl = lights.spotLights[i];
    if (backend.pbrSpotLightPos[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrSpotLightPos[i], &sl.position.x);
    }
    if (backend.pbrSpotLightDir[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrSpotLightDir[i], &sl.direction.x);
    }
    if (backend.pbrSpotLightColor[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrSpotLightColor[i], &sl.color.x);
    }
    if (backend.pbrSpotLightIntensity[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightIntensity[i], sl.intensity);
    }
    if (backend.pbrSpotLightRadius[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightRadius[i], sl.radius);
    }
    if (backend.pbrSpotLightInnerCone[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightInnerCone[i],
                             sl.innerConeAngle);
    }
    if (backend.pbrSpotLightOuterCone[i] >= 0) {
      dev->set_uniform_float(backend.pbrSpotLightOuterCone[i],
                             sl.outerConeAngle);
    }
  }
}

struct DistanceFogUniformLocations final {
  int mode = -1;
  int start = -1;
  int end = -1;
  int density = -1;
  int color = -1;
};

struct HeightFogUniformLocations final {
  int enabled = -1;
  int baseHeight = -1;
  int density = -1;
  int falloff = -1;
  int stepCount = -1;
};

void upload_distance_fog_uniforms(
    const RenderDevice *dev, const DistanceFogUniformLocations &locations,
    const DistanceFogSettings &settings) noexcept {
  const DistanceFogSettings fog = normalize_distance_fog_settings(settings);
  if (locations.mode >= 0) {
    dev->set_uniform_int(locations.mode, static_cast<std::int32_t>(fog.mode));
  }
  if (locations.start >= 0) {
    dev->set_uniform_float(locations.start, fog.start);
  }
  if (locations.end >= 0) {
    dev->set_uniform_float(locations.end, fog.end);
  }
  if (locations.density >= 0) {
    dev->set_uniform_float(locations.density, fog.density);
  }
  if (locations.color >= 0) {
    dev->set_uniform_vec3(locations.color, &fog.color.x);
  }
}

void upload_height_fog_uniforms(
    const RenderDevice *dev, const HeightFogUniformLocations &locations,
    const HeightFogSettings &settings) noexcept {
  const HeightFogSettings fog = normalize_height_fog_settings(settings);
  if (locations.enabled >= 0) {
    dev->set_uniform_int(locations.enabled, fog.enabled ? 1 : 0);
  }
  if (locations.baseHeight >= 0) {
    dev->set_uniform_float(locations.baseHeight, fog.baseHeight);
  }
  if (locations.density >= 0) {
    dev->set_uniform_float(locations.density, fog.density);
  }
  if (locations.falloff >= 0) {
    dev->set_uniform_float(locations.falloff, fog.falloff);
  }
  if (locations.stepCount >= 0) {
    dev->set_uniform_int(locations.stepCount, fog.stepCount);
  }
}

/// Handles upload pbr distance fog uniforms.
void upload_pbr_distance_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const DistanceFogSettings &settings) noexcept {
  upload_distance_fog_uniforms(
      dev,
      DistanceFogUniformLocations{backend.pbrFogModeLocation,
                                  backend.pbrFogStartLocation,
                                  backend.pbrFogEndLocation,
                                  backend.pbrFogDensityLocation,
                                  backend.pbrFogColorLocation},
      settings);
}

/// Handles upload pbr height fog uniforms.
void upload_pbr_height_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const HeightFogSettings &settings) noexcept {
  upload_height_fog_uniforms(
      dev,
      HeightFogUniformLocations{backend.pbrHeightFogEnabledLocation,
                                backend.pbrHeightFogBaseHeightLocation,
                                backend.pbrHeightFogDensityLocation,
                                backend.pbrHeightFogFalloffLocation,
                                backend.pbrHeightFogStepCountLocation},
      settings);
}

/// Handles upload pbr foliage uniforms.
void upload_pbr_foliage_uniforms(const BackendState &backend,
                                 const RenderDevice *dev,
                                 const DrawCommand &command) noexcept {
  if (backend.pbrFoliageWindStrengthLocation >= 0) {
    dev->set_uniform_float(backend.pbrFoliageWindStrengthLocation,
                           command.foliageWindStrength);
  }
  if (backend.pbrFoliageWindFrequencyLocation >= 0) {
    dev->set_uniform_float(backend.pbrFoliageWindFrequencyLocation,
                           command.foliageWindFrequency);
  }
  if (backend.pbrFoliagePhaseLocation >= 0) {
    dev->set_uniform_float(backend.pbrFoliagePhaseLocation,
                           command.foliageWindPhase);
  }
}

/// Handles upload gbuffer foliage uniforms.
void upload_gbuffer_foliage_uniforms(const BackendState &backend,
                                     const RenderDevice *dev,
                                     const DrawCommand &command) noexcept {
  if (backend.gbufFoliageWindStrengthLoc >= 0) {
    dev->set_uniform_float(backend.gbufFoliageWindStrengthLoc,
                           command.foliageWindStrength);
  }
  if (backend.gbufFoliageWindFrequencyLoc >= 0) {
    dev->set_uniform_float(backend.gbufFoliageWindFrequencyLoc,
                           command.foliageWindFrequency);
  }
  if (backend.gbufFoliagePhaseLoc >= 0) {
    dev->set_uniform_float(backend.gbufFoliagePhaseLoc,
                           command.foliageWindPhase);
  }
}

/// Handles upload deferred distance fog uniforms.
void upload_deferred_distance_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const DistanceFogSettings &settings) noexcept {
  upload_distance_fog_uniforms(
      dev,
      DistanceFogUniformLocations{backend.dlFogModeLoc, backend.dlFogStartLoc,
                                  backend.dlFogEndLoc, backend.dlFogDensityLoc,
                                  backend.dlFogColorLoc},
      settings);
}

/// Handles upload deferred height fog uniforms.
void upload_deferred_height_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const HeightFogSettings &settings) noexcept {
  upload_height_fog_uniforms(
      dev,
      HeightFogUniformLocations{backend.dlHeightFogEnabledLoc,
                                backend.dlHeightFogBaseHeightLoc,
                                backend.dlHeightFogDensityLoc,
                                backend.dlHeightFogFalloffLoc,
                                backend.dlHeightFogStepCountLoc},
      settings);
}

/// Handles bind pbr shadow uniforms.
void bind_pbr_shadow_uniforms(const BackendState &backend,
                              const RenderDevice *dev,
                              const SceneLightData &lights, bool shadowEnabled,
                              bool spotShadowEnabled,
                              bool pointShadowEnabled) noexcept {
  if ((dev == nullptr) || (dev->set_uniform_int == nullptr)) {
    return;
  }

  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    const int texUnit = 6 + static_cast<int>(c);
    if (shadowEnabled) {
      dev->bind_texture(texUnit, backend.shadowState.depthTextures[c]);
    }
    if (backend.pbrShadowMapLocs[c] >= 0) {
      dev->set_uniform_int(backend.pbrShadowMapLocs[c], texUnit);
    }
    if (backend.pbrShadowMatrixLocs[c] >= 0) {
      dev->set_uniform_mat4(
          backend.pbrShadowMatrixLocs[c],
          &backend.shadowState.cascades[c].lightViewProjection.columns[0].x);
    }
    if (backend.pbrCascadeSplitLocs[c] >= 0) {
      dev->set_uniform_float(backend.pbrCascadeSplitLocs[c],
                             backend.shadowState.cascades[c].splitDistance);
    }
  }
  if (backend.pbrShadowEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrShadowEnabledLoc, shadowEnabled ? 1 : 0);
  }

  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    const auto &slot = backend.spotShadowState.slots[s];
    const int texUnit = 10 + static_cast<int>(s);
    if (spotShadowEnabled) {
      dev->bind_texture(texUnit, slot.depthTexture);
    }
    if (backend.pbrSpotShadowMapLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrSpotShadowMapLocs[s], texUnit);
    }
    if (backend.pbrSpotShadowMatrixLocs[s] >= 0) {
      dev->set_uniform_mat4(backend.pbrSpotShadowMatrixLocs[s],
                            &slot.lightViewProjection.columns[0].x);
    }
    if (backend.pbrSpotShadowLightIdxLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrSpotShadowLightIdxLocs[s],
                           slot.lightIndex);
    }
  }
  if (backend.pbrSpotShadowEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrSpotShadowEnabledLoc,
                         spotShadowEnabled ? 1 : 0);
  }

  const math::Vec3 zero{};
  for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
    const auto &slot = backend.pointShadowState.slots[s];
    const int texUnit = 14 + static_cast<int>(s);
    if (pointShadowEnabled && (dev->bind_texture_cubemap != nullptr)) {
      dev->bind_texture_cubemap(texUnit, slot.depthCubemap);
    }
    if (backend.pbrPointShadowMapLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrPointShadowMapLocs[s], texUnit);
    }
    if (backend.pbrPointShadowLightPosLocs[s] >= 0) {
      const bool validLight =
          (slot.lightIndex >= 0) &&
          (static_cast<std::size_t>(slot.lightIndex) < lights.pointLightCount);
      const math::Vec3 &lightPos =
          validLight
              ? lights.pointLights[static_cast<std::size_t>(slot.lightIndex)]
                    .position
              : zero;
      dev->set_uniform_vec3(backend.pbrPointShadowLightPosLocs[s], &lightPos.x);
    }
    if (backend.pbrPointShadowFarPlaneLocs[s] >= 0) {
      dev->set_uniform_float(backend.pbrPointShadowFarPlaneLocs[s],
                             slot.farPlane);
    }
    if (backend.pbrPointShadowLightIdxLocs[s] >= 0) {
      dev->set_uniform_int(backend.pbrPointShadowLightIdxLocs[s],
                           slot.lightIndex);
    }
  }
  if (backend.pbrPointShadowEnabledLoc >= 0) {
    dev->set_uniform_int(backend.pbrPointShadowEnabledLoc,
                         pointShadowEnabled ? 1 : 0);
  }
}

/// Handles unbind pbr shadow textures.
void unbind_pbr_shadow_textures(const RenderDevice *dev) noexcept {
  if (dev == nullptr) {
    return;
  }
  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    dev->bind_texture(6 + static_cast<int>(c), 0U);
  }
  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    dev->bind_texture(10 + static_cast<int>(s), 0U);
  }
  if (dev->bind_texture_cubemap != nullptr) {
    for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
      dev->bind_texture_cubemap(14 + static_cast<int>(s), 0U);
    }
  }
}

/// Destroys or releases the requested object, handle, or resource for skybox resources.
void destroy_skybox_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.skyboxVertexBuffer != 0U) && (dev != nullptr)) {
    dev->destroy_buffer(backend.skyboxVertexBuffer);
    backend.skyboxVertexBuffer = 0U;
  }
  if ((backend.skyboxVertexArray != 0U) && (dev != nullptr)) {
    dev->destroy_vertex_array(backend.skyboxVertexArray);
    backend.skyboxVertexArray = 0U;
  }
  if (backend.skyboxShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.skyboxShaderHandle);
    backend.skyboxShaderHandle = ShaderProgramHandle{};
  }
  if (backend.preethamSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.preethamSkyShaderHandle);
    backend.preethamSkyShaderHandle = ShaderProgramHandle{};
  }
  if (backend.hosekSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.hosekSkyShaderHandle);
    backend.hosekSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.skyboxProgram = 0U;
  backend.skyboxAvailable = false;
  backend.preethamSkyProgram = 0U;
  backend.preethamSkyAvailable = false;
  backend.hosekSkyProgram = 0U;
  backend.hosekSkyAvailable = false;
}

/// Destroys or releases the requested object, handle, or resource for preetham sky resources.
void destroy_preetham_sky_resources(BackendState &backend) noexcept {
  if (backend.preethamSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.preethamSkyShaderHandle);
    backend.preethamSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.preethamSkyProgram = 0U;
  backend.preethamSkyAvailable = false;
  backend.preethamSkyViewLoc = -1;
  backend.preethamSkyProjectionLoc = -1;
  backend.preethamSkySunDirectionLoc = -1;
  backend.preethamSkyTurbidityLoc = -1;
}

/// Destroys or releases the requested object, handle, or resource for hosek sky resources.
void destroy_hosek_sky_resources(BackendState &backend) noexcept {
  if (backend.hosekSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.hosekSkyShaderHandle);
    backend.hosekSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.hosekSkyProgram = 0U;
  backend.hosekSkyAvailable = false;
  backend.hosekSkyViewLoc = -1;
  backend.hosekSkyProjectionLoc = -1;
  backend.hosekSkySunDirectionLoc = -1;
  backend.hosekSkyTurbidityLoc = -1;
  backend.hosekSkyGroundAlbedoLoc = -1;
}

/// Creates a new object, handle, or resource for skybox geometry.
bool create_skybox_geometry(BackendState &backend,
                            const RenderDevice *dev) noexcept {
  if ((backend.skyboxVertexArray != 0U) && (backend.skyboxVertexBuffer != 0U)) {
    return true;
  }

  if ((dev == nullptr) || (dev->create_vertex_array == nullptr) ||
      (dev->create_buffer == nullptr) || (dev->bind_vertex_array == nullptr) ||
      (dev->bind_array_buffer == nullptr) ||
      (dev->buffer_data_array == nullptr) ||
      (dev->enable_vertex_attrib == nullptr) ||
      (dev->vertex_attrib_float == nullptr)) {
    return false;
  }

  backend.skyboxVertexArray = dev->create_vertex_array();
  backend.skyboxVertexBuffer = dev->create_buffer();
  if ((backend.skyboxVertexArray == 0U) || (backend.skyboxVertexBuffer == 0U)) {
    destroy_skybox_resources(backend);
    return false;
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->bind_array_buffer(backend.skyboxVertexBuffer);
  dev->buffer_data_array(kSkyboxCubeVertices, sizeof(kSkyboxCubeVertices));
  dev->enable_vertex_attrib(0U);
  dev->vertex_attrib_float(0U, 3, static_cast<std::int32_t>(3 * sizeof(float)),
                           nullptr);
  dev->bind_array_buffer(0U);
  dev->bind_vertex_array(0U);
  return true;
}

/// Handles cvar string equals.
bool cvar_string_equals(const char *lhs, const char *rhs) noexcept {
  return (lhs != nullptr) && (rhs != nullptr) && (std::strcmp(lhs, rhs) == 0);
}

/// Handles selected sky model.
SkyModel selected_sky_model() noexcept {
  const char *model = core::cvar_get_string("r_sky_model", "hosek");
  if (cvar_string_equals(model, "cubemap")) {
    return SkyModel::Cubemap;
  }
  if (cvar_string_equals(model, "preetham")) {
    return SkyModel::Preetham;
  }
  if (cvar_string_equals(model, "none")) {
    return SkyModel::None;
  }
  return SkyModel::Hosek;
}

/// Handles distance fog settings from cvars.
DistanceFogSettings distance_fog_settings_from_cvars() noexcept {
  DistanceFogSettings settings{};
  settings.mode =
      parse_distance_fog_mode(core::cvar_get_string("r_fog_mode", "exp2"));
  settings.start = core::cvar_get_float("r_fog_start", settings.start);
  settings.end = core::cvar_get_float("r_fog_end", settings.end);
  settings.density = core::cvar_get_float("r_fog_density", settings.density);

  math::Vec3 color = settings.color;
  if (parse_distance_fog_color(
          core::cvar_get_string("r_fog_color", "0.55 0.65 0.75"), &color)) {
    settings.color = color;
  }

  return normalize_distance_fog_settings(settings);
}

/// Handles height fog settings from cvars.
HeightFogSettings height_fog_settings_from_cvars() noexcept {
  HeightFogSettings settings{};
  settings.enabled = core::cvar_get_bool("r_height_fog", settings.enabled);
  settings.baseHeight =
      core::cvar_get_float("r_height_fog_base", settings.baseHeight);
  settings.density =
      core::cvar_get_float("r_height_fog_density", settings.density);
  settings.falloff =
      core::cvar_get_float("r_height_fog_falloff", settings.falloff);
  settings.stepCount =
      core::cvar_get_int("r_height_fog_steps", settings.stepCount);
  return normalize_height_fog_settings(settings);
}

/// Handles active skybox gpu texture.
std::uint32_t active_skybox_gpu_texture(const BackendState &backend) noexcept {
  if (!backend.skyboxAvailable ||
      (renderer_context().activeSkyboxTexture == kInvalidTextureHandle) ||
      !is_texture_cubemap(renderer_context().activeSkyboxTexture)) {
    return 0U;
  }

  return texture_gpu_id(renderer_context().activeSkyboxTexture);
}

/// Handles preetham sun direction.
math::Vec3 preetham_sun_direction(const SceneLightData &lights) noexcept {
  if (lights.directionalLightCount > 0U) {
    const math::Vec3 sunDir =
        math::normalize(math::negate(lights.directionalLights[0].direction));
    if (math::length_sq(sunDir) > 0.0F) {
      return sunDir;
    }
  }

  return math::normalize(math::Vec3(0.25F, 0.85F, 0.45F));
}

/// Handles prepare procedural sky draw.
void prepare_procedural_sky_draw(const RenderDevice *dev) noexcept {
  dev->enable_depth_test();
  dev->set_depth_func_less_equal();
  dev->set_depth_mask(false);
  dev->disable_face_culling();
}

/// Handles finish procedural sky draw.
void finish_procedural_sky_draw(const RenderDevice *dev) noexcept {
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  dev->set_depth_mask(true);
  dev->set_depth_func_less();
  dev->enable_face_culling();
}

/// Handles draw skybox.
void draw_skybox(const BackendState &backend, const RenderDevice *dev,
                 const math::Mat4 &viewMat, const math::Mat4 &projMat,
                 std::uint32_t cubemapGpuId,
                 RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || (cubemapGpuId == 0U) ||
      (dev->bind_texture_cubemap == nullptr) ||
      (dev->set_depth_func_less_equal == nullptr) ||
      (dev->set_depth_func_less == nullptr)) {
    return;
  }

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.skyboxProgram);
  if (backend.skyboxViewLoc >= 0) {
    dev->set_uniform_mat4(backend.skyboxViewLoc, &viewMat.columns[0].x);
  }
  if (backend.skyboxProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.skyboxProjectionLoc, &projMat.columns[0].x);
  }
  if (backend.skyboxTextureLoc >= 0) {
    dev->set_uniform_int(backend.skyboxTextureLoc, 0);
  }

  dev->bind_texture_cubemap(0, cubemapGpuId);
  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->draw_arrays_triangles(0, kSkyboxVertexCount);

  dev->bind_texture_cubemap(0, 0U);
  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

/// Handles draw preetham sky.
void draw_preetham_sky(const BackendState &backend, const RenderDevice *dev,
                       const math::Mat4 &viewMat, const math::Mat4 &projMat,
                       const SceneLightData &lights,
                       RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || !backend.preethamSkyAvailable ||
      (dev->set_depth_func_less_equal == nullptr) ||
      (dev->set_depth_func_less == nullptr)) {
    return;
  }

  const math::Vec3 sunDir = preetham_sun_direction(lights);
  const float turbidity = core::cvar_get_float("r_sky_turbidity", 3.0F);

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.preethamSkyProgram);
  if (backend.preethamSkyViewLoc >= 0) {
    dev->set_uniform_mat4(backend.preethamSkyViewLoc, &viewMat.columns[0].x);
  }
  if (backend.preethamSkyProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.preethamSkyProjectionLoc,
                          &projMat.columns[0].x);
  }
  if (backend.preethamSkySunDirectionLoc >= 0) {
    dev->set_uniform_vec3(backend.preethamSkySunDirectionLoc, &sunDir.x);
  }
  if (backend.preethamSkyTurbidityLoc >= 0) {
    dev->set_uniform_float(backend.preethamSkyTurbidityLoc, turbidity);
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->draw_arrays_triangles(0, kSkyboxVertexCount);

  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

/// Handles draw hosek sky.
void draw_hosek_sky(const BackendState &backend, const RenderDevice *dev,
                    const math::Mat4 &viewMat, const math::Mat4 &projMat,
                    const SceneLightData &lights,
                    RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || !backend.hosekSkyAvailable ||
      (dev->set_depth_func_less_equal == nullptr) ||
      (dev->set_depth_func_less == nullptr)) {
    return;
  }

  const math::Vec3 sunDir = preetham_sun_direction(lights);
  const float turbidity = core::cvar_get_float("r_sky_turbidity", 3.0F);
  const float groundAlbedo = core::cvar_get_float("r_sky_ground_albedo", 0.1F);

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.hosekSkyProgram);
  if (backend.hosekSkyViewLoc >= 0) {
    dev->set_uniform_mat4(backend.hosekSkyViewLoc, &viewMat.columns[0].x);
  }
  if (backend.hosekSkyProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.hosekSkyProjectionLoc, &projMat.columns[0].x);
  }
  if (backend.hosekSkySunDirectionLoc >= 0) {
    dev->set_uniform_vec3(backend.hosekSkySunDirectionLoc, &sunDir.x);
  }
  if (backend.hosekSkyTurbidityLoc >= 0) {
    dev->set_uniform_float(backend.hosekSkyTurbidityLoc, turbidity);
  }
  if (backend.hosekSkyGroundAlbedoLoc >= 0) {
    dev->set_uniform_float(backend.hosekSkyGroundAlbedoLoc, groundAlbedo);
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->draw_arrays_triangles(0, kSkyboxVertexCount);

  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

/// Handles cubemap mip size.
int cubemap_mip_size(int faceSize, int mipLevel) noexcept {
  int size = faceSize;
  for (int mip = 0; mip < mipLevel; ++mip) {
    size = std::max(1, size / 2);
  }
  return size;
}

/// Handles positive cvar u32.
std::uint32_t positive_cvar_u32(const char *name, int fallback) noexcept {
  const int value = core::cvar_get_int(name, fallback);
  return (value > 0) ? static_cast<std::uint32_t>(value) : 0U;
}

/// Handles cvar reflection probe bake settings.
ReflectionProbeBakeSettings cvar_reflection_probe_bake_settings() noexcept {
  ReflectionProbeBakeSettings settings{};
  settings.prefilteredFaceSize =
      positive_cvar_u32("r_env_prefilter_size", 128);
  settings.prefilteredMipLevels =
      positive_cvar_u32("r_env_prefilter_mips", 5);
  settings.irradianceFaceSize =
      positive_cvar_u32("r_env_irradiance_size", 32);
  settings.brdfLutSize = positive_cvar_u32("r_env_brdf_lut_size", 512);
  return normalize_reflection_probe_bake_settings(settings);
}

/// Handles cubemap capture views.
void cubemap_capture_views(std::array<math::Mat4, 6> &outViews) noexcept {
  const math::Vec3 origin{};
  outViews[0] = math::look_at(origin, math::Vec3(1.0F, 0.0F, 0.0F),
                              math::Vec3(0.0F, -1.0F, 0.0F));
  outViews[1] = math::look_at(origin, math::Vec3(-1.0F, 0.0F, 0.0F),
                              math::Vec3(0.0F, -1.0F, 0.0F));
  outViews[2] = math::look_at(origin, math::Vec3(0.0F, 1.0F, 0.0F),
                              math::Vec3(0.0F, 0.0F, 1.0F));
  outViews[3] = math::look_at(origin, math::Vec3(0.0F, -1.0F, 0.0F),
                              math::Vec3(0.0F, 0.0F, -1.0F));
  outViews[4] = math::look_at(origin, math::Vec3(0.0F, 0.0F, 1.0F),
                              math::Vec3(0.0F, -1.0F, 0.0F));
  outViews[5] = math::look_at(origin, math::Vec3(0.0F, 0.0F, -1.0F),
                              math::Vec3(0.0F, -1.0F, 0.0F));
}

/// Destroys or releases the requested object, handle, or resource for environment prefilter resources.
void destroy_environment_prefilter_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.prefilteredEnvironmentTexture != 0U) && (dev != nullptr)) {
    dev->destroy_texture(backend.prefilteredEnvironmentTexture);
    backend.prefilteredEnvironmentTexture = 0U;
  }
  if ((backend.environmentPrefilterFbo != 0U) && (dev != nullptr)) {
    dev->destroy_framebuffer(backend.environmentPrefilterFbo);
    backend.environmentPrefilterFbo = 0U;
  }
  if (backend.environmentPrefilterShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.environmentPrefilterShaderHandle);
    backend.environmentPrefilterShaderHandle = ShaderProgramHandle{};
  }
  backend.environmentPrefilterProgram = 0U;
  backend.environmentPrefilterAvailable = false;
  backend.prefilteredEnvironmentSource = 0U;
  backend.prefilteredEnvironmentFaceSize = 0;
  backend.prefilteredEnvironmentMipLevels = 0;
}

/// Handles ensure prefiltered environment.
std::uint32_t
ensure_prefiltered_environment(BackendState &backend, const RenderDevice *dev,
                               std::uint32_t sourceCubemap,
                               ReflectionProbeBakeSettings settings) noexcept {
  if ((dev == nullptr) || !backend.environmentPrefilterAvailable ||
      !core::cvar_get_bool("r_env_prefilter", true) || (sourceCubemap == 0U) ||
      (dev->create_cubemap_hdr_empty == nullptr) ||
      (dev->framebuffer_cubemap_color_face_mip == nullptr) ||
      (dev->bind_texture_cubemap == nullptr)) {
    return 0U;
  }

  settings = normalize_reflection_probe_bake_settings(settings);
  const int faceSize = static_cast<int>(settings.prefilteredFaceSize);
  const int mipLevels = static_cast<int>(settings.prefilteredMipLevels);

  if ((backend.prefilteredEnvironmentTexture != 0U) &&
      (backend.prefilteredEnvironmentSource == sourceCubemap) &&
      (backend.prefilteredEnvironmentFaceSize == faceSize) &&
      (backend.prefilteredEnvironmentMipLevels == mipLevels)) {
    return backend.prefilteredEnvironmentTexture;
  }

  if (backend.prefilteredEnvironmentTexture != 0U) {
    dev->destroy_texture(backend.prefilteredEnvironmentTexture);
    backend.prefilteredEnvironmentTexture = 0U;
  }

  if (backend.environmentPrefilterFbo == 0U) {
    backend.environmentPrefilterFbo = dev->create_framebuffer(0U, 0U);
    if (backend.environmentPrefilterFbo == 0U) {
      return 0U;
    }
  }

  const std::uint32_t prefiltered =
      dev->create_cubemap_hdr_empty(faceSize, mipLevels);
  if (prefiltered == 0U) {
    return 0U;
  }

  std::array<math::Mat4, 6> views{};
  cubemap_capture_views(views);
  const math::Mat4 projection =
      math::perspective(1.57079632679F, 1.0F, 0.1F, 10.0F);

  dev->disable_depth_test();
  dev->disable_face_culling();
  dev->bind_program(backend.environmentPrefilterProgram);
  dev->bind_texture_cubemap(0, sourceCubemap);
  if (backend.environmentPrefilterTextureLoc >= 0) {
    dev->set_uniform_int(backend.environmentPrefilterTextureLoc, 0);
  }
  if (backend.environmentPrefilterProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.environmentPrefilterProjectionLoc,
                          &projection.columns[0].x);
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  for (int mip = 0; mip < mipLevels; ++mip) {
    const int mipSize = cubemap_mip_size(faceSize, mip);
    const float roughness =
        (mipLevels > 1)
            ? static_cast<float>(mip) / static_cast<float>(mipLevels - 1)
            : 0.0F;
    dev->set_viewport(0, 0, mipSize, mipSize);
    if (backend.environmentPrefilterRoughnessLoc >= 0) {
      dev->set_uniform_float(backend.environmentPrefilterRoughnessLoc,
                             roughness);
    }

    for (int face = 0; face < 6; ++face) {
      dev->framebuffer_cubemap_color_face_mip(backend.environmentPrefilterFbo,
                                              prefiltered, face, mip);
      if (backend.environmentPrefilterViewLoc >= 0) {
        dev->set_uniform_mat4(
            backend.environmentPrefilterViewLoc,
            &views[static_cast<std::size_t>(face)].columns[0].x);
      }
      dev->draw_arrays_triangles(0, kSkyboxVertexCount);
    }
  }

  dev->bind_texture_cubemap(0, 0U);
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);

  backend.prefilteredEnvironmentTexture = prefiltered;
  backend.prefilteredEnvironmentSource = sourceCubemap;
  backend.prefilteredEnvironmentFaceSize = faceSize;
  backend.prefilteredEnvironmentMipLevels = mipLevels;
  return prefiltered;
}

/// Destroys or releases the requested object, handle, or resource for environment irradiance resources.
void destroy_environment_irradiance_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.irradianceEnvironmentTexture != 0U) && (dev != nullptr)) {
    dev->destroy_texture(backend.irradianceEnvironmentTexture);
    backend.irradianceEnvironmentTexture = 0U;
  }
  if ((backend.environmentIrradianceFbo != 0U) && (dev != nullptr)) {
    dev->destroy_framebuffer(backend.environmentIrradianceFbo);
    backend.environmentIrradianceFbo = 0U;
  }
  if (backend.environmentIrradianceShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.environmentIrradianceShaderHandle);
    backend.environmentIrradianceShaderHandle = ShaderProgramHandle{};
  }
  backend.environmentIrradianceProgram = 0U;
  backend.environmentIrradianceAvailable = false;
  backend.irradianceEnvironmentSource = 0U;
  backend.irradianceEnvironmentFaceSize = 0;
}

/// Handles ensure irradiance environment.
std::uint32_t
ensure_irradiance_environment(BackendState &backend, const RenderDevice *dev,
                              std::uint32_t sourceCubemap,
                              ReflectionProbeBakeSettings settings) noexcept {
  if ((dev == nullptr) || !backend.environmentIrradianceAvailable ||
      !core::cvar_get_bool("r_env_irradiance", true) || (sourceCubemap == 0U) ||
      (dev->create_cubemap_hdr_empty == nullptr) ||
      (dev->framebuffer_cubemap_color_face_mip == nullptr) ||
      (dev->bind_texture_cubemap == nullptr)) {
    return 0U;
  }

  settings = normalize_reflection_probe_bake_settings(settings);
  const int faceSize = static_cast<int>(settings.irradianceFaceSize);

  if ((backend.irradianceEnvironmentTexture != 0U) &&
      (backend.irradianceEnvironmentSource == sourceCubemap) &&
      (backend.irradianceEnvironmentFaceSize == faceSize)) {
    return backend.irradianceEnvironmentTexture;
  }

  if (backend.irradianceEnvironmentTexture != 0U) {
    dev->destroy_texture(backend.irradianceEnvironmentTexture);
    backend.irradianceEnvironmentTexture = 0U;
  }

  if (backend.environmentIrradianceFbo == 0U) {
    backend.environmentIrradianceFbo = dev->create_framebuffer(0U, 0U);
    if (backend.environmentIrradianceFbo == 0U) {
      return 0U;
    }
  }

  const std::uint32_t irradiance = dev->create_cubemap_hdr_empty(faceSize, 1);
  if (irradiance == 0U) {
    return 0U;
  }

  std::array<math::Mat4, 6> views{};
  cubemap_capture_views(views);
  const math::Mat4 projection =
      math::perspective(1.57079632679F, 1.0F, 0.1F, 10.0F);

  dev->disable_depth_test();
  dev->disable_face_culling();
  dev->bind_program(backend.environmentIrradianceProgram);
  dev->bind_texture_cubemap(0, sourceCubemap);
  if (backend.environmentIrradianceTextureLoc >= 0) {
    dev->set_uniform_int(backend.environmentIrradianceTextureLoc, 0);
  }
  if (backend.environmentIrradianceProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.environmentIrradianceProjectionLoc,
                          &projection.columns[0].x);
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->set_viewport(0, 0, faceSize, faceSize);
  for (int face = 0; face < 6; ++face) {
    dev->framebuffer_cubemap_color_face_mip(backend.environmentIrradianceFbo,
                                            irradiance, face, 0);
    if (backend.environmentIrradianceViewLoc >= 0) {
      dev->set_uniform_mat4(
          backend.environmentIrradianceViewLoc,
          &views[static_cast<std::size_t>(face)].columns[0].x);
    }
    dev->draw_arrays_triangles(0, kSkyboxVertexCount);
  }

  dev->bind_texture_cubemap(0, 0U);
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);

  backend.irradianceEnvironmentTexture = irradiance;
  backend.irradianceEnvironmentSource = sourceCubemap;
  backend.irradianceEnvironmentFaceSize = faceSize;
  return irradiance;
}

/// Destroys or releases the requested object, handle, or resource for brdf lut resources.
void destroy_brdf_lut_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.brdfLutFbo != 0U) && (dev != nullptr)) {
    dev->destroy_framebuffer(backend.brdfLutFbo);
    backend.brdfLutFbo = 0U;
  }
  if ((backend.brdfLutTexture != 0U) && (dev != nullptr)) {
    dev->destroy_texture(backend.brdfLutTexture);
    backend.brdfLutTexture = 0U;
  }
  if (backend.environmentBrdfLutShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.environmentBrdfLutShaderHandle);
    backend.environmentBrdfLutShaderHandle = ShaderProgramHandle{};
  }
  backend.environmentBrdfLutProgram = 0U;
  backend.environmentBrdfLutAvailable = false;
  backend.brdfLutSize = 0;
}

/// Handles ensure brdf lut.
std::uint32_t ensure_brdf_lut(BackendState &backend, const RenderDevice *dev,
                              ReflectionProbeBakeSettings settings) noexcept {
  if ((dev == nullptr) || !backend.environmentBrdfLutAvailable ||
      !core::cvar_get_bool("r_env_brdf_lut", true) ||
      (dev->create_texture_2d_hdr == nullptr) ||
      (dev->create_framebuffer == nullptr)) {
    return 0U;
  }

  settings = normalize_reflection_probe_bake_settings(settings);
  const int lutSize = static_cast<int>(settings.brdfLutSize);
  if ((backend.brdfLutTexture != 0U) && (backend.brdfLutSize == lutSize)) {
    return backend.brdfLutTexture;
  }

  if (backend.brdfLutFbo != 0U) {
    dev->destroy_framebuffer(backend.brdfLutFbo);
    backend.brdfLutFbo = 0U;
  }
  if (backend.brdfLutTexture != 0U) {
    dev->destroy_texture(backend.brdfLutTexture);
    backend.brdfLutTexture = 0U;
  }

  const std::uint32_t lutTexture =
      dev->create_texture_2d_hdr(lutSize, lutSize, 2, nullptr);
  if (lutTexture == 0U) {
    return 0U;
  }

  const std::uint32_t lutFbo = dev->create_framebuffer(lutTexture, 0U);
  if (lutFbo == 0U) {
    dev->destroy_texture(lutTexture);
    return 0U;
  }

  dev->bind_framebuffer(lutFbo);
  dev->set_viewport(0, 0, lutSize, lutSize);
  dev->disable_depth_test();
  dev->disable_face_culling();
  dev->bind_program(backend.environmentBrdfLutProgram);
  dev->bind_vertex_array(backend.emptyVao);
  dev->draw_arrays_triangles(0, 3);
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  dev->bind_framebuffer(0U);

  backend.brdfLutTexture = lutTexture;
  backend.brdfLutFbo = lutFbo;
  backend.brdfLutSize = lutSize;
  return lutTexture;
}

/// Destroys or releases the requested object, handle, or resource for bloom resources.
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

/// Handles ensure bloom resources.
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

/// Destroys or releases the requested object, handle, or resource for luminance resources.
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

/// Handles ensure luminance resources.
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

/// Handles generate ssao kernel.
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

/// Creates a new object, handle, or resource for ssao noise texture.
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

/// Returns whether can upload instance matrices.
bool can_upload_instance_matrices(const RenderDevice *dev) noexcept {
  return (dev != nullptr) && (dev->vertex_attrib_divisor != nullptr) &&
         (dev->draw_elements_triangles_u32_instanced != nullptr);
}

/// Handles upload instance matrices.
bool upload_instance_matrices(BackendState &backend, const RenderDevice *dev,
                              const GpuMesh &mesh,
                              CommandBufferView commandBufferView,
                              const StaticMeshBatch &batch) noexcept {
  if (!can_upload_instance_matrices(dev) || (mesh.vertexArray == 0U) ||
      (batch.count == 0U) || (commandBufferView.data == nullptr)) {
    return false;
  }

  if (backend.instanceMatrixBuffer == 0U) {
    backend.instanceMatrixBuffer = dev->create_buffer();
    if (backend.instanceMatrixBuffer == 0U) {
      return false;
    }
  }

  backend.instanceAttributes.resize(batch.count);
  for (std::uint32_t i = 0U; i < batch.count; ++i) {
    const std::size_t commandIndex =
        static_cast<std::size_t>(batch.first) + static_cast<std::size_t>(i);
    const DrawCommand &command = commandBufferView.data[commandIndex];
    backend.instanceAttributes[i].model = command.modelMatrix;
    backend.instanceAttributes[i].foliage =
        math::Vec4(command.foliageWindPhase,
                   static_cast<float>(command.foliageLodIndex), 0.0F, 0.0F);
  }

  dev->bind_vertex_array(mesh.vertexArray);
  dev->bind_array_buffer(backend.instanceMatrixBuffer);
  dev->buffer_data_array(
      backend.instanceAttributes.data(),
      static_cast<std::ptrdiff_t>(backend.instanceAttributes.size() *
                                  sizeof(InstanceAttributes)));

  constexpr std::int32_t stride =
      static_cast<std::int32_t>(sizeof(InstanceAttributes));
  for (std::uint32_t column = 0U; column < kInstanceModelAttribCount;
       ++column) {
    const std::uint32_t attrib = kInstanceModelAttrib0 + column;
    const auto offset = reinterpret_cast<const void *>(
        static_cast<std::uintptr_t>(offsetof(InstanceAttributes, model) +
                                    (sizeof(float) * 4U * column)));
    dev->enable_vertex_attrib(attrib);
    dev->vertex_attrib_float(attrib, 4, stride, offset);
    dev->vertex_attrib_divisor(attrib, 1U);
  }

  const auto foliageOffset = reinterpret_cast<const void *>(
      static_cast<std::uintptr_t>(offsetof(InstanceAttributes, foliage)));
  dev->enable_vertex_attrib(kInstanceFoliageAttrib);
  dev->vertex_attrib_float(kInstanceFoliageAttrib, 4, stride, foliageOffset);
  dev->vertex_attrib_divisor(kInstanceFoliageAttrib, 1U);

  return true;
}

} // namespace

/// Flushes queued work to the backing runtime system for renderer.
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
  if ((renderer_context().sceneViewportWidth > 0) && (renderer_context().sceneViewportHeight > 0)) {
    drawableWidth = renderer_context().sceneViewportWidth;
    drawableHeight = renderer_context().sceneViewportHeight;
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
  const ReflectionProbeBakeSettings environmentBakeSettings =
      cvar_reflection_probe_bake_settings();
  const DistanceFogSettings fogSettings = distance_fog_settings_from_cvars();
  const HeightFogSettings heightFogSettings = height_fog_settings_from_cvars();
  static_cast<void>(ensure_brdf_lut(backend, dev, environmentBakeSettings));

  // Check if deferred rendering is enabled.
  const bool useDeferred =
      backend.deferredAvailable && core::cvar_get_bool("r_deferred", true);
  const int gbufferDebugMode = core::cvar_get_int("r_gbuffer_debug", 0);

  // Camera setup (shared by both paths).
  const float aspect =
      static_cast<float>(drawableWidth) / static_cast<float>(drawableHeight);
  const math::Mat4 viewMat = math::look_at(
      renderer_context().activeCamera.position, renderer_context().activeCamera.target, renderer_context().activeCamera.up);
  const float fov = (renderer_context().activeCamera.fovRadians > 0.0F)
                        ? renderer_context().activeCamera.fovRadians
                        : kDefaultFovRadians;
  const float nearP =
      (renderer_context().activeCamera.nearPlane > 0.0F) ? renderer_context().activeCamera.nearPlane : kNearClip;
  const float farP =
      (renderer_context().activeCamera.farPlane > nearP) ? renderer_context().activeCamera.farPlane : kFarClip;
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
  std::size_t opaqueCount = 0U;
  std::size_t totalCount = 0U;

  if ((commandBufferView.data != nullptr) && (commandBufferView.count > 0U)) {
    totalCount = static_cast<std::size_t>(commandBufferView.count);
    for (std::size_t i = 0U; i < totalCount; ++i) {
      if ((commandBufferView.data[i].sortKey.value & kDrawKeyTransparentBit) !=
          0U) {
        break;
      }
      opaqueCount = i + 1U;
    }
  }
  if (backend.staticMeshBatches.size() < opaqueCount) {
    backend.staticMeshBatches.resize(opaqueCount);
  }
  const std::size_t opaqueBatchCount = build_static_mesh_batches(
      commandBufferView, 0U, opaqueCount, backend.staticMeshBatches.data(),
      backend.staticMeshBatches.size());

  // ====================================================================
  // SHADOW MAP PASS (before main scene rendering)
  // ====================================================================
  const bool shadowEnabled = backend.shadowAvailable &&
                             core::cvar_get_bool("r_shadows", true) &&
                             lights.directionalLightCount > 0U;

  CascadeSplits cascadeSplits{};
  bool directionalShadowCacheReused = false;
  if (shadowEnabled && (commandBufferView.data != nullptr) &&
      (opaqueCount > 0U)) {
    const float lambda = core::cvar_get_float("r_shadow_lambda", 0.75F);
    cascadeSplits = compute_cascade_splits(nearP, farP, lambda);

    const math::Vec3 &lightDir = lights.directionalLights[0].direction;
    std::array<math::Mat4, kShadowCascadeCount> lightMatrices{};

    for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
      const int shadowResolution = (backend.shadowState.resolutions[c] > 0)
                                       ? backend.shadowState.resolutions[c]
                                       : shadow_cascade_resolution(c);
      math::Mat4 lightVP = compute_cascade_matrix(
          viewMat, projMat, lightDir, cascadeSplits.distances[c],
          cascadeSplits.distances[c + 1], shadowResolution);
      lightVP = snap_to_texel(lightVP, shadowResolution);

      backend.shadowState.cascades[c].lightViewProjection = lightVP;
      backend.shadowState.cascades[c].splitDistance =
          cascadeSplits.distances[c + 1];
      lightMatrices[c] = lightVP;
    }

    const std::uint64_t cacheKey = directional_shadow_cache_key(
        commandBufferView, opaqueCount, lights.directionalLights[0],
        cascadeSplits, lightMatrices);
    const bool cacheEnabled = core::cvar_get_bool("r_shadow_cache", true);
    directionalShadowCacheReused =
        cacheEnabled && backend.directionalShadowCacheValid &&
        (backend.directionalShadowCacheKey == cacheKey);

    if (directionalShadowCacheReused) {
      if (core::cvar_get_bool("r_shadow_debug", false)) {
        core::log_message(core::LogLevel::Info, "renderer",
                          "reused directional shadow maps");
      }
    } else {
      gpu_profiler_begin_pass(GpuPassId::ShadowMap);

      for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
        const math::Mat4 &lightVP = lightMatrices[c];
        const int shadowResolution = (backend.shadowState.resolutions[c] > 0)
                                         ? backend.shadowState.resolutions[c]
                                         : shadow_cascade_resolution(c);

        dev->bind_framebuffer(backend.shadowState.depthFbos[c]);
        dev->set_viewport(0, 0, shadowResolution, shadowResolution);
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
      backend.directionalShadowCacheKey = cacheKey;
      backend.directionalShadowCacheValid = true;
    }
  } else {
    backend.directionalShadowCacheKey = 0U;
    backend.directionalShadowCacheValid = false;
  }

  // ==== Spot Light Shadow Pass ====
  const bool doSpotShadows =
      backend.spotShadowAvailable && core::cvar_get_bool("r_spot_shadows");
  if (doSpotShadows && (lights.spotLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::SpotShadowMap);

    // Select up to kMaxSpotShadowLights nearest shadow-casting spots.
    for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
      backend.spotShadowState.slots[i].lightIndex = -1;
    }

    std::array<ShadowCandidate, kMaxSpotLights> spotCandidates{};
    std::size_t spotCandidateCount = 0U;
    const math::Vec3 &camPos = renderer_context().activeCamera.position;
    for (std::size_t li = 0U; li < lights.spotLightCount; ++li) {
      if (!lights.spotLights[li].castShadow) {
        continue;
      }
      const math::Vec3 &p = lights.spotLights[li].position;
      const float dx = p.x - camPos.x;
      const float dy = p.y - camPos.y;
      const float dz = p.z - camPos.z;
      spotCandidates[spotCandidateCount++] = {li, dx * dx + dy * dy + dz * dz};
    }
    std::sort(spotCandidates.data(), spotCandidates.data() + spotCandidateCount,
              [](const ShadowCandidate &a, const ShadowCandidate &b) noexcept {
                return a.distSq < b.distSq;
              });
    if ((spotCandidateCount > kMaxSpotShadowLights) &&
        core::cvar_get_bool("r_shadow_debug")) {
      core::log_message(core::LogLevel::Warning, "shadow",
                        "spot shadow casters dropped: only 4 slots available");
    }
    const std::size_t activeSpotShadows =
        std::min(spotCandidateCount, kMaxSpotShadowLights);
    for (std::size_t s = 0U; s < activeSpotShadows; ++s) {
      const std::size_t li = spotCandidates[s].lightIndex;
      auto &slot = backend.spotShadowState.slots[s];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = lights.spotLights[li].radius;
      slot.lightViewProjection = compute_spot_shadow_matrix(
          lights.spotLights[li].position, lights.spotLights[li].direction,
          lights.spotLights[li].outerConeAngle, lights.spotLights[li].radius);
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
    for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
      backend.pointShadowState.slots[i].lightIndex = -1;
    }

    std::array<ShadowCandidate, kMaxPointLights> pointCandidates{};
    std::size_t pointCandidateCount = 0U;
    const math::Vec3 &camPos = renderer_context().activeCamera.position;
    for (std::size_t li = 0U; li < lights.pointLightCount; ++li) {
      if (!lights.pointLights[li].castShadow) {
        continue;
      }
      const math::Vec3 &p = lights.pointLights[li].position;
      const float dx = p.x - camPos.x;
      const float dy = p.y - camPos.y;
      const float dz = p.z - camPos.z;
      pointCandidates[pointCandidateCount++] = {li,
                                                dx * dx + dy * dy + dz * dz};
    }
    std::sort(pointCandidates.data(),
              pointCandidates.data() + pointCandidateCount,
              [](const ShadowCandidate &a, const ShadowCandidate &b) noexcept {
                return a.distSq < b.distSq;
              });
    if ((pointCandidateCount > kMaxPointShadowLights) &&
        core::cvar_get_bool("r_shadow_debug")) {
      core::log_message(core::LogLevel::Warning, "shadow",
                        "point shadow casters dropped: only 4 slots available");
    }
    const std::size_t activePointShadows =
        std::min(pointCandidateCount, kMaxPointShadowLights);
    for (std::size_t s = 0U; s < activePointShadows; ++s) {
      const std::size_t li = pointCandidates[s].lightIndex;
      auto &slot = backend.pointShadowState.slots[s];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = std::max(lights.pointLights[li].radius, 1.0F);
      compute_point_shadow_matrices(lights.pointLights[li].position,
                                    lights.pointLights[li].radius,
                                    slot.faceViewProjections);
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
    bool sceneDepthHasOpaque = false;
    auto ensureSceneDepthHasOpaque = [&]() noexcept -> bool {
      if (sceneDepthHasOpaque) {
        return true;
      }
      if (dev->blit_depth == nullptr) {
        return false;
      }

      const std::uint32_t gbufferFbo =
          pass_resource_framebuffer(passRes.gbufferAlbedo);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->blit_depth(gbufferFbo, sceneFbo, drawableWidth, drawableHeight);
      dev->bind_framebuffer(sceneFbo);
      sceneDepthHasOpaque = true;
      return true;
    };

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
    if (backend.gbufTimeLoc >= 0) {
      dev->set_uniform_float(backend.gbufTimeLoc, timeSeconds);
    }

    auto drawGBufferBatches = [&]() {
      std::uint32_t boundVertexArray = 0U;
      for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
           ++batchIndex) {
        const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
        const DrawCommand &command = commandBufferView.data[batch.first];
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
        upload_gbuffer_foliage_uniforms(backend, dev, command);

        if ((batch.count > 1U) && (mesh->indexCount > 0U) &&
            upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                     batch)) {
          boundVertexArray = mesh->vertexArray;
          if (backend.gbufUseInstancingLoc >= 0) {
            dev->set_uniform_int(backend.gbufUseInstancingLoc, 1);
          }
          ++frameStats.drawCalls;
          frameStats.triangleCount +=
              (mesh->indexCount / 3U) * static_cast<std::uint64_t>(batch.count);
          dev->draw_elements_triangles_u32_instanced(
              static_cast<std::int32_t>(mesh->indexCount),
              static_cast<std::int32_t>(batch.count));
          continue;
        }

        if (backend.gbufUseInstancingLoc >= 0) {
          dev->set_uniform_int(backend.gbufUseInstancingLoc, 0);
        }
        for (std::uint32_t local = 0U; local < batch.count; ++local) {
          const std::size_t commandIndex =
              static_cast<std::size_t>(batch.first) +
              static_cast<std::size_t>(local);
          const DrawCommand &singleCommand = commandBufferView.data[commandIndex];
          upload_gbuffer_foliage_uniforms(backend, dev, singleCommand);
          const math::Mat4 model = compute_model_matrix(singleCommand);
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
      }
    };

    // Opaque geometry only for G-Buffer.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawGBufferBatches();

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
            const auto &lp = lights
                                 .pointLights[static_cast<std::size_t>(
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
                              &renderer_context().activeCamera.position.x);
      }
      if (backend.dlScreenSizeLoc >= 0) {
        const float screenSize[2] = {static_cast<float>(drawableWidth),
                                     static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.dlScreenSizeLoc, screenSize);
      }
      upload_deferred_distance_fog_uniforms(backend, dev, fogSettings);
      upload_deferred_height_fog_uniforms(backend, dev, heightFogSettings);

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

    const SkyModel skyModel = selected_sky_model();
    const std::uint32_t skyboxTexture = (skyModel == SkyModel::Cubemap)
                                            ? active_skybox_gpu_texture(backend)
                                            : 0U;
    if (skyboxTexture != 0U) {
      static_cast<void>(
          ensure_prefiltered_environment(backend, dev, skyboxTexture,
                                         environmentBakeSettings));
      static_cast<void>(
          ensure_irradiance_environment(backend, dev, skyboxTexture,
                                        environmentBakeSettings));
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_skybox(backend, dev, viewMat, projMat, skyboxTexture, frameStats);
      }
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_hosek_sky(backend, dev, viewMat, projMat, lights, frameStats);
      }
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_preetham_sky(backend, dev, viewMat, projMat, lights, frameStats);
      }
    }

    // Forward-render transparent geometry into the scene HDR FBO.
    if (opaqueCount < totalCount) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->enable_depth_test();

      // Carry opaque deferred depth into the scene FBO so forward transparent
      // draws depth-test against G-Buffer geometry.
      static_cast<void>(ensureSceneDepthHasOpaque());
      dev->bind_program(backend.pbrProgram);

      // Re-upload forward PBR camera/lights for transparent pass.
      if (backend.pbrTimeLocation >= 0) {
        dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
      }
      if (backend.pbrCameraPosLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                              &renderer_context().activeCamera.position.x);
      }
      if (backend.pbrViewLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrViewLocation, &viewMat.columns[0].x);
      }
      if (backend.pbrViewProjectionLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                              &viewProjection.columns[0].x);
      }
      if (backend.pbrUseInstancingLocation >= 0) {
        dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
      }
      upload_pbr_lighting_uniforms(backend, dev, lights);
      upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
      upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
      bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled,
                               doSpotShadows, doPointShadows);
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
          if (backend.pbrOpacityLocation >= 0)
            dev->set_uniform_float(backend.pbrOpacityLocation,
                                   cmd.material.opacity);
          upload_pbr_foliage_uniforms(backend, dev, cmd);
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
      unbind_pbr_shadow_textures(dev);
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
                            &renderer_context().activeCamera.position.x);
    }
    if (backend.pbrViewLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewLocation, &viewMat.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                            &viewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation >= 0) {
      dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled, doSpotShadows,
                             doPointShadows);

    // Set albedo map sampler to texture unit 0.
    if (backend.pbrAlbedoMapLocation >= 0) {
      dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
    }

    auto drawForwardCommand = [&](const DrawCommand &command,
                                  const GpuMesh &mesh) {
      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);

      if (backend.pbrUseInstancingLocation >= 0) {
        dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
      }
      upload_pbr_foliage_uniforms(backend, dev, command);
      if (backend.pbrModelLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
      }
      dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
      dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

      if (mesh.indexCount > 0U) {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.indexCount / 3U);
        dev->draw_elements_triangles_u32(
            static_cast<std::int32_t>(mesh.indexCount));
      } else {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.vertexCount / 3U);
        dev->draw_arrays_triangles(0,
                                   static_cast<std::int32_t>(mesh.vertexCount));
      }
    };

    auto uploadForwardMaterial = [&](const Material &material,
                                     std::uint32_t *boundAlbedoTexture) {
      if (backend.pbrAlbedoLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrAlbedoLocation, &material.albedo.x);
      }
      if (backend.pbrRoughnessLocation >= 0) {
        dev->set_uniform_float(backend.pbrRoughnessLocation,
                               material.roughness);
      }
      if (backend.pbrMetallicLocation >= 0) {
        dev->set_uniform_float(backend.pbrMetallicLocation, material.metallic);
      }
      if (backend.pbrOpacityLocation >= 0) {
        dev->set_uniform_float(backend.pbrOpacityLocation, material.opacity);
      }

      const std::uint32_t albedoGpuId = texture_gpu_id(material.albedoTexture);
      const bool hasAlbedoTex =
          (material.albedoTexture != kInvalidTextureHandle) &&
          (albedoGpuId != 0U);
      if (backend.pbrHasAlbedoTextureLocation >= 0) {
        dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                             hasAlbedoTex ? 1 : 0);
      }
      if (hasAlbedoTex && (albedoGpuId != *boundAlbedoTexture)) {
        dev->bind_texture(0, albedoGpuId);
        *boundAlbedoTexture = albedoGpuId;
      } else if (!hasAlbedoTex && (*boundAlbedoTexture != 0U)) {
        dev->bind_texture(0, 0U);
        *boundAlbedoTexture = 0U;
      }
    };

    auto drawRange = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTexture = 0U;

      if ((start == 0U) && (end == opaqueCount)) {
        for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
             ++batchIndex) {
          const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
          const DrawCommand &command = commandBufferView.data[batch.first];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U)) {
            continue;
          }

          if (mesh->vertexArray != boundVertexArray) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVertexArray = mesh->vertexArray;
          }

          uploadForwardMaterial(command.material, &boundAlbedoTexture);
          upload_pbr_foliage_uniforms(backend, dev, command);

          if ((batch.count > 1U) && (mesh->indexCount > 0U) &&
              upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                       batch)) {
            boundVertexArray = mesh->vertexArray;
            if (backend.pbrUseInstancingLocation >= 0) {
              dev->set_uniform_int(backend.pbrUseInstancingLocation, 1);
            }
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U) *
                                        static_cast<std::uint64_t>(batch.count);
            dev->draw_elements_triangles_u32_instanced(
                static_cast<std::int32_t>(mesh->indexCount),
                static_cast<std::int32_t>(batch.count));
            continue;
          }

          for (std::uint32_t local = 0U; local < batch.count; ++local) {
            const std::size_t commandIndex =
                static_cast<std::size_t>(batch.first) +
                static_cast<std::size_t>(local);
            drawForwardCommand(commandBufferView.data[commandIndex], *mesh);
          }
        }
        return;
      }

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

        uploadForwardMaterial(command.material, &boundAlbedoTexture);
        drawForwardCommand(command, *mesh);
      }
    };

    // Pass 1: Opaque.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawRange(0U, opaqueCount);

    const SkyModel skyModel = selected_sky_model();
    const std::uint32_t skyboxTexture = (skyModel == SkyModel::Cubemap)
                                            ? active_skybox_gpu_texture(backend)
                                            : 0U;
    if (skyboxTexture != 0U) {
      static_cast<void>(
          ensure_prefiltered_environment(backend, dev, skyboxTexture,
                                         environmentBakeSettings));
      static_cast<void>(
          ensure_irradiance_environment(backend, dev, skyboxTexture,
                                        environmentBakeSettings));
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      draw_skybox(backend, dev, viewMat, projMat, skyboxTexture, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      draw_hosek_sky(backend, dev, viewMat, projMat, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      draw_preetham_sky(backend, dev, viewMat, projMat, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    }

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
    unbind_pbr_shadow_textures(dev);
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
      core::cvar_get_bool("r_auto_exposure", true);
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
  renderer_context().fxaaAppliedThisFrame = false;
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

    renderer_context().fxaaAppliedThisFrame = true;
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
  frameStats.gpuShadowMapMs = directionalShadowCacheReused
                                  ? 0.0F
                                  : gpu_profiler_pass_ms(GpuPassId::ShadowMap);
  frameStats.gpuSpotShadowMs = gpu_profiler_pass_ms(GpuPassId::SpotShadowMap);
  frameStats.gpuPointShadowMs = gpu_profiler_pass_ms(GpuPassId::PointShadowMap);
  frameStats.gpuAutoExposureMs = gpu_profiler_pass_ms(GpuPassId::AutoExposure);
  renderer_context().lastFrameStats = frameStats;
}

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

/// Returns the requested value for skybox texture.
TextureHandle get_skybox_texture() noexcept {
  return renderer_context().activeSkyboxTexture;
}

/// Returns the requested value for active camera.
CameraState get_active_camera() noexcept {
  return renderer_context().activeCamera;
}

/// Returns the requested value for scene viewport texture.
std::uint32_t get_scene_viewport_texture() noexcept {
  const PassResources &passRes = get_pass_resources();
  if (renderer_context().fxaaAppliedThisFrame) {
    return pass_resource_gpu_texture(passRes.sceneColor);
  }
  return pass_resource_gpu_texture(passRes.finalColor);
}

/// Returns the requested value for prefiltered environment texture.
std::uint32_t get_prefiltered_environment_texture() noexcept {
  if ((selected_sky_model() != SkyModel::Cubemap) ||
      !core::cvar_get_bool("r_env_prefilter", true)) {
    return 0U;
  }
  return backend_state().prefilteredEnvironmentTexture;
}

/// Returns the requested value for irradiance environment texture.
std::uint32_t get_irradiance_environment_texture() noexcept {
  if ((selected_sky_model() != SkyModel::Cubemap) ||
      !core::cvar_get_bool("r_env_irradiance", true)) {
    return 0U;
  }
  return backend_state().irradianceEnvironmentTexture;
}

/// Returns the requested value for brdf lut texture.
std::uint32_t get_brdf_lut_texture() noexcept {
  if (!core::cvar_get_bool("r_env_brdf_lut", true)) {
    return 0U;
  }
  return backend_state().brdfLutTexture;
}

/// Handles bake reflection probe.
ReflectionProbeBakeResult
bake_reflection_probe(const ReflectionProbeBakeRequest &request) noexcept {
  ReflectionProbeBakeResult result{};
  result.settings = normalize_reflection_probe_bake_settings(request.settings);

  if ((request.sourceCubemap == kInvalidTextureHandle) &&
      (renderer_context().activeSkyboxTexture == kInvalidTextureHandle)) {
    return result;
  }

  if (!initialize_backend()) {
    return result;
  }

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return result;
  }

  const std::uint32_t sourceCubemap =
      (request.sourceCubemap == kInvalidTextureHandle)
          ? active_skybox_gpu_texture(backend)
          : texture_gpu_id(request.sourceCubemap);
  result.sourceCubemapTexture = sourceCubemap;
  if (sourceCubemap == 0U) {
    return result;
  }

  result.prefilteredEnvironmentTexture = ensure_prefiltered_environment(
      backend, dev, sourceCubemap, result.settings);
  result.irradianceEnvironmentTexture = ensure_irradiance_environment(
      backend, dev, sourceCubemap, result.settings);
  result.brdfLutTexture = ensure_brdf_lut(backend, dev, result.settings);
  result.baked = (result.prefilteredEnvironmentTexture != 0U) &&
                 (result.irradianceEnvironmentTexture != 0U) &&
                 (result.brdfLutTexture != 0U);
  return result;
}

/// Handles renderer get last frame stats.
RendererFrameStats renderer_get_last_frame_stats() noexcept {
  return renderer_context().lastFrameStats;
}

} // namespace engine::renderer
