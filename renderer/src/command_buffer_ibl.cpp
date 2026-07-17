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

} // namespace

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

} // namespace engine::renderer
