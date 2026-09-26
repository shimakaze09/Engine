// Implements the shadow-caster draw shared by the directional, spot and
// point shadow passes.

#include "shadow_caster_draw.h"

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"

#include "engine/renderer/command_buffer.h"
#include "engine/renderer/material.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

namespace {

/// Texture unit the MASKED shadow stages sample the opacity mask from.
constexpr std::uint32_t kOpacityMaskSlot = 0U;

/// The opacity mask a caster discards against, or invalid when it casts a
/// full silhouette: anything but mask mode, and mask mode with no mask
/// texture, which the lit passes never cut either.
DeviceTextureHandle caster_mask_texture(const Material &material) noexcept {
  if ((material.alphaMode != AlphaMode::Mask) ||
      (material.opacityTexture == kInvalidTextureHandle)) {
    return kInvalidDeviceTexture;
  }
  return texture_device_handle(material.opacityTexture);
}

/// Uploads the mask, cutoff and UV transform a bound MASKED program tests.
void upload_mask(const RenderDevice *dev, const MaskedShadowProgram &program,
                 const Material &material, DeviceTextureHandle mask) noexcept {
  dev->set_param_i32(program.opacityMaskLoc,
                     static_cast<std::int32_t>(kOpacityMaskSlot));
  dev->bind_texture_slot(kOpacityMaskSlot, mask);
  dev->set_param_f32(program.alphaCutoffLoc, material.alphaCutoff);
  dev->set_param_vec2(program.uvTilingLoc, &material.uvTiling.x);
  dev->set_param_vec2(program.uvOffsetLoc, &material.uvOffset.x);
}

/// Issues the caster's draw call; returns the triangles drawn.
std::uint32_t issue_draw(const RenderDevice *dev,
                         const GpuMesh &mesh) noexcept {
  if (mesh.indexCount > 0U) {
    dev->draw_indexed(mesh.geometry,
                      static_cast<std::int32_t>(mesh.indexCount));
    return mesh.indexCount / 3U;
  }
  dev->draw(mesh.geometry, PrimitiveTopology::Triangles, 0,
            static_cast<std::int32_t>(mesh.vertexCount));
  return mesh.vertexCount / 3U;
}

/// Point pass: the world matrix and the face view-projection go up apart.
std::uint32_t draw_point_caster(BackendState &backend, const RenderDevice *dev,
                                const GpuMesh &mesh, const DrawCommand &command,
                                const ShadowCasterPass &pass) noexcept {
  const DeviceTextureHandle mask = caster_mask_texture(command.material);
  const MaskedShadowProgram &masked = backend.shadowPointMasked;
  if ((mask != kInvalidDeviceTexture) &&
      (masked.program != kInvalidDeviceProgram)) {
    dev->bind_program(masked.program);
    dev->set_param_mat4(masked.lightMvpLoc, &pass.viewProjection.columns[0].x);
    dev->set_param_mat4(masked.modelLoc, &command.modelMatrix.columns[0].x);
    dev->set_param_vec3(masked.lightPosLoc, &pass.lightPosition.x);
    dev->set_param_f32(masked.farPlaneLoc, pass.farPlane);
    upload_mask(dev, masked, command.material, mask);
    const std::uint32_t triangles = issue_draw(dev, mesh);
    dev->bind_program(pass.baseProgram);
    return triangles;
  }
  if (backend.shadowPointLightMvpLoc.valid()) {
    dev->set_param_mat4(backend.shadowPointLightMvpLoc,
                        &pass.viewProjection.columns[0].x);
  }
  if (backend.shadowPointModelLoc.valid()) {
    dev->set_param_mat4(backend.shadowPointModelLoc,
                        &command.modelMatrix.columns[0].x);
  }
  return issue_draw(dev, mesh);
}

} // namespace

std::uint32_t draw_shadow_caster(BackendState &backend, const RenderDevice *dev,
                                 const GpuMesh &mesh,
                                 const DrawCommand &command,
                                 const ShadowCasterPass &pass) noexcept {
  if (pass.point) {
    return draw_point_caster(backend, dev, mesh, command, pass);
  }

  const math::Mat4 lightMvp =
      math::mul(pass.viewProjection, command.modelMatrix);
  const DeviceTextureHandle mask = caster_mask_texture(command.material);
  const bool masked = mask != kInvalidDeviceTexture;
  const bool posed =
      mesh.hasSkin && (command.skinPalette != kInvalidSkinPalette);

  // Param tokens resolve against the bound program on both backends, so
  // each program is bound before its palette and uniforms upload (a stale
  // bind once sent the palette into the static program's uniforms). A
  // palette that cannot upload drops to the next program that fits. The
  // pass's base program is bound on entry and on every return.
  bool baseBound = true;
  if (posed && masked &&
      (backend.shadowSkinnedMasked.program != kInvalidDeviceProgram)) {
    const MaskedShadowProgram &program = backend.shadowSkinnedMasked;
    dev->bind_program(program.program);
    baseBound = false;
    if (upload_bone_palette(backend, dev, command.skinPalette,
                            program.bonesParam,
                            &backend.lastShadowMaskedBonePalette)) {
      dev->set_param_mat4(program.lightMvpLoc, &lightMvp.columns[0].x);
      upload_mask(dev, program, command.material, mask);
      const std::uint32_t triangles = issue_draw(dev, mesh);
      dev->bind_program(pass.baseProgram);
      return triangles;
    }
  }
  if (posed && (backend.shadowDepthSkinnedProgram != kInvalidDeviceProgram)) {
    dev->bind_program(backend.shadowDepthSkinnedProgram);
    baseBound = false;
    if (upload_bone_palette(backend, dev, command.skinPalette,
                            backend.shadowSkinnedBonesParam,
                            &backend.lastShadowBonePalette)) {
      if (backend.shadowSkinnedLightMvpLoc.valid()) {
        dev->set_param_mat4(backend.shadowSkinnedLightMvpLoc,
                            &lightMvp.columns[0].x);
      }
      const std::uint32_t triangles = issue_draw(dev, mesh);
      dev->bind_program(pass.baseProgram);
      return triangles;
    }
  }
  if (masked && (backend.shadowMasked.program != kInvalidDeviceProgram)) {
    const MaskedShadowProgram &program = backend.shadowMasked;
    dev->bind_program(program.program);
    dev->set_param_mat4(program.lightMvpLoc, &lightMvp.columns[0].x);
    upload_mask(dev, program, command.material, mask);
    const std::uint32_t triangles = issue_draw(dev, mesh);
    dev->bind_program(pass.baseProgram);
    return triangles;
  }

  if (!baseBound) {
    dev->bind_program(pass.baseProgram);
  }
  if (backend.shadowLightMvpLoc.valid()) {
    dev->set_param_mat4(backend.shadowLightMvpLoc, &lightMvp.columns[0].x);
  }
  if (backend.shadowModelLoc.valid()) {
    dev->set_param_mat4(backend.shadowModelLoc,
                        &command.modelMatrix.columns[0].x);
  }
  return issue_draw(dev, mesh);
}

} // namespace engine::renderer
