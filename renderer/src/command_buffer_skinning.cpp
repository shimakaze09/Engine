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

bool upload_bone_palette(const BackendState &backend, const RenderDevice *dev,
                         std::uint32_t paletteIndex) noexcept {
  const RendererContext &context = renderer_context();
  if (!backend.skinningAvailable || (backend.bonePaletteUbo == 0U) ||
      (dev == nullptr) || (dev->bind_uniform_buffer == nullptr) ||
      (dev->buffer_sub_data_uniform == nullptr) ||
      (paletteIndex >= context.skinPaletteCount)) {
    return false;
  }

  const SkinPalette &palette = context.skinPalettes[paletteIndex];
  if (palette.jointCount == 0U) {
    return false;
  }

  dev->bind_uniform_buffer(backend.bonePaletteUbo);
  dev->buffer_sub_data_uniform(
      palette.joints.data(),
      static_cast<std::ptrdiff_t>(
          static_cast<std::size_t>(palette.jointCount) * sizeof(math::Mat4)));
  dev->bind_uniform_buffer(0U);
  return true;
}

void upload_skinned_gbuffer_uniforms(
    const BackendState &backend, const RenderDevice *dev,
    const math::Mat4 &view, const math::Mat4 &projection, float timeSeconds,
    const DrawCommand &command, const math::Mat4 &model,
    const float *normalMatrix,
    std::uint32_t *inOutBoundAlbedoTex) noexcept {
  if (backend.gbufSkinnedViewLoc >= 0) {
    dev->set_uniform_mat4(backend.gbufSkinnedViewLoc, &view.columns[0].x);
  }
  if (backend.gbufSkinnedProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.gbufSkinnedProjectionLoc,
                          &projection.columns[0].x);
  }
  if (backend.gbufSkinnedTimeLoc >= 0) {
    dev->set_uniform_float(backend.gbufSkinnedTimeLoc, timeSeconds);
  }
  if (backend.gbufSkinnedUseInstancingLoc >= 0) {
    dev->set_uniform_int(backend.gbufSkinnedUseInstancingLoc, 0);
  }
  if (backend.gbufSkinnedModelLoc >= 0) {
    dev->set_uniform_mat4(backend.gbufSkinnedModelLoc, &model.columns[0].x);
  }
  if ((backend.gbufSkinnedNormalMatrixLoc >= 0) && (normalMatrix != nullptr)) {
    dev->set_uniform_mat3(backend.gbufSkinnedNormalMatrixLoc, normalMatrix);
  }
  if (backend.gbufSkinnedAlbedoLoc >= 0) {
    dev->set_uniform_vec3(backend.gbufSkinnedAlbedoLoc,
                          &command.material.albedo.x);
  }
  if (backend.gbufSkinnedMetallicLoc >= 0) {
    dev->set_uniform_float(backend.gbufSkinnedMetallicLoc,
                           command.material.metallic);
  }
  if (backend.gbufSkinnedRoughnessLoc >= 0) {
    dev->set_uniform_float(backend.gbufSkinnedRoughnessLoc,
                           command.material.roughness);
  }
  if (backend.gbufSkinnedAOLoc >= 0) {
    dev->set_uniform_float(backend.gbufSkinnedAOLoc, 1.0F);
  }
  if (backend.gbufSkinnedEmissiveLoc >= 0) {
    dev->set_uniform_vec3(backend.gbufSkinnedEmissiveLoc,
                          &command.material.emissive.x);
  }

  const std::uint32_t albedoGpu = texture_gpu_id(command.material.albedoTexture);
  const bool hasAlbedoTex =
      (command.material.albedoTexture != kInvalidTextureHandle) &&
      (albedoGpu != 0U);
  if (backend.gbufSkinnedAlbedoTextureLoc >= 0) {
    dev->set_uniform_int(backend.gbufSkinnedAlbedoTextureLoc, 0);
  }
  if (backend.gbufSkinnedHasAlbedoTextureLoc >= 0) {
    dev->set_uniform_int(backend.gbufSkinnedHasAlbedoTextureLoc,
                         hasAlbedoTex ? 1 : 0);
  }
  if (inOutBoundAlbedoTex != nullptr) {
    const std::uint32_t wanted = hasAlbedoTex ? albedoGpu : 0U;
    if (*inOutBoundAlbedoTex != wanted) {
      dev->bind_texture(0, wanted);
      *inOutBoundAlbedoTex = wanted;
    }
  }
}

} // namespace engine::renderer
