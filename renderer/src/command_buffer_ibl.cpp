// Implements the IBL bake path for the renderer backend: prefiltered
// specular environment, diffuse irradiance convolution, and the split-sum
// BRDF LUT, plus the public reflection-probe bake entry points.
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#include "command_buffer_ibl.h"

#include "command_buffer_context.h"
#include "command_buffer_sky.h"

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
#include "engine/renderer/command_buffer.h"
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

int cubemap_mip_size(int faceSize, int mipLevel) noexcept {
  int size = faceSize;
  for (int mip = 0; mip < mipLevel; ++mip) {
    size = std::max(1, size / 2);
  }
  return size;
}

std::uint32_t positive_cvar_u32(const char *name, int fallback) noexcept {
  const int value = core::cvar_get_int(name, fallback);
  return (value > 0) ? static_cast<std::uint32_t>(value) : 0U;
}

/// Returns the device to the engine's ambient scene state after an
/// offscreen bake: back buffer bound, opaque-scene render state. The
/// frame passes re-apply their own viewport before drawing (audit M-05).
void restore_scene_state(const RenderDevice *dev) noexcept {
  dev->bind_render_target(kBackBufferTarget);
  dev->apply_render_state(RenderState{DepthTest::Less, true,
                                      BlendMode::Disabled, CullMode::Back});
}

/// Cube-face color render target over one mip of a bake cubemap; invalid
/// when the attachment combination is unsupported.
RenderTargetHandle create_face_target(const RenderDevice *dev,
                                      DeviceTextureHandle cubemap, int face,
                                      int mip) noexcept {
  RenderTargetDesc desc{};
  desc.colorCount = 1U;
  desc.colors[0].texture = cubemap;
  desc.colors[0].face = static_cast<CubeFace>(face);
  desc.colors[0].mipLevel = mip;
  return dev->create_render_target(desc);
}

/// Empty RGB16F bake cubemap with an explicit mip count.
DeviceTextureHandle create_bake_cubemap(const RenderDevice *dev, int faceSize,
                                        int mipLevels) noexcept {
  TextureDesc desc{};
  desc.kind = TextureKind::Cube;
  desc.format = TextureFormat::RGB16F;
  desc.width = faceSize;
  desc.mipLevels = mipLevels;
  desc.filter =
      (mipLevels > 1) ? TextureFilter::LinearMipmap : TextureFilter::Linear;
  desc.wrap = TextureWrap::ClampEdge;
  return dev->create_texture(desc);
}

/// True when the device exposes every operation the bake path drives.
bool bake_device_ready(const RenderDevice *dev) noexcept {
  return (dev != nullptr) && (dev->create_texture != nullptr) &&
         (dev->create_render_target != nullptr) &&
         (dev->destroy_render_target != nullptr) &&
         (dev->bind_render_target != nullptr) &&
         (dev->apply_render_state != nullptr) && (dev->draw != nullptr) &&
         (dev->bind_texture_slot != nullptr);
}

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

} // namespace

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

/// Destroys or releases the requested object, handle, or resource for environment prefilter resources.
void destroy_environment_prefilter_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.prefilteredEnvironmentTexture != kInvalidDeviceTexture) &&
      (dev != nullptr) && (dev->destroy_texture != nullptr)) {
    dev->destroy_texture(backend.prefilteredEnvironmentTexture);
  }
  backend.prefilteredEnvironmentTexture = kInvalidDeviceTexture;
  if (backend.environmentPrefilterShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.environmentPrefilterShaderHandle);
    backend.environmentPrefilterShaderHandle = ShaderProgramHandle{};
  }
  backend.environmentPrefilterProgram = kInvalidDeviceProgram;
  backend.environmentPrefilterAvailable = false;
  backend.prefilteredEnvironmentSource = kInvalidDeviceTexture;
  backend.prefilteredEnvironmentFaceSize = 0;
  backend.prefilteredEnvironmentMipLevels = 0;
}

