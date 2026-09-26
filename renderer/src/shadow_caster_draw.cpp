// Implements the shadow-caster draw shared by the directional, spot and
// point shadow passes, and the directional cache key over what it draws.

#include "shadow_caster_draw.h"

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"

#include <cstring>

#include "engine/core/hash.h"
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

/// Appends one integer value to an FNV-1a hash.
std::uint64_t hash_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  return core::fnv1a_64_append_u64(hash, value);
}

/// Appends one finite float value to an FNV-1a hash.
std::uint64_t hash_float(std::uint64_t hash, float value) noexcept {
  std::uint32_t bits = 0U;
  if (value != 0.0F) {
    std::memcpy(&bits, &value, sizeof(bits));
  }
  return hash_u64(hash, bits);
}

/// Appends one vector value to an FNV-1a hash.
std::uint64_t hash_vec3(std::uint64_t hash, const math::Vec3 &value) noexcept {
  hash = hash_float(hash, value.x);
  hash = hash_float(hash, value.y);
  return hash_float(hash, value.z);
}

/// Appends one matrix value to an FNV-1a hash.
std::uint64_t hash_mat4(std::uint64_t hash, const math::Mat4 &value) noexcept {
  for (const math::Vec4 &column : value.columns) {
    hash = hash_float(hash, column.x);
    hash = hash_float(hash, column.y);
    hash = hash_float(hash, column.z);
    hash = hash_float(hash, column.w);
  }
  return hash;
}

/// Appends what draw_shadow_caster resolves for one caster this frame: the
/// geometry its mesh handle finds (zero when it finds none, which draws
/// nothing) and the mask it discards against.
std::uint64_t hash_caster_resolution(std::uint64_t hash,
                                     const GpuMeshRegistry *registry,
                                     const DrawCommand &command) noexcept {
  const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
  hash = hash_u64(hash, (mesh != nullptr) ? mesh->geometry.value : 0U);
  hash = hash_u64(hash, (mesh != nullptr) ? mesh->vertexCount : 0U);
  hash = hash_u64(hash, (mesh != nullptr) ? mesh->indexCount : 0U);
  const DeviceTextureHandle mask = caster_mask_texture(command.material);
  hash = hash_u64(hash, mask.value);
  if (mask != kInvalidDeviceTexture) {
    hash = hash_float(hash, command.material.alphaCutoff);
    hash = hash_float(hash, command.material.uvTiling.x);
    hash = hash_float(hash, command.material.uvTiling.y);
    hash = hash_float(hash, command.material.uvOffset.x);
    hash = hash_float(hash, command.material.uvOffset.y);
  }
  return hash;
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

std::uint64_t directional_shadow_cache_key(
    CommandBufferView commandBufferView, std::size_t opaqueCount,
    const DirectionalLightData &light, const CascadeSplits &splits,
    const std::array<math::Mat4, kShadowCascadeCount> &matrices,
    CommandBufferView auxiliaryView, std::size_t auxiliaryOpaqueCount,
    const GpuMeshRegistry *registry) noexcept {
  std::uint64_t hash = core::kFnv1a64Offset;
  hash = hash_u64(hash, static_cast<std::uint64_t>(opaqueCount));
  hash = hash_u64(hash, static_cast<std::uint64_t>(auxiliaryOpaqueCount));
  hash = hash_vec3(hash, light.direction);
  hash = hash_vec3(hash, light.color);
  hash = hash_float(hash, light.intensity);

  for (std::size_t i = 0U; i <= kShadowCascadeCount; ++i) {
    hash = hash_float(hash, splits.distances[i]);
  }
  for (const math::Mat4 &matrix : matrices) {
    hash = hash_mat4(hash, matrix);
  }

  for (std::size_t i = 0U; i < opaqueCount; ++i) {
    const DrawCommand &command = commandBufferView.data[i];
    hash = hash_u64(hash, command.sortKey.value);
    hash = hash_u64(hash, command.entity);
    hash = hash_u64(hash, command.mesh.id);
    hash = hash_float(hash, command.foliageWindStrength);
    hash = hash_float(hash, command.foliageWindFrequency);
    hash = hash_float(hash, command.foliageWindPhase);
    hash = hash_u64(hash, command.foliageLodIndex);
    hash = hash_mat4(hash, command.modelMatrix);
    hash = hash_caster_resolution(hash, registry, command);
  }
  // Off-screen casters shape the maps just as visible ones do.
  for (std::size_t i = 0U;
       (auxiliaryView.data != nullptr) && (i < auxiliaryOpaqueCount); ++i) {
    const DrawCommand &command = auxiliaryView.data[i];
    if ((command.passMask & kPassShadowCaster) == 0U) {
      continue;
    }
    hash = hash_u64(hash, command.entity);
    hash = hash_u64(hash, command.mesh.id);
    hash = hash_u64(hash, command.foliageLodIndex);
    hash = hash_mat4(hash, command.modelMatrix);
    hash = hash_caster_resolution(hash, registry, command);
  }

  return hash;
}

} // namespace engine::renderer
