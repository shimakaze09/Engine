// Declares the frame-flush pass surface: the per-frame context threaded
// through the pass TUs and the shared uniform-upload helpers every geometry
// pass binds.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "command_buffer_context.h"

namespace engine::renderer {

// Scene clear color shared by the forward path and scene captures.
constexpr float kClearRed = 0.18F;
constexpr float kClearGreen = 0.28F;
constexpr float kClearBlue = 0.60F;

// Environment IBL texture units shared by the deferred and forward passes
// (units 0-17 hold G-buffer/tile/shadow data, 18 the light-data texture).
constexpr int kIblIrradianceUnit = 19;
constexpr int kIblPrefilteredUnit = 20;
constexpr int kIblBrdfLutUnit = 21;

/// Everything flush_renderer computes once per frame and the pass functions
/// share: targets, camera matrices, partition counts, feature toggles, and
/// the accumulated frame stats. Shadow toggles are written by
/// flush_shadow_passes and read by the lighting binds downstream.
struct FrameFlushContext final {
  BackendState &backend;
  const RenderDevice *dev;
  CommandBufferView commandBufferView;
  const GpuMeshRegistry *registry;
  const SceneLightData &lights;
  float timeSeconds;
  const PassResources &passRes;
  int drawableWidth;
  int drawableHeight;
  DistanceFogSettings fogSettings;
  HeightFogSettings heightFogSettings;
  std::uint32_t envSkyboxTexture;
  std::uint32_t iblPrefilteredTex;
  std::uint32_t iblIrradianceTex;
  bool iblAvailable;
  math::Mat4 viewMat;
  math::Mat4 projMat;
  math::Mat4 viewProjection;
  float nearP;
  float farP;
  std::size_t opaqueCount;
  std::size_t totalCount;
  std::size_t opaqueBatchCount;
  int gbufferDebugMode;
  bool shadowEnabled = false;
  bool doSpotShadows = false;
  bool doPointShadows = false;
  bool directionalShadowCacheReused = false;
  RendererFrameStats frameStats{};
};

/// Cascade, spot, and point shadow-map passes; writes the shadow feature
/// toggles and the directional cache-reuse flag into the context.
void flush_shadow_passes(FrameFlushContext &ctx) noexcept;

/// Scene-capture render-to-texture passes (forward-lit, no sky/shadow/post
/// by design; they run before the main passes so they cannot disturb the
/// shadow/tile state computed for the active camera).
void flush_scene_captures(FrameFlushContext &ctx) noexcept;

/// Deferred path: G-Buffer MRT, SSAO with blur, CPU tile light culling and
/// data-texture uploads, G-Buffer debug or deferred lighting, sky, and the
/// forward transparent tail over the deferred depth.
void flush_deferred_path(FrameFlushContext &ctx) noexcept;

/// Forward path: opaque batches, sky, then transparent geometry.
void flush_forward_path(FrameFlushContext &ctx) noexcept;

/// Depth-tested debug line overlay into the scene target, then expires
/// per-frame debug primitives.
void flush_debug_overlay(FrameFlushContext &ctx) noexcept;

/// Post chain: bloom, auto exposure, tonemap into the LDR final target,
/// optional FXAA ping-pong back into sceneColor, and back-buffer prep for
/// the editor overlay.
void flush_post_chain(FrameFlushContext &ctx) noexcept;

/// Reads the distance fog settings from their cvars.
DistanceFogSettings distance_fog_settings_from_cvars() noexcept;

/// Reads the height fog settings from their cvars.
HeightFogSettings height_fog_settings_from_cvars() noexcept;

/// Uploads the environment IBL uniforms for the forward PBR program and
/// binds its textures when enabled; every pbrProgram pass must call this so
/// stale program state never leaks between passes. The sampler units are
/// assigned even when IBL is off: a samplerCube uniform left at its default
/// unit 0 aliases the sampler2D albedo there, which is a draw-time
/// GL_INVALID_OPERATION that corrupts every draw.
void apply_pbr_ibl_uniforms(const BackendState &backend,
                            const RenderDevice *dev, bool enabled) noexcept;

/// Uploads the forward PBR light arrays and counts.
void upload_pbr_lighting_uniforms(const BackendState &backend,
                                  const RenderDevice *dev,
                                  const SceneLightData &lights) noexcept;

/// Uploads distance fog settings to the forward PBR program.
void upload_pbr_distance_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const DistanceFogSettings &settings) noexcept;

/// Uploads height fog settings to the forward PBR program.
void upload_pbr_height_fog_uniforms(const BackendState &backend,
                                    const RenderDevice *dev,
                                    const HeightFogSettings &settings) noexcept;

/// Uploads per-draw foliage wind uniforms to the forward PBR program.
void upload_pbr_foliage_uniforms(const BackendState &backend,
                                 const RenderDevice *dev,
                                 const DrawCommand &command) noexcept;

/// Uploads per-draw foliage wind uniforms to the G-Buffer program.
void upload_gbuffer_foliage_uniforms(const BackendState &backend,
                                     const RenderDevice *dev,
                                     const DrawCommand &command) noexcept;

/// Uploads distance fog settings to the deferred lighting program.
void upload_deferred_distance_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const DistanceFogSettings &settings) noexcept;

/// Uploads height fog settings to the deferred lighting program.
void upload_deferred_height_fog_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const HeightFogSettings &settings) noexcept;

/// Binds the cascade/spot/point shadow maps and matrices for the forward
/// PBR program (or their disabled state when a family is off).
void bind_pbr_shadow_uniforms(const BackendState &backend,
                              const RenderDevice *dev,
                              const SceneLightData &lights, bool shadowEnabled,
                              bool spotShadowsEnabled,
                              bool pointShadowsEnabled) noexcept;

/// Unbinds every shadow texture unit the forward PBR pass bound.
void unbind_pbr_shadow_textures(const RenderDevice *dev) noexcept;

/// Unbinds the IBL texture units the forward PBR pass bound.
void unbind_pbr_ibl_textures(const RenderDevice *dev) noexcept;

/// Uploads a batch's per-instance model/foliage attributes; false when the
/// device or mesh cannot support instanced draws.
bool upload_instance_matrices(BackendState &backend, const RenderDevice *dev,
                              const GpuMesh &mesh,
                              CommandBufferView commandBufferView,
                              const StaticMeshBatch &batch) noexcept;

/// Uploads the frame palette at paletteIndex into the bone-palette uniform
/// buffer; false when skinning is unavailable or the index is out of range
/// (the caller then draws the mesh unskinned in bind pose).
bool upload_bone_palette(const BackendState &backend, const RenderDevice *dev,
                         std::uint32_t paletteIndex) noexcept;

/// Uploads every uniform the bound skinned G-buffer program needs for one
/// draw (camera, material, model) and rebinds unit 0 to the command's
/// albedo texture, keeping the caller's binding cache in sync.
void upload_skinned_gbuffer_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const math::Mat4 &view, const math::Mat4 &projection, float timeSeconds,
    const DrawCommand &command, const math::Mat4 &model,
    const float *normalMatrix,
    std::uint32_t *inOutBoundAlbedoTex) noexcept;

} // namespace engine::renderer
