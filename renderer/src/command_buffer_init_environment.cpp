// Implements the soft-fail sky and IBL program initialization: cubemap
// skybox, Preetham and procedural scatter skies, environment prefilter
// and irradiance convolution, and the split-sum BRDF LUT.
#include "command_buffer_ibl.h"
#include "command_buffer_math.h"
#include "command_buffer_post_resources.h"
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
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/post_process_stack.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/texture_loader.h"
#include "command_buffer_init_internal.h"

namespace engine::renderer {

// REQUIRED: view/projection and the cubemap sampler — every skybox-pass
// uniform. No OPTIONAL uniforms.
bool resolve_skybox_program_state(BackendState &backend,
                                  const RenderDevice *dev) noexcept {
  backend.skyboxProgram = shader_device_program(backend.skyboxShaderHandle);
  const DeviceProgramHandle skyboxProgram = backend.skyboxProgram;
  if (skyboxProgram == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.skyboxViewLoc = required_param(&ok, dev, skyboxProgram, "u_view");
  backend.skyboxProjectionLoc =
      required_param(&ok, dev, skyboxProgram, "u_projection");
  backend.skyboxTextureLoc =
      required_param(&ok, dev, skyboxProgram, "u_skybox");
  return ok;
}

// REQUIRED: view/projection, sun direction, and turbidity — every
// Preetham uniform. No OPTIONAL uniforms.
bool resolve_preetham_sky_program_state(BackendState &backend,
                                        const RenderDevice *dev) noexcept {
  backend.preethamSkyProgram =
      shader_device_program(backend.preethamSkyShaderHandle);
  const DeviceProgramHandle preethamProgram = backend.preethamSkyProgram;
  if (preethamProgram == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.preethamSkyViewLoc =
      required_param(&ok, dev, preethamProgram, "u_view");
  backend.preethamSkyProjectionLoc =
      required_param(&ok, dev, preethamProgram, "u_projection");
  backend.preethamSkySunDirectionLoc =
      required_param(&ok, dev, preethamProgram, "u_sunDirection");
  backend.preethamSkyTurbidityLoc =
      required_param(&ok, dev, preethamProgram, "u_turbidity");
  return ok;
}

// REQUIRED: view/projection, sun direction, turbidity, and ground albedo
// — every procedural-scatter uniform. No OPTIONAL uniforms.
bool resolve_hosek_sky_program_state(BackendState &backend,
                                     const RenderDevice *dev) noexcept {
  backend.hosekSkyProgram = shader_device_program(backend.hosekSkyShaderHandle);
  const DeviceProgramHandle hosekProgram = backend.hosekSkyProgram;
  if (hosekProgram == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.hosekSkyViewLoc = required_param(&ok, dev, hosekProgram, "u_view");
  backend.hosekSkyProjectionLoc =
      required_param(&ok, dev, hosekProgram, "u_projection");
  backend.hosekSkySunDirectionLoc =
      required_param(&ok, dev, hosekProgram, "u_sunDirection");
  backend.hosekSkyTurbidityLoc =
      required_param(&ok, dev, hosekProgram, "u_turbidity");
  backend.hosekSkyGroundAlbedoLoc =
      required_param(&ok, dev, hosekProgram, "u_groundAlbedo");
  return ok;
}

// REQUIRED: view/projection, the source environment sampler, and the
// per-mip roughness — every prefilter uniform. No OPTIONAL uniforms.
bool resolve_environment_prefilter_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept {
  backend.environmentPrefilterProgram =
      shader_device_program(backend.environmentPrefilterShaderHandle);
  const DeviceProgramHandle prefilterProgram = backend.environmentPrefilterProgram;
  if (prefilterProgram == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.environmentPrefilterViewLoc =
      required_param(&ok, dev, prefilterProgram, "u_view");
  backend.environmentPrefilterProjectionLoc =
      required_param(&ok, dev, prefilterProgram, "u_projection");
  backend.environmentPrefilterTextureLoc =
      required_param(&ok, dev, prefilterProgram, "u_environmentMap");
  backend.environmentPrefilterRoughnessLoc =
      required_param(&ok, dev, prefilterProgram, "u_roughness");
  return ok;
}

// REQUIRED: view/projection and the source environment sampler — every
// irradiance-convolution uniform. No OPTIONAL uniforms.
bool resolve_environment_irradiance_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept {
  backend.environmentIrradianceProgram =
      shader_device_program(backend.environmentIrradianceShaderHandle);
  const DeviceProgramHandle irradianceProgram = backend.environmentIrradianceProgram;
  if (irradianceProgram == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  backend.environmentIrradianceViewLoc =
      required_param(&ok, dev, irradianceProgram, "u_view");
  backend.environmentIrradianceProjectionLoc =
      required_param(&ok, dev, irradianceProgram, "u_projection");
  backend.environmentIrradianceTextureLoc =
      required_param(&ok, dev, irradianceProgram, "u_environmentMap");
  return ok;
}

// The split-sum LUT bake has no uniforms: a nonzero link is the whole
// contract.
bool resolve_environment_brdf_lut_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept {
  static_cast<void>(dev);
  backend.environmentBrdfLutProgram =
      shader_device_program(backend.environmentBrdfLutShaderHandle);
  return backend.environmentBrdfLutProgram != kInvalidDeviceProgram;
}

void init_backend_environment(BackendState &backend,
                              const RenderDevice *dev) noexcept {
  // Skybox shader and cube geometry (soft-fail: clear color remains visible).
  core::cvar_register_string("r_sky_model", "hosek",
                             "Sky model: hosek, preetham, cubemap, or none");
  const ShaderProgramHandle skyboxShader = load_configured_shader_program(
      "skybox.vert", "skybox.frag");
  if (skyboxShader != kInvalidShaderProgram) {
    backend.skyboxShaderHandle = skyboxShader;
    if (resolve_skybox_program_state(backend, dev) &&
        create_skybox_geometry(backend, dev)) {
      backend.skyboxAvailable = true;
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "skybox setup failed — skybox disabled");
      destroy_skybox_resources(backend);
      backend.skyboxProgram = kInvalidDeviceProgram;
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "skybox shader not available — skybox disabled");
  }

  // Preetham procedural sky (soft-fail: cubemap skybox or clear color remains).
  core::cvar_register_float("r_sky_turbidity", 3.0F,
                            "Preetham sky turbidity (1.7 clear, 10 hazy)");
  const ShaderProgramHandle preethamShader = load_configured_shader_program(
      "skybox.vert", "preetham_sky.frag");
  if (preethamShader != kInvalidShaderProgram) {
    backend.preethamSkyShaderHandle = preethamShader;
    if (resolve_preetham_sky_program_state(backend, dev) &&
        create_skybox_geometry(backend, dev)) {
      backend.preethamSkyAvailable = true;
    } else {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "Preetham sky setup failed — procedural sky disabled");
      destroy_preetham_sky_resources(backend);
      backend.preethamSkyProgram = kInvalidDeviceProgram;
    }
  } else {
    core::log_message(
        core::LogLevel::Warning, "renderer",
        "Preetham sky shader not available — procedural sky disabled");
  }

  // Default procedural scatter sky (preferred over Preetham when available;
  // selected by the legacy "hosek" value of r_sky_model).
  core::cvar_register_float("r_sky_ground_albedo", 0.1F,
                            "Procedural sky ground albedo");
  const ShaderProgramHandle hosekShader = load_configured_shader_program(
      "skybox.vert", "procedural_sky.frag");
  if (hosekShader != kInvalidShaderProgram) {
    backend.hosekSkyShaderHandle = hosekShader;
    if (resolve_hosek_sky_program_state(backend, dev) &&
        create_skybox_geometry(backend, dev)) {
      backend.hosekSkyAvailable = true;
    } else {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "Hosek-Wilkie sky setup failed — falling back to Preetham");
      destroy_hosek_sky_resources(backend);
      backend.hosekSkyProgram = kInvalidDeviceProgram;
    }
  } else {
    core::log_message(
        core::LogLevel::Warning, "renderer",
        "Hosek-Wilkie sky shader not available — falling back to Preetham");
  }

  // Specular environment prefilter for cubemap IBL.
  core::cvar_register_bool("r_env_prefilter", true,
                           "Bake prefiltered specular cubemap radiance");
  core::cvar_register_int("r_env_prefilter_size", 128,
                          "Prefiltered environment cubemap face size");
  core::cvar_register_int("r_env_prefilter_mips", 5,
                          "Prefiltered environment cubemap mip levels");
  const ShaderProgramHandle prefilterShader =
      load_configured_shader_program("skybox.vert",
                          "prefilter_environment.frag");
  if (prefilterShader != kInvalidShaderProgram) {
    backend.environmentPrefilterShaderHandle = prefilterShader;
    if (resolve_environment_prefilter_program_state(backend, dev) &&
        create_skybox_geometry(backend, dev)) {
      backend.environmentPrefilterAvailable = true;
    } else {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "environment prefilter setup failed — IBL prefilter disabled");
      destroy_environment_prefilter_resources(backend);
      backend.environmentPrefilterProgram = kInvalidDeviceProgram;
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "environment prefilter shader not available");
  }

  // Diffuse irradiance convolution for cubemap IBL.
  core::cvar_register_bool("r_env_irradiance", true,
                           "Bake diffuse irradiance cubemap");
  core::cvar_register_int("r_env_irradiance_size", 32,
                          "Diffuse irradiance cubemap face size");
  const ShaderProgramHandle irradianceShader =
      load_configured_shader_program("skybox.vert",
                          "irradiance_convolution.frag");
  if (irradianceShader != kInvalidShaderProgram) {
    backend.environmentIrradianceShaderHandle = irradianceShader;
    if (resolve_environment_irradiance_program_state(backend, dev) &&
        create_skybox_geometry(backend, dev)) {
      backend.environmentIrradianceAvailable = true;
    } else {
      core::log_message(
          core::LogLevel::Warning, "renderer",
          "environment irradiance setup failed — IBL irradiance disabled");
      destroy_environment_irradiance_resources(backend);
      backend.environmentIrradianceProgram = kInvalidDeviceProgram;
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "environment irradiance shader not available");
  }

  // Split-sum BRDF LUT for image-based lighting.
  core::cvar_register_bool("r_env_brdf_lut", true,
                           "Bake split-sum BRDF lookup texture");
  core::cvar_register_int("r_env_brdf_lut_size", 512,
                          "Split-sum BRDF LUT resolution");
  const ShaderProgramHandle brdfLutShader = load_configured_shader_program(
      "fullscreen.vert", "brdf_lut.frag");
  if (brdfLutShader != kInvalidShaderProgram) {
    backend.environmentBrdfLutShaderHandle = brdfLutShader;
    if (resolve_environment_brdf_lut_program_state(backend, dev)) {
      backend.environmentBrdfLutAvailable = true;
    } else {
      backend.environmentBrdfLutShaderHandle = ShaderProgramHandle{};
      destroy_shader_program(brdfLutShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "BRDF LUT shader not available");
  }

}

} // namespace engine::renderer