DeviceTextureHandle
ensure_prefiltered_environment(BackendState &backend, const RenderDevice *dev,
                               DeviceTextureHandle sourceCubemap,
                               ReflectionProbeBakeSettings settings) noexcept {
  if (!backend.environmentPrefilterAvailable ||
      !core::cvar_get_bool("r_env_prefilter", true) ||
      (sourceCubemap == kInvalidDeviceTexture) || !bake_device_ready(dev)) {
    return kInvalidDeviceTexture;
  }

  settings = normalize_reflection_probe_bake_settings(settings);
  const int faceSize = static_cast<int>(settings.prefilteredFaceSize);
  const int mipLevels = static_cast<int>(settings.prefilteredMipLevels);

  if ((backend.prefilteredEnvironmentTexture != kInvalidDeviceTexture) &&
      (backend.prefilteredEnvironmentSource == sourceCubemap) &&
      (backend.prefilteredEnvironmentFaceSize == faceSize) &&
      (backend.prefilteredEnvironmentMipLevels == mipLevels)) {
    return backend.prefilteredEnvironmentTexture;
  }

  if (backend.prefilteredEnvironmentTexture != kInvalidDeviceTexture) {
    dev->destroy_texture(backend.prefilteredEnvironmentTexture);
    backend.prefilteredEnvironmentTexture = kInvalidDeviceTexture;
  }

  const DeviceTextureHandle prefiltered =
      create_bake_cubemap(dev, faceSize, mipLevels);
  if (prefiltered == kInvalidDeviceTexture) {
    return kInvalidDeviceTexture;
  }

  std::array<math::Mat4, 6> views{};
  cubemap_capture_views(views);
  const math::Mat4 projection =
      (device_depth_zero_one()
           ? math::perspective_zero_one(1.57079632679F, 1.0F, 0.1F, 10.0F)
           : math::perspective(1.57079632679F, 1.0F, 0.1F, 10.0F));

  dev->apply_render_state(RenderState{DepthTest::Disabled, true,
                                      BlendMode::Disabled, CullMode::None});
  dev->bind_program(backend.environmentPrefilterProgram);
  dev->bind_texture_slot(0U, sourceCubemap);
  if (backend.environmentPrefilterTextureLoc.valid()) {
    dev->set_param_i32(backend.environmentPrefilterTextureLoc, 0);
  }
  if (backend.environmentPrefilterProjectionLoc.valid()) {
    dev->set_param_mat4(backend.environmentPrefilterProjectionLoc,
                        &projection.columns[0].x);
  }

  bool baked = true;
  for (int mip = 0; baked && (mip < mipLevels); ++mip) {
    const int mipSize = cubemap_mip_size(faceSize, mip);
    const float roughness =
        (mipLevels > 1)
            ? static_cast<float>(mip) / static_cast<float>(mipLevels - 1)
            : 0.0F;
    dev->set_viewport(0, 0, mipSize, mipSize);
    if (backend.environmentPrefilterRoughnessLoc.valid()) {
      dev->set_param_f32(backend.environmentPrefilterRoughnessLoc, roughness);
    }

    for (int face = 0; face < 6; ++face) {
      const RenderTargetHandle faceTarget =
          create_face_target(dev, prefiltered, face, mip);
      if (faceTarget.value == 0U) {
        baked = false;
        break;
      }
      dev->bind_render_target(faceTarget);
      if (backend.environmentPrefilterViewLoc.valid()) {
        dev->set_param_mat4(
            backend.environmentPrefilterViewLoc,
            &views[static_cast<std::size_t>(face)].columns[0].x);
      }
      dev->draw(backend.skyboxGeometry, PrimitiveTopology::Triangles, 0,
                kSkyboxVertexCount);
      dev->bind_render_target(kBackBufferTarget);
      dev->destroy_render_target(faceTarget);
    }
  }

  dev->bind_texture_slot(0U, kInvalidDeviceTexture);
  dev->bind_program(kInvalidDeviceProgram);
  restore_scene_state(dev);

  if (!baked) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "environment prefilter render target unavailable; bake "
                      "aborted");
    dev->destroy_texture(prefiltered);
    return kInvalidDeviceTexture;
  }

  backend.prefilteredEnvironmentTexture = prefiltered;
  backend.prefilteredEnvironmentSource = sourceCubemap;
  backend.prefilteredEnvironmentFaceSize = faceSize;
  backend.prefilteredEnvironmentMipLevels = mipLevels;
  return prefiltered;
}

/// Destroys or releases the requested object, handle, or resource for environment irradiance resources.
void destroy_environment_irradiance_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.irradianceEnvironmentTexture != kInvalidDeviceTexture) &&
      (dev != nullptr) && (dev->destroy_texture != nullptr)) {
    dev->destroy_texture(backend.irradianceEnvironmentTexture);
  }
  backend.irradianceEnvironmentTexture = kInvalidDeviceTexture;
  if (backend.environmentIrradianceShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.environmentIrradianceShaderHandle);
    backend.environmentIrradianceShaderHandle = ShaderProgramHandle{};
  }
  backend.environmentIrradianceProgram = kInvalidDeviceProgram;
  backend.environmentIrradianceAvailable = false;
  backend.irradianceEnvironmentSource = kInvalidDeviceTexture;
  backend.irradianceEnvironmentFaceSize = 0;
}

