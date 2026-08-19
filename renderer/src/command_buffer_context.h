// Declares private renderer command buffer context state.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/core/nothrow_buffer.h"
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

/// Uniform-buffer binding index shared by every skinned shader variant's
/// BonePalette block.
inline constexpr std::uint32_t kBonePaletteUboBinding = 0U;

/// Stores per-instance attributes uploaded for static mesh instancing.
struct InstanceAttributes final {
  math::Mat4 model = math::Mat4();
  math::Vec4 foliage = math::Vec4(0.0F, 0.0F, 0.0F, 0.0F);
};

/// Device resources backing one scene-capture render target slot.
struct SceneCaptureTarget final {
  DeviceTextureHandle colorTexture{};
  DeviceTextureHandle depthTexture{};
  RenderTargetHandle target{};
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

  // Snapshot of shader_reload_epoch() the cached program ids and uniform
  // locations below were resolved against; a mismatch at flush time
  // triggers refresh_backend_program_state (audit H-09).
  std::uint64_t programCacheEpoch = 0U;

  // Fallback shader (kept for compatibility).
  ShaderProgramHandle defaultShaderHandle{};
  DeviceProgramHandle defaultProgram{};

  // PBR shader.
  ShaderProgramHandle pbrShaderHandle{};
  DeviceProgramHandle pbrProgram{};

  // PBR uniform locations.
  ShaderParam pbrModelLocation{};
  ShaderParam pbrMvpLocation{};
  ShaderParam pbrNormalMatrixLocation{};
  ShaderParam pbrAlbedoLocation{};
  ShaderParam pbrRoughnessLocation{};
  ShaderParam pbrMetallicLocation{};
  ShaderParam pbrTimeLocation{};
  ShaderParam pbrCameraPosLocation{};
  ShaderParam pbrCameraForwardOrthoLocation{};
  ShaderParam pbrIblEnabledLoc{};
  ShaderParam pbrIrradianceMapLoc{};
  ShaderParam pbrPrefilteredMapLoc{};
  ShaderParam pbrBrdfLutLoc{};
  ShaderParam pbrPrefilteredMipsLoc{};
  ShaderParam pbrHasAlbedoTextureLocation{};
  ShaderParam pbrAlbedoMapLocation{};
  ShaderParam pbrOpacityLocation{};
  // issue #160: texture-backed PBR material slots (forward program).
  ShaderParam pbrEmissiveLocation{};
  ShaderParam pbrHasMetallicRoughnessTextureLocation{};
  ShaderParam pbrMetallicRoughnessMapLocation{};
  ShaderParam pbrHasEmissiveTextureLocation{};
  ShaderParam pbrEmissiveMapLocation{};
  ShaderParam pbrHasOcclusionTextureLocation{};
  ShaderParam pbrOcclusionMapLocation{};
  ShaderParam pbrHasOpacityTextureLocation{};
  ShaderParam pbrOpacityMapLocation{};
  ShaderParam pbrAlphaModeLocation{};
  ShaderParam pbrAlphaCutoffLocation{};
  ShaderParam pbrUvTilingLocation{};
  ShaderParam pbrUvOffsetLocation{};
  ShaderParam pbrViewLocation{};
  ShaderParam pbrViewProjectionLocation{};
  ShaderParam pbrUseInstancingLocation{};
  ShaderParam pbrFoliageWindStrengthLocation{};
  ShaderParam pbrFoliageWindFrequencyLocation{};
  ShaderParam pbrFoliagePhaseLocation{};
  ShaderParam pbrFogModeLocation{};
  ShaderParam pbrFogStartLocation{};
  ShaderParam pbrFogEndLocation{};
  ShaderParam pbrFogDensityLocation{};
  ShaderParam pbrFogColorLocation{};
  ShaderParam pbrHeightFogEnabledLocation{};
  ShaderParam pbrHeightFogBaseHeightLocation{};
  ShaderParam pbrHeightFogDensityLocation{};
  ShaderParam pbrHeightFogFalloffLocation{};
  ShaderParam pbrHeightFogStepCountLocation{};
  // Directional lights.
  ShaderParam pbrDirLightCountLocation{};
  std::array<ShaderParam, kMaxDirectionalLights> pbrDirLightDir{};
  std::array<ShaderParam, kMaxDirectionalLights> pbrDirLightColor{};
  std::array<ShaderParam, kMaxDirectionalLights> pbrDirLightIntensity{};

