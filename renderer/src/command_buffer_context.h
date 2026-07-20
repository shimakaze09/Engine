// Declares private renderer command buffer context state.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/math/mat4.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"

namespace engine::renderer {

inline constexpr std::size_t kForwardMaxPointLights = 8U;
inline constexpr std::size_t kForwardMaxSpotLights = 8U;

/// Stores per-instance attributes uploaded for static mesh instancing.
struct InstanceAttributes final {
  math::Mat4 model = math::Mat4();
  math::Vec4 foliage = math::Vec4(0.0F, 0.0F, 0.0F, 0.0F);
};

/// GL objects backing one scene-capture render target slot.
struct SceneCaptureTarget final {
  std::uint32_t colorTexture = 0U;
  std::uint32_t depthTexture = 0U;
  std::uint32_t framebuffer = 0U;
  // Stable external texture-system handle materials can reference; keeps
  // pointing at colorTexture across lazy target (re)creation.
  TextureHandle textureHandle = kInvalidTextureHandle;
  int width = 0;
  int height = 0;
};

/// Owns private GPU backend state for command buffer rendering.
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

  // Deferred rendering state.
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

  // Deferred per-light data texture sampler (replaces per-light uniform
  // arrays, which exceeded the NVIDIA fragment uniform register limit).
  std::int32_t dlLightDataTexLoc = -1;

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

  // Per-light data texture consumed by the deferred lighting shader
  // (uploaded each frame; fixed layout, see light_culling.h).
  std::uint32_t lightDataTex = 0U;
  std::array<float, kLightDataBufferSize> lightDataBuffer{};
  std::uint32_t instanceMatrixBuffer = 0U;
  std::vector<InstanceAttributes> instanceAttributes;
  std::vector<StaticMeshBatch> staticMeshBatches;

  // Bloom state.
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

  // SSAO state.
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

  // SSAO sampling resources.
  std::uint32_t ssaoNoiseTexture = 0U;
  float ssaoKernel[32 * 3] = {};

  // Shadow map state.
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

  // Spot shadow state.
  SpotShadowState spotShadowState{};
  bool spotShadowAvailable = false;

  std::int32_t dlSpotShadowEnabledLoc = -1;
  std::array<std::int32_t, kMaxSpotShadowLights> dlSpotShadowMapLocs{};
  std::array<std::int32_t, kMaxSpotShadowLights> dlSpotShadowMatrixLocs{};
  std::array<std::int32_t, kMaxSpotShadowLights> dlSpotShadowLightIdxLocs{};

  // Point shadow state.
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

  // Auto-exposure state.
  bool autoExposureAvailable = false;

  ShaderProgramHandle luminanceShaderHandle{};
  std::uint32_t luminanceProgram = 0U;
  std::int32_t lumSceneColorLoc = -1;

  // Luminance mip chain for averaging.
  static constexpr int kLuminanceMipLevels = 7;
  std::uint32_t lumMipTextures[kLuminanceMipLevels] = {};
  std::uint32_t lumMipFbos[kLuminanceMipLevels] = {};
  int lumMipWidths[kLuminanceMipLevels] = {};
  int lumMipHeights[kLuminanceMipLevels] = {};
  int lumAllocatedWidth = 0;
  int lumAllocatedHeight = 0;

  // Temporal adaptation.
  float currentExposure = 1.0F;

  // Scene capture render targets (slot i backs capture request i).
  std::array<SceneCaptureTarget, kMaxSceneCaptures> sceneCaptureTargets{};
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
  std::array<SceneCaptureRequest, kMaxSceneCaptures> sceneCaptureRequests{};
  std::size_t sceneCaptureRequestCount = 0U;
  BackendState backend{};
};

/// Returns the default renderer context used by the legacy renderer API.
RendererContext &renderer_context() noexcept;

/// Returns the backend state owned by the default renderer context.
BackendState &backend_state() noexcept;

/// Resets public renderer state that can otherwise leak between runs.
void reset_renderer_public_state() noexcept;

/// Marks backend initialization as failed while clearing partial state.
void reset_backend_on_failure() noexcept;

/// Lazily creates every backend GPU resource (shaders and their uniform
/// locations, sky geometry, SSAO sampling data). Returns immediately once
/// initialized, so the frame flush may call it every frame; returns false
/// after recording failure when the device or a required shader is missing.
bool initialize_backend() noexcept;

} // namespace engine::renderer