DeviceTextureHandle
ensure_irradiance_environment(BackendState &backend, const RenderDevice *dev,
                              DeviceTextureHandle sourceCubemap,
                              ReflectionProbeBakeSettings settings) noexcept {
  if (!backend.environmentIrradianceAvailable ||
      !core::cvar_get_bool("r_env_irradiance", true) ||
      (sourceCubemap == kInvalidDeviceTexture) || !bake_device_ready(dev)) {
    return kInvalidDeviceTexture;
  }

  settings = normalize_reflection_probe_bake_settings(settings);
  const int faceSize = static_cast<int>(settings.irradianceFaceSize);

  if ((backend.irradianceEnvironmentTexture != kInvalidDeviceTexture) &&
      (backend.irradianceEnvironmentSource == sourceCubemap) &&
      (backend.irradianceEnvironmentFaceSize == faceSize)) {
    return backend.irradianceEnvironmentTexture;
  }

  if (backend.irradianceEnvironmentTexture != kInvalidDeviceTexture) {
    dev->destroy_texture(backend.irradianceEnvironmentTexture);
    backend.irradianceEnvironmentTexture = kInvalidDeviceTexture;
  }

  const DeviceTextureHandle irradiance = create_bake_cubemap(dev, faceSize, 1);
  if (irradiance == kInvalidDeviceTexture) {
    return kInvalidDeviceTexture;
  }

  std::array<math::Mat4, 6> views{};
  cubemap_capture_views(views);
  const math::Mat4 projection =
      (device_depth_zero_one()
           ? math::perspective_zero_one(1.57079632679F, 1.0F, 0.1F, 10.0F)
           : math::perspective(1.57079632679F, 1.0F, 0.1F, 10.0F));

  dev->apply_render_state(RenderState{DepthTest::Disabled, true,
                                      BlendMode::Disabled, CullMode::None});
  dev->bind_program(backend.environmentIrradianceProgram);
  dev->bind_texture_slot(0U, sourceCubemap);
  if (backend.environmentIrradianceTextureLoc.valid()) {
    dev->set_param_i32(backend.environmentIrradianceTextureLoc, 0);
  }
  if (backend.environmentIrradianceProjectionLoc.valid()) {
    dev->set_param_mat4(backend.environmentIrradianceProjectionLoc,
                        &projection.columns[0].x);
  }

  dev->set_viewport(0, 0, faceSize, faceSize);
  bool baked = true;
  for (int face = 0; face < 6; ++face) {
    const RenderTargetHandle faceTarget =
        create_face_target(dev, irradiance, face, 0);
    if (faceTarget.value == 0U) {
      baked = false;
      break;
    }
    dev->bind_render_target(faceTarget);
    if (backend.environmentIrradianceViewLoc.valid()) {
      dev->set_param_mat4(backend.environmentIrradianceViewLoc,
                          &views[static_cast<std::size_t>(face)].columns[0].x);
    }
    dev->draw(backend.skyboxGeometry, PrimitiveTopology::Triangles, 0,
              kSkyboxVertexCount);
    dev->bind_render_target(kBackBufferTarget);
    dev->destroy_render_target(faceTarget);
  }

  dev->bind_texture_slot(0U, kInvalidDeviceTexture);
  dev->bind_program(kInvalidDeviceProgram);
  restore_scene_state(dev);

  if (!baked) {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "environment irradiance render target unavailable; "
                      "bake aborted");
    dev->destroy_texture(irradiance);
    return kInvalidDeviceTexture;
  }

  backend.irradianceEnvironmentTexture = irradiance;
  backend.irradianceEnvironmentSource = sourceCubemap;
  backend.irradianceEnvironmentFaceSize = faceSize;
  return irradiance;
}

/// Destroys or releases the requested object, handle, or resource for brdf lut resources.
void destroy_brdf_lut_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.brdfLutTexture != kInvalidDeviceTexture) && (dev != nullptr) &&
      (dev->destroy_texture != nullptr)) {
    dev->destroy_texture(backend.brdfLutTexture);
  }
  backend.brdfLutTexture = kInvalidDeviceTexture;
  if (backend.environmentBrdfLutShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.environmentBrdfLutShaderHandle);
    backend.environmentBrdfLutShaderHandle = ShaderProgramHandle{};
  }
  backend.environmentBrdfLutProgram = kInvalidDeviceProgram;
  backend.environmentBrdfLutAvailable = false;
  backend.brdfLutSize = 0;
}

