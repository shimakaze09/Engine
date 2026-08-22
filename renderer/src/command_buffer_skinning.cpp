// Implements the renderer's per-frame skin-palette store: bone palettes
// submitted by render prep, clamped to the fixed slot and joint budgets,
// and consumed by the skinned draw paths during flush_renderer.

#include "engine/renderer/command_buffer.h"

#include <cstddef>

#include "engine/core/logging.h"
#include "engine/renderer/texture_loader.h"
#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"

namespace engine::renderer {

namespace {
constexpr const char *kSkinningLogChannel = "renderer";
} // namespace

void set_skin_palettes(const SkinPalette *palettes,
                       std::size_t count) noexcept {
  RendererContext &context = renderer_context();
  if ((palettes == nullptr) && (count > 0U)) {
    core::log_message(core::LogLevel::Error, kSkinningLogChannel,
                      "skin palette array is null");
    context.skinPaletteCount = 0U;
    return;
  }

  if (count > kMaxSkinPalettes) {
    core::log_message(core::LogLevel::Warning, kSkinningLogChannel,
                      "skin palettes exceed slot count; extra palettes "
                      "dropped");
    count = kMaxSkinPalettes;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    context.skinPalettes[i] = palettes[i];
    if (context.skinPalettes[i].jointCount > kMaxSkinPaletteJoints) {
      context.skinPalettes[i].jointCount =
          static_cast<std::uint32_t>(kMaxSkinPaletteJoints);
    }
  }
  context.skinPaletteCount = count;
}

std::size_t skin_palette_count() noexcept {
  return renderer_context().skinPaletteCount;
}

bool upload_bone_palette(BackendState &backend, const RenderDevice *dev,
                         std::uint32_t paletteIndex, ShaderParam bonesParam,
                         std::uint32_t *lastUploaded) noexcept {
  const RendererContext &context = renderer_context();
  if (!backend.skinningAvailable || !bonesParam.valid() ||
      (dev == nullptr) || (dev->set_param_mat4_array == nullptr) ||
      (lastUploaded == nullptr) ||
      (paletteIndex >= context.skinPaletteCount)) {
    return false;
  }

  const SkinPalette &palette = context.skinPalettes[paletteIndex];
  if (palette.jointCount == 0U) {
    return false;
  }
  // Plain-array palettes are per-program uniform state, so the caller
  // passes its program's cache slot; the set applies to the currently
  // bound (skinned) program.
  if (*lastUploaded == paletteIndex) {
    return true;
  }
  *lastUploaded = paletteIndex;

  dev->set_param_mat4_array(
      bonesParam, &palette.joints[0].columns[0].x,
      static_cast<std::int32_t>(palette.jointCount));
  return true;
}

void upload_skinned_gbuffer_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const math::Mat4 &view, const math::Mat4 &projection, float timeSeconds,
    const DrawCommand &command, const math::Mat4 &model,
    const float *normalMatrix, DeviceTextureHandle *inOutBoundAlbedoTex,
    DeviceTextureHandle inOutBoundMaterialTex[4]) noexcept {
  if (backend.gbufSkinnedViewLoc.valid()) {
    dev->set_param_mat4(backend.gbufSkinnedViewLoc, &view.columns[0].x);
  }
  if (backend.gbufSkinnedProjectionLoc.valid()) {
    dev->set_param_mat4(backend.gbufSkinnedProjectionLoc,
                          &projection.columns[0].x);
  }
  if (backend.gbufSkinnedTimeLoc.valid()) {
    dev->set_param_f32(backend.gbufSkinnedTimeLoc, timeSeconds);
  }
  if (backend.gbufSkinnedUseInstancingLoc.valid()) {
    dev->set_param_i32(backend.gbufSkinnedUseInstancingLoc, 0);
  }
  if (backend.gbufSkinnedModelLoc.valid()) {
    dev->set_param_mat4(backend.gbufSkinnedModelLoc, &model.columns[0].x);
  }
  if ((backend.gbufSkinnedNormalMatrixLoc.valid()) && (normalMatrix != nullptr)) {
    dev->set_param_mat3(backend.gbufSkinnedNormalMatrixLoc, normalMatrix);
  }
  if (backend.gbufSkinnedAlbedoLoc.valid()) {
    dev->set_param_vec3(backend.gbufSkinnedAlbedoLoc,
                          &command.material.albedo.x);
  }
  if (backend.gbufSkinnedMetallicLoc.valid()) {
    dev->set_param_f32(backend.gbufSkinnedMetallicLoc,
                           command.material.metallic);
  }
  if (backend.gbufSkinnedRoughnessLoc.valid()) {
    dev->set_param_f32(backend.gbufSkinnedRoughnessLoc,
                           command.material.roughness);
  }
  if (backend.gbufSkinnedAOLoc.valid()) {
    dev->set_param_f32(backend.gbufSkinnedAOLoc, 1.0F);
  }
  if (backend.gbufSkinnedEmissiveLoc.valid()) {
    dev->set_param_vec3(backend.gbufSkinnedEmissiveLoc,
                          &command.material.emissive.x);
  }

  const DeviceTextureHandle albedoTex =
      texture_device_handle(command.material.albedoTexture);
  const bool hasAlbedoTex =
      (command.material.albedoTexture != kInvalidTextureHandle) &&
      (albedoTex != kInvalidDeviceTexture);
  if (backend.gbufSkinnedAlbedoTextureLoc.valid()) {
    dev->set_param_i32(backend.gbufSkinnedAlbedoTextureLoc, 0);
  }
  if (backend.gbufSkinnedHasAlbedoTextureLoc.valid()) {
    dev->set_param_i32(backend.gbufSkinnedHasAlbedoTextureLoc,
                         hasAlbedoTex ? 1 : 0);
  }
  if (inOutBoundAlbedoTex != nullptr) {
    const DeviceTextureHandle wanted =
        hasAlbedoTex ? albedoTex : kInvalidDeviceTexture;
    if (*inOutBoundAlbedoTex != wanted) {
      dev->bind_texture_slot(0U, wanted);
      *inOutBoundAlbedoTex = wanted;
    }
  }

  if (inOutBoundMaterialTex != nullptr) {
    const MaterialTextureUniformLocs locs{
        backend.gbufSkinnedHasMetallicRoughnessTextureLoc,
        backend.gbufSkinnedMetallicRoughnessTextureLoc,
        backend.gbufSkinnedHasEmissiveTextureLoc,
        backend.gbufSkinnedEmissiveTextureLoc,
        backend.gbufSkinnedHasOcclusionTextureLoc,
        backend.gbufSkinnedOcclusionTextureLoc,
        backend.gbufSkinnedHasOpacityTextureLoc,
        backend.gbufSkinnedOpacityTextureLoc,
        backend.gbufSkinnedAlphaModeLoc,
        backend.gbufSkinnedAlphaCutoffLoc,
        backend.gbufSkinnedUvTilingLoc,
        backend.gbufSkinnedUvOffsetLoc};
    upload_material_texture_slots(locs, dev, command.material,
                                  backend.fallbackTexture2D,
                                  inOutBoundMaterialTex);
  }
}

} // namespace engine::renderer
