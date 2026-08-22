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
  // Instanced sibling (INSTANCED vertex variant): loaded only on
  // backends without a runtime instancing toggle (bgfx); batches bind
  // it instead of setting uUseInstancing. Uniform values carry over
  // through the backend's global-by-name registry.
  ShaderProgramHandle pbrInstancedShaderHandle{};
  DeviceProgramHandle pbrInstancedProgram{};

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
  // Lights (#138 flat array vocabulary shared by both backends: packed
  // vec4 element arrays uploaded through set_param_vec4_array).
  ShaderParam pbrDirLightCountLocation{};
  ShaderParam pbrDirLightDirectionParam{};     // vec4[N]: xyz direction
  ShaderParam pbrDirLightColorParam{};         // vec4[N]: rgb color, w intensity
  ShaderParam pbrPointLightCountLocation{};
  ShaderParam pbrPointLightPosRadiusParam{};   // vec4[N]: xyz pos, w radius
  ShaderParam pbrPointLightColorParam{};       // vec4[N]: rgb color, w intensity
  ShaderParam pbrSpotLightCountLocation{};
  ShaderParam pbrSpotLightPosRadiusParam{};    // vec4[N]: xyz pos, w radius
  ShaderParam pbrSpotLightDirInnerParam{};     // vec4[N]: xyz dir, w innerCone
  ShaderParam pbrSpotLightColorParam{};        // vec4[N]: rgb color, w intensity
  ShaderParam pbrSpotLightParamsParam{};       // vec4[N]: x outerCone

  // PBR forward shadow uniforms (#138: matrices as one mat4 array;
  // cascade splits and shadow-light indices packed into single vec4s;
  // samplers stay per-slot — bgfx has no sampler arrays).
  ShaderParam pbrShadowEnabledLoc{};
  std::array<ShaderParam, kShadowCascadeCount> pbrShadowMapLocs{};
  ShaderParam pbrShadowMatrixParam{};          // mat4[kShadowCascadeCount]
  ShaderParam pbrCascadeSplitsParam{};         // vec4: split per cascade
  ShaderParam pbrSpotShadowEnabledLoc{};
  std::array<ShaderParam, kMaxSpotShadowLights> pbrSpotShadowMapLocs{};
  ShaderParam pbrSpotShadowMatrixParam{};      // mat4[kMaxSpotShadowLights]
  ShaderParam pbrSpotShadowLightIdxParam{};    // vec4: light index per slot
  ShaderParam pbrPointShadowEnabledLoc{};
  std::array<ShaderParam, kMaxPointShadowLights> pbrPointShadowMapLocs{};
  ShaderParam pbrPointShadowPosFarParam{};     // vec4[N]: xyz pos, w far
  ShaderParam pbrPointShadowLightIdxParam{};   // vec4: light index per slot

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

  // Present blit: player mode's final-image draw to the back buffer
  // (r_present_scene, #138) — the editor overlay otherwise carries the
  // scene texture to the swapchain.
  ShaderProgramHandle presentBlitShaderHandle{};
  DeviceProgramHandle presentBlitProgram{};
  ShaderParam presentBlitInputLoc{};

  // Attribute-less geometry for fullscreen triangles.
  DeviceGeometryHandle emptyGeometry{};
  // 1x1 fallbacks bound to disabled sampler slots: Vulkan-family
  // backends require every declared sampler's descriptor valid at draw
  // (2D for the shadow-map slots, cube for point-shadow/IBL slots),
  // where GL merely tolerates unbound units on untaken branches.
  DeviceTextureHandle fallbackTexture2D{};
  DeviceTextureHandle fallbackCubemap{};

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
  // Instanced sibling; same selection rule as pbrInstancedProgram.
  ShaderProgramHandle gbufferInstancedShaderHandle{};
  DeviceProgramHandle gbufferInstancedProgram{};
  // Depth-seed pass (loaded when !caps.depthBlit): copies the G-buffer
  // depth into the scene target with a fullscreen draw before the
  // depth-tested sky pass.
  ShaderProgramHandle depthCopyShaderHandle{};
  DeviceProgramHandle depthCopyProgram{};
  ShaderParam depthCopyDepthLoc{};
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
  ShaderParam ssaoSamplesParam{}; // vec4[32]: hemisphere kernel (xyz)
  ShaderParam ssaoInvProjectionLoc{};

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
  ShaderParam dlShadowMatrixParam{};        // mat4[kShadowCascadeCount]
  ShaderParam dlCascadeSplitsParam{};       // vec4: split per cascade
  std::uint64_t directionalShadowCacheKey = 0U;
  bool directionalShadowCacheValid = false;

  // Spot shadow state.
  SpotShadowState spotShadowState{};
  bool spotShadowAvailable = false;

  ShaderParam dlSpotShadowEnabledLoc{};
  std::array<ShaderParam, kMaxSpotShadowLights> dlSpotShadowMapLocs{};
  ShaderParam dlSpotShadowMatrixParam{};    // mat4[kMaxSpotShadowLights]
  ShaderParam dlSpotShadowLightIdxParam{};  // vec4: light index per slot

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
  ShaderParam dlPointShadowPosFarParam{};   // vec4[N]: xyz pos, w far
  ShaderParam dlPointShadowLightIdxParam{}; // vec4: light index per slot

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
  // #138: palettes upload as plain mat4 arrays (set_param_mat4_array) —
  // per-program uniform state, so each skinned program caches its last
  // palette separately.
  ShaderParam gbufSkinnedBonesParam{};   // mat4[kMaxSkinPaletteJoints]
  ShaderParam shadowSkinnedBonesParam{}; // mat4[kMaxSkinPaletteJoints]
  std::uint32_t lastGbufferBonePalette = 0xFFFFFFFFU;
  std::uint32_t lastShadowBonePalette = 0xFFFFFFFFU;

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