DeviceTextureHandle
ensure_brdf_lut(BackendState &backend, const RenderDevice *dev,
                ReflectionProbeBakeSettings settings) noexcept {
  if (!backend.environmentBrdfLutAvailable ||
      !core::cvar_get_bool("r_env_brdf_lut", true) || !bake_device_ready(dev)) {
    return kInvalidDeviceTexture;
  }

  settings = normalize_reflection_probe_bake_settings(settings);
  const int lutSize = static_cast<int>(settings.brdfLutSize);
  if ((backend.brdfLutTexture != kInvalidDeviceTexture) &&
      (backend.brdfLutSize == lutSize)) {
    return backend.brdfLutTexture;
  }

  if (backend.brdfLutTexture != kInvalidDeviceTexture) {
    dev->destroy_texture(backend.brdfLutTexture);
    backend.brdfLutTexture = kInvalidDeviceTexture;
  }

  TextureDesc lutDesc{};
  lutDesc.kind = TextureKind::Tex2D;
  lutDesc.format = TextureFormat::RG16F;
  lutDesc.width = lutSize;
  lutDesc.height = lutSize;
  lutDesc.filter = TextureFilter::Linear;
  lutDesc.wrap = TextureWrap::Repeat;
  const DeviceTextureHandle lutTexture = dev->create_texture(lutDesc);
  if (lutTexture == kInvalidDeviceTexture) {
    return kInvalidDeviceTexture;
  }

  RenderTargetDesc targetDesc{};
  targetDesc.colorCount = 1U;
  targetDesc.colors[0].texture = lutTexture;
  const RenderTargetHandle lutTarget = dev->create_render_target(targetDesc);
  if (lutTarget.value == 0U) {
    dev->destroy_texture(lutTexture);
    return kInvalidDeviceTexture;
  }

  dev->bind_render_target(lutTarget);
  dev->set_viewport(0, 0, lutSize, lutSize);
  dev->apply_render_state(RenderState{DepthTest::Disabled, true,
                                      BlendMode::Disabled, CullMode::None});
  dev->bind_program(backend.environmentBrdfLutProgram);
  dev->draw(backend.emptyGeometry, PrimitiveTopology::Triangles, 0, 3);
  dev->bind_program(kInvalidDeviceProgram);
  restore_scene_state(dev);
  // The LUT render target only exists for this one draw; the texture keeps
  // the baked contents.
  dev->destroy_render_target(lutTarget);

  backend.brdfLutTexture = lutTexture;
  backend.brdfLutSize = lutSize;
  return lutTexture;
}

DeviceTextureHandle get_prefiltered_environment_texture() noexcept {
  if ((selected_sky_model() != SkyModel::Cubemap) ||
      !core::cvar_get_bool("r_env_prefilter", true)) {
    return kInvalidDeviceTexture;
  }
  return backend_state().prefilteredEnvironmentTexture;
}

DeviceTextureHandle get_irradiance_environment_texture() noexcept {
  if ((selected_sky_model() != SkyModel::Cubemap) ||
      !core::cvar_get_bool("r_env_irradiance", true)) {
    return kInvalidDeviceTexture;
  }
  return backend_state().irradianceEnvironmentTexture;
}

DeviceTextureHandle get_brdf_lut_texture() noexcept {
  if (!core::cvar_get_bool("r_env_brdf_lut", true)) {
    return kInvalidDeviceTexture;
  }
  return backend_state().brdfLutTexture;
}

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

  const DeviceTextureHandle sourceCubemap =
      (request.sourceCubemap == kInvalidTextureHandle)
          ? active_skybox_device_texture(backend)
          : texture_device_handle(request.sourceCubemap);
  result.sourceCubemapTexture = sourceCubemap;
  if (sourceCubemap == kInvalidDeviceTexture) {
    return result;
  }

  result.prefilteredEnvironmentTexture = ensure_prefiltered_environment(
      backend, dev, sourceCubemap, result.settings);
  result.irradianceEnvironmentTexture = ensure_irradiance_environment(
      backend, dev, sourceCubemap, result.settings);
  result.brdfLutTexture = ensure_brdf_lut(backend, dev, result.settings);
  result.baked =
      (result.prefilteredEnvironmentTexture != kInvalidDeviceTexture) &&
      (result.irradianceEnvironmentTexture != kInvalidDeviceTexture) &&
      (result.brdfLutTexture != kInvalidDeviceTexture);
  return result;
}

} // namespace engine::renderer