  // Point lights.
  ShaderParam pbrPointLightCountLocation{};
  std::array<ShaderParam, kForwardMaxPointLights> pbrPointLightPos{};
  std::array<ShaderParam, kForwardMaxPointLights> pbrPointLightColor{};
  std::array<ShaderParam, kForwardMaxPointLights> pbrPointLightIntensity{};
  std::array<ShaderParam, kForwardMaxPointLights> pbrPointLightRadius{};

  // Spot lights.
  ShaderParam pbrSpotLightCountLocation{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightPos{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightDir{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightColor{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightIntensity{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightRadius{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightInnerCone{};
  std::array<ShaderParam, kForwardMaxSpotLights> pbrSpotLightOuterCone{};

  // PBR forward shadow uniforms.
  ShaderParam pbrShadowEnabledLoc{};
  std::array<ShaderParam, kShadowCascadeCount> pbrShadowMapLocs{};
  std::array<ShaderParam, kShadowCascadeCount> pbrShadowMatrixLocs{};
  std::array<ShaderParam, kShadowCascadeCount> pbrCascadeSplitLocs{};
  ShaderParam pbrSpotShadowEnabledLoc{};
  std::array<ShaderParam, kMaxSpotShadowLights> pbrSpotShadowMapLocs{};
  std::array<ShaderParam, kMaxSpotShadowLights> pbrSpotShadowMatrixLocs{};
  std::array<ShaderParam, kMaxSpotShadowLights> pbrSpotShadowLightIdxLocs{};
  ShaderParam pbrPointShadowEnabledLoc{};
  std::array<ShaderParam, kMaxPointShadowLights> pbrPointShadowMapLocs{};
  std::array<ShaderParam, kMaxPointShadowLights> pbrPointShadowLightPosLocs{};
  std::array<ShaderParam, kMaxPointShadowLights> pbrPointShadowFarPlaneLocs{};
  std::array<ShaderParam, kMaxPointShadowLights> pbrPointShadowLightIdxLocs{};

  // Tonemap shader.
  ShaderProgramHandle tonemapShaderHandle{};
  DeviceProgramHandle tonemapProgram{};
  ShaderParam tonemapSceneColorLocation{};
  ShaderParam tonemapExposureLocation{};
  ShaderParam tonemapOperatorLocation{};

  // FXAA shader.
  ShaderProgramHandle fxaaShaderHandle{};
  DeviceProgramHandle fxaaProgram{};
  ShaderParam fxaaInputTextureLocation{};
  ShaderParam fxaaTexelSizeLocation{};

  // Attribute-less geometry for fullscreen triangles.
  DeviceGeometryHandle emptyGeometry{};

  // Skybox shader and cube geometry.
  bool skyboxAvailable = false;
  ShaderProgramHandle skyboxShaderHandle{};
  DeviceProgramHandle skyboxProgram{};
  ShaderParam skyboxViewLoc{};
  ShaderParam skyboxProjectionLoc{};
  ShaderParam skyboxTextureLoc{};
  DeviceGeometryHandle skyboxGeometry{};
  DeviceBufferHandle skyboxVertexBuffer{};

  bool preethamSkyAvailable = false;
  ShaderProgramHandle preethamSkyShaderHandle{};
  DeviceProgramHandle preethamSkyProgram{};
  ShaderParam preethamSkyViewLoc{};
  ShaderParam preethamSkyProjectionLoc{};
  ShaderParam preethamSkySunDirectionLoc{};
  ShaderParam preethamSkyTurbidityLoc{};

  bool hosekSkyAvailable = false;
  ShaderProgramHandle hosekSkyShaderHandle{};
  DeviceProgramHandle hosekSkyProgram{};
  ShaderParam hosekSkyViewLoc{};
  ShaderParam hosekSkyProjectionLoc{};
  ShaderParam hosekSkySunDirectionLoc{};
  ShaderParam hosekSkyTurbidityLoc{};
  ShaderParam hosekSkyGroundAlbedoLoc{};

  bool environmentPrefilterAvailable = false;
  ShaderProgramHandle environmentPrefilterShaderHandle{};
  DeviceProgramHandle environmentPrefilterProgram{};
  ShaderParam environmentPrefilterViewLoc{};
  ShaderParam environmentPrefilterProjectionLoc{};
  ShaderParam environmentPrefilterTextureLoc{};
  ShaderParam environmentPrefilterRoughnessLoc{};
  DeviceTextureHandle prefilteredEnvironmentTexture{};
  DeviceTextureHandle prefilteredEnvironmentSource{};
  int prefilteredEnvironmentFaceSize = 0;
  int prefilteredEnvironmentMipLevels = 0;

  bool environmentIrradianceAvailable = false;
  ShaderProgramHandle environmentIrradianceShaderHandle{};
  DeviceProgramHandle environmentIrradianceProgram{};
  ShaderParam environmentIrradianceViewLoc{};
  ShaderParam environmentIrradianceProjectionLoc{};
  ShaderParam environmentIrradianceTextureLoc{};
  DeviceTextureHandle irradianceEnvironmentTexture{};
  DeviceTextureHandle irradianceEnvironmentSource{};
  int irradianceEnvironmentFaceSize = 0;

  bool environmentBrdfLutAvailable = false;
  ShaderProgramHandle environmentBrdfLutShaderHandle{};
  DeviceProgramHandle environmentBrdfLutProgram{};
  DeviceTextureHandle brdfLutTexture{};
  int brdfLutSize = 0;

  // Tracked drawable dimensions for pass resource resize.
  int lastWidth = 0;
  int lastHeight = 0;

  // Deferred rendering state.
  bool deferredAvailable = false;

  // G-Buffer shader.
  ShaderProgramHandle gbufferShaderHandle{};
  DeviceProgramHandle gbufferProgram{};
  ShaderParam gbufModelLoc{};
  ShaderParam gbufViewLoc{};
  ShaderParam gbufProjectionLoc{};
  ShaderParam gbufNormalMatrixLoc{};
  ShaderParam gbufUseInstancingLoc{};
  ShaderParam gbufTimeLoc{};
  ShaderParam gbufFoliageWindStrengthLoc{};
  ShaderParam gbufFoliageWindFrequencyLoc{};
  ShaderParam gbufFoliagePhaseLoc{};
  ShaderParam gbufAlbedoLoc{};
  ShaderParam gbufHasAlbedoTextureLoc{};
  ShaderParam gbufAlbedoTextureLoc{};
  ShaderParam gbufMetallicLoc{};
  ShaderParam gbufRoughnessLoc{};
  ShaderParam gbufAOLoc{};
  ShaderParam gbufEmissiveLoc{};
  // issue #160: texture-backed PBR material slots (static G-buffer program).
  ShaderParam gbufHasMetallicRoughnessTextureLoc{};
  ShaderParam gbufMetallicRoughnessTextureLoc{};
  ShaderParam gbufHasEmissiveTextureLoc{};
  ShaderParam gbufEmissiveTextureLoc{};
  ShaderParam gbufHasOcclusionTextureLoc{};
  ShaderParam gbufOcclusionTextureLoc{};
  ShaderParam gbufHasOpacityTextureLoc{};
  ShaderParam gbufOpacityTextureLoc{};
  ShaderParam gbufAlphaModeLoc{};
  ShaderParam gbufAlphaCutoffLoc{};
  ShaderParam gbufUvTilingLoc{};
  ShaderParam gbufUvOffsetLoc{};
  // Deferred lighting shader.
  ShaderProgramHandle deferredLightShaderHandle{};
  DeviceProgramHandle deferredLightProgram{};
  ShaderParam dlGBufAlbedoLoc{};
  ShaderParam dlGBufNormalLoc{};
  ShaderParam dlGBufEmissiveLoc{};
  ShaderParam dlGBufDepthLoc{};
  ShaderParam dlIblEnabledLoc{};
  ShaderParam dlIrradianceMapLoc{};
  ShaderParam dlPrefilteredMapLoc{};
  ShaderParam dlBrdfLutLoc{};
  ShaderParam dlPrefilteredMipsLoc{};
  ShaderParam dlTileLightTexLoc{};
  ShaderParam dlTileCountXLoc{};
  ShaderParam dlTileCountYLoc{};
  ShaderParam dlInvProjectionLoc{};
  ShaderParam dlInvViewLoc{};
  ShaderParam dlDirLightDirLoc{};
  ShaderParam dlDirLightColorLoc{};
  ShaderParam dlCameraPosLoc{};
  ShaderParam dlCameraForwardOrthoLoc{};
  ShaderParam dlScreenSizeLoc{};
  ShaderParam dlFogModeLoc{};
  ShaderParam dlFogStartLoc{};
  ShaderParam dlFogEndLoc{};
  ShaderParam dlFogDensityLoc{};
  ShaderParam dlFogColorLoc{};
  ShaderParam dlHeightFogEnabledLoc{};
  ShaderParam dlHeightFogBaseHeightLoc{};
  ShaderParam dlHeightFogDensityLoc{};
  ShaderParam dlHeightFogFalloffLoc{};
  ShaderParam dlHeightFogStepCountLoc{};
  ShaderParam dlPointLightCountLoc{};
  ShaderParam dlSpotLightCountLoc{};

  // Deferred per-light data texture sampler (replaces per-light uniform
  // arrays, which exceeded the NVIDIA fragment uniform register limit).
  ShaderParam dlLightDataTexLoc{};

  // G-Buffer debug shader.
  ShaderProgramHandle gbufferDebugShaderHandle{};
  DeviceProgramHandle gbufferDebugProgram{};
  ShaderParam dbgGBufAlbedoLoc{};
  ShaderParam dbgGBufNormalLoc{};
  ShaderParam dbgGBufEmissiveLoc{};
  ShaderParam dbgGBufDepthLoc{};
  ShaderParam dbgModeLoc{};

  // Depth-tested debug line pass (positions+colors streamed each frame from
  // the core debug-draw queue).
  bool debugLineAvailable = false;
  ShaderProgramHandle debugLineShaderHandle{};
  DeviceProgramHandle debugLineProgram{};
  ShaderParam debugLineViewProjectionLoc{};
  DeviceGeometryHandle debugLineGeometry{};
  DeviceBufferHandle debugLineVbo{};

  // Tile light texture (uploaded each frame by CPU culling); rows tracks the
  // allocated height so viewport growth recreates it.
  DeviceTextureHandle tileLightTex{};
  int tileLightTexRows = 0;
  // Grow-only nothrow-allocating scratch buffer (audit #204): a failed grow
  // leaves this at zero capacity instead of terminating, and the downstream
  // dataSize < requiredSize check in cull_lights_tiled already degrades
  // gracefully (deferred lighting renders without local lights that frame).
  core::NothrowBuffer<float> tileBuffer;

  // Per-light data texture consumed by the deferred lighting shader
  // (uploaded each frame; fixed layout, see light_culling.h).
  DeviceTextureHandle lightDataTex{};
  std::array<float, kLightDataBufferSize> lightDataBuffer{};
  DeviceBufferHandle instanceMatrixBuffer{};
  // Grow-only nothrow-allocating scratch buffers (audit #204): a failed grow
  // leaves the buffer empty instead of terminating; callers already treat a
  // too-small capacity as a safe degrade (fewer/no batches, non-instanced
  // draw fallback) rather than a correctness requirement on exact sizing.
  core::NothrowBuffer<InstanceAttributes> instanceAttributes;
  core::NothrowBuffer<StaticMeshBatch> staticMeshBatches;

  // Bloom state.
  ShaderProgramHandle bloomThresholdShaderHandle{};
  DeviceProgramHandle bloomThresholdProgram{};
  ShaderParam bloomThreshSceneColorLoc{};
  ShaderParam bloomThreshThresholdLoc{};

  ShaderProgramHandle bloomDownsampleShaderHandle{};
  DeviceProgramHandle bloomDownsampleProgram{};
  ShaderParam bloomDownInputLoc{};
  ShaderParam bloomDownTexelSizeLoc{};

  ShaderProgramHandle bloomUpsampleShaderHandle{};
  DeviceProgramHandle bloomUpsampleProgram{};
  ShaderParam bloomUpInputLoc{};
  ShaderParam bloomUpTexelSizeLoc{};

  // Tonemap bloom integration uniforms.
  ShaderParam tonemapBloomTextureLoc{};
  ShaderParam tonemapBloomIntensityLoc{};
  ShaderParam tonemapBloomEnabledLoc{};

  // Bloom mip chain resources (managed internally).
  static constexpr int kBloomMipLevels = 6;
  DeviceTextureHandle bloomMipTextures[kBloomMipLevels] = {};
  RenderTargetHandle bloomMipTargets[kBloomMipLevels] = {};
  int bloomMipWidths[kBloomMipLevels] = {};
  int bloomMipHeights[kBloomMipLevels] = {};
  int bloomAllocatedWidth = 0;
  int bloomAllocatedHeight = 0;

  // SSAO state.
  bool ssaoAvailable = false;

  ShaderProgramHandle ssaoShaderHandle{};
  DeviceProgramHandle ssaoProgram{};
  ShaderParam ssaoDepthLoc{};
  ShaderParam ssaoNormalLoc{};
  ShaderParam ssaoNoiseLoc{};
  ShaderParam ssaoProjectionLoc{};
  ShaderParam ssaoViewLoc{};
  ShaderParam ssaoNoiseScaleLoc{};
  ShaderParam ssaoRadiusLoc{};
  ShaderParam ssaoBiasLoc{};
  std::array<ShaderParam, 32> ssaoSampleLocs{};

  ShaderProgramHandle ssaoBlurShaderHandle{};
  DeviceProgramHandle ssaoBlurProgram{};
  ShaderParam ssaoBlurInputLoc{};
  ShaderParam ssaoBlurTexelSizeLoc{};

  // Deferred lighting SSAO uniforms.
  ShaderParam dlSsaoTextureLoc{};
  ShaderParam dlSsaoEnabledLoc{};

  // SSAO sampling resources.
  DeviceTextureHandle ssaoNoiseTexture{};
  float ssaoKernel[32 * 3] = {};

  // Shadow map state.
  ShadowMapState shadowState{};
  bool shadowAvailable = false;

  ShaderProgramHandle shadowDepthShaderHandle{};
  DeviceProgramHandle shadowDepthProgram{};
  ShaderParam shadowLightMvpLoc{};
  ShaderParam shadowModelLoc{};

  // Deferred lighting shadow uniforms.
  ShaderParam dlShadowEnabledLoc{};
  std::array<ShaderParam, kShadowCascadeCount> dlShadowMapLocs{};
  std::array<ShaderParam, kShadowCascadeCount> dlShadowMatrixLocs{};
  std::array<ShaderParam, kShadowCascadeCount> dlCascadeSplitLocs{};
  std::uint64_t directionalShadowCacheKey = 0U;
  bool directionalShadowCacheValid = false;

  // Spot shadow state.
  SpotShadowState spotShadowState{};
  bool spotShadowAvailable = false;

  ShaderParam dlSpotShadowEnabledLoc{};
  std::array<ShaderParam, kMaxSpotShadowLights> dlSpotShadowMapLocs{};
  std::array<ShaderParam, kMaxSpotShadowLights> dlSpotShadowMatrixLocs{};
  std::array<ShaderParam, kMaxSpotShadowLights> dlSpotShadowLightIdxLocs{};

  // Point shadow state.
  PointShadowState pointShadowState{};
  bool pointShadowAvailable = false;

  ShaderProgramHandle shadowDepthPointShaderHandle{};
  DeviceProgramHandle shadowDepthPointProgram{};
  ShaderParam shadowPointLightMvpLoc{};
  ShaderParam shadowPointModelLoc{};
  ShaderParam shadowPointLightPosLoc{};
  ShaderParam shadowPointFarPlaneLoc{};

  ShaderParam dlPointShadowEnabledLoc{};
  std::array<ShaderParam, kMaxPointShadowLights> dlPointShadowMapLocs{};
  std::array<ShaderParam, kMaxPointShadowLights> dlPointShadowLightPosLocs{};
  std::array<ShaderParam, kMaxPointShadowLights> dlPointShadowFarPlaneLocs{};
  std::array<ShaderParam, kMaxPointShadowLights> dlPointShadowLightIdxLocs{};

  // Auto-exposure state.
  bool autoExposureAvailable = false;

  ShaderProgramHandle luminanceShaderHandle{};
  DeviceProgramHandle luminanceProgram{};
  ShaderParam lumSceneColorLoc{};

  // Luminance mip chain for averaging.
  static constexpr int kLuminanceMipLevels = 7;
  DeviceTextureHandle lumMipTextures[kLuminanceMipLevels] = {};
  RenderTargetHandle lumMipTargets[kLuminanceMipLevels] = {};
  int lumMipWidths[kLuminanceMipLevels] = {};
  int lumMipHeights[kLuminanceMipLevels] = {};
  int lumAllocatedWidth = 0;
  int lumAllocatedHeight = 0;

  // Temporal adaptation.
  float currentExposure = 1.0F;

  // Scene capture render targets (slot i backs capture request i).
  std::array<SceneCaptureTarget, kMaxSceneCaptures> sceneCaptureTargets{};

  // GPU skinning state: skinned G-buffer and shadow-depth program
  // variants plus the shared bone-palette uniform buffer they sample.
  // lastUploadedBonePalette dedupes uploads within one flush (palette
  // contents are per-frame, so flush start resets it to invalid).
  bool skinningAvailable = false;
  DeviceBufferHandle bonePaletteUbo{};
  std::uint32_t lastUploadedBonePalette = 0xFFFFFFFFU;

  ShaderProgramHandle gbufferSkinnedShaderHandle{};
  DeviceProgramHandle gbufferSkinnedProgram{};
  ShaderParam gbufSkinnedModelLoc{};
  ShaderParam gbufSkinnedViewLoc{};
  ShaderParam gbufSkinnedProjectionLoc{};
  ShaderParam gbufSkinnedNormalMatrixLoc{};
  ShaderParam gbufSkinnedUseInstancingLoc{};
  ShaderParam gbufSkinnedTimeLoc{};
  ShaderParam gbufSkinnedAlbedoLoc{};
  ShaderParam gbufSkinnedHasAlbedoTextureLoc{};
  ShaderParam gbufSkinnedAlbedoTextureLoc{};
  ShaderParam gbufSkinnedMetallicLoc{};
  ShaderParam gbufSkinnedRoughnessLoc{};
  ShaderParam gbufSkinnedAOLoc{};
  ShaderParam gbufSkinnedEmissiveLoc{};
  // issue #160: texture-backed PBR material slots (skinned G-buffer
  // program). Shares gbuffer.frag with the static program, so the uniform
  // names match; only the cached locations differ per linked program.
  ShaderParam gbufSkinnedHasMetallicRoughnessTextureLoc{};
  ShaderParam gbufSkinnedMetallicRoughnessTextureLoc{};
  ShaderParam gbufSkinnedHasEmissiveTextureLoc{};
  ShaderParam gbufSkinnedEmissiveTextureLoc{};
  ShaderParam gbufSkinnedHasOcclusionTextureLoc{};
  ShaderParam gbufSkinnedOcclusionTextureLoc{};
  ShaderParam gbufSkinnedHasOpacityTextureLoc{};
  ShaderParam gbufSkinnedOpacityTextureLoc{};
  ShaderParam gbufSkinnedAlphaModeLoc{};
  ShaderParam gbufSkinnedAlphaCutoffLoc{};
  ShaderParam gbufSkinnedUvTilingLoc{};
  ShaderParam gbufSkinnedUvOffsetLoc{};
  ShaderProgramHandle shadowDepthSkinnedShaderHandle{};
  DeviceProgramHandle shadowDepthSkinnedProgram{};
  ShaderParam shadowSkinnedLightMvpLoc{};
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
  std::array<SkinPalette, kMaxSkinPalettes> skinPalettes{};
  std::size_t skinPaletteCount = 0U;
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
