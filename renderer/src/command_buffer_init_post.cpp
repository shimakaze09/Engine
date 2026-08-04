// Implements the soft-fail post and overlay program initialization:
// FXAA, the bloom pyramid programs with their tonemap integration
// uniforms, SSAO with its kernel and noise, debug lines, and the
// auto-exposure luminance chain.
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

// REQUIRED: both uniforms — the input sampler and the texel size the
// edge taps offset by. On failure the cached program id is zeroed
// because the FXAA pass gates on it rather than on an availability flag.
bool resolve_fxaa_program_state(BackendState &backend,
                                const RenderDevice *dev) noexcept {
  backend.fxaaProgram = shader_gpu_program(backend.fxaaShaderHandle);
  const std::uint32_t fxaaProg = backend.fxaaProgram;
  if (fxaaProg == 0U) {
    return false;
  }
  bool ok = true;
  backend.fxaaInputTextureLocation =
      required_location(&ok, dev, fxaaProg, "u_inputTexture");
  backend.fxaaTexelSizeLocation =
      required_location(&ok, dev, fxaaProg, "u_texelSize");
  if (!ok) {
    backend.fxaaProgram = 0U;
  }
  return ok;
}

// REQUIRED per program: every uniform (input sampler plus threshold or
// filter texel size — a dropped upload reads as 0 and breaks the
// pyramid). A failing program's cached id is zeroed because the bloom
// pass gates on all three ids rather than on an availability flag.
bool resolve_bloom_program_state(BackendState &backend,
                                 const RenderDevice *dev) noexcept {
  bool ok = true;
  if (backend.bloomThresholdShaderHandle != kInvalidShaderProgram) {
    backend.bloomThresholdProgram =
        shader_gpu_program(backend.bloomThresholdShaderHandle);
    const std::uint32_t prog = backend.bloomThresholdProgram;
    bool programOk = prog != 0U;
    if (programOk) {
      backend.bloomThreshSceneColorLoc =
          required_location(&programOk, dev, prog, "u_sceneColor");
      backend.bloomThreshThresholdLoc =
          required_location(&programOk, dev, prog, "u_threshold");
    }
    if (!programOk) {
      backend.bloomThresholdProgram = 0U;
      ok = false;
    }
  }
  if (backend.bloomDownsampleShaderHandle != kInvalidShaderProgram) {
    backend.bloomDownsampleProgram =
        shader_gpu_program(backend.bloomDownsampleShaderHandle);
    const std::uint32_t prog = backend.bloomDownsampleProgram;
    bool programOk = prog != 0U;
    if (programOk) {
      backend.bloomDownInputLoc =
          required_location(&programOk, dev, prog, "u_input");
      backend.bloomDownTexelSizeLoc =
          required_location(&programOk, dev, prog, "u_texelSize");
    }
    if (!programOk) {
      backend.bloomDownsampleProgram = 0U;
      ok = false;
    }
  }
  if (backend.bloomUpsampleShaderHandle != kInvalidShaderProgram) {
    backend.bloomUpsampleProgram =
        shader_gpu_program(backend.bloomUpsampleShaderHandle);
    const std::uint32_t prog = backend.bloomUpsampleProgram;
    bool programOk = prog != 0U;
    if (programOk) {
      backend.bloomUpInputLoc =
          required_location(&programOk, dev, prog, "u_input");
      backend.bloomUpTexelSizeLoc =
          required_location(&programOk, dev, prog, "u_texelSize");
    }
    if (!programOk) {
      backend.bloomUpsampleProgram = 0U;
      ok = false;
    }
  }
  return ok;
}

// REQUIRED: every scalar/matrix uniform (radius and bias upload from
// cvars each frame; dropped uploads read as 0 and null the occlusion
// term) and kernel element u_samples[0]; higher kernel elements stay
// OPTIONAL because a driver may legitimately trim the active array.
// Blur: both uniforms REQUIRED.
bool resolve_ssao_program_state(BackendState &backend,
                                const RenderDevice *dev) noexcept {
  bool ok = true;
  if (backend.ssaoShaderHandle != kInvalidShaderProgram) {
    backend.ssaoProgram = shader_gpu_program(backend.ssaoShaderHandle);
    const std::uint32_t prog = backend.ssaoProgram;
    if (prog != 0U) {
      backend.ssaoDepthLoc = required_location(&ok, dev, prog, "u_gBufferDepth");
      backend.ssaoNormalLoc =
          required_location(&ok, dev, prog, "u_gBufferNormal");
      backend.ssaoNoiseLoc = required_location(&ok, dev, prog, "u_noiseTexture");
      backend.ssaoProjectionLoc =
          required_location(&ok, dev, prog, "u_projection");
      backend.ssaoViewLoc = required_location(&ok, dev, prog, "u_view");
      backend.ssaoNoiseScaleLoc =
          required_location(&ok, dev, prog, "u_noiseScale");
      backend.ssaoRadiusLoc = required_location(&ok, dev, prog, "u_radius");
      backend.ssaoBiasLoc = required_location(&ok, dev, prog, "u_bias");
      for (int i = 0; i < 32; ++i) {
        char nm[64] = {};
        std::snprintf(nm, sizeof(nm), "u_samples[%d]", i);
        backend.ssaoSampleLocs[static_cast<std::size_t>(i)] =
            dev->uniform_location(prog, nm);
      }
      if (backend.ssaoSampleLocs[0] < 0) {
        ok = false;
      }
    } else {
      ok = false;
    }
  }
  if (backend.ssaoBlurShaderHandle != kInvalidShaderProgram) {
    backend.ssaoBlurProgram = shader_gpu_program(backend.ssaoBlurShaderHandle);
    const std::uint32_t prog = backend.ssaoBlurProgram;
    if (prog != 0U) {
      backend.ssaoBlurInputLoc =
          required_location(&ok, dev, prog, "u_ssaoInput");
      backend.ssaoBlurTexelSizeLoc =
          required_location(&ok, dev, prog, "u_texelSize");
    } else {
      ok = false;
    }
  }
  return ok;
}

// REQUIRED: uViewProjection — the only uniform; lines cannot project
// without it.
bool resolve_debug_line_program_state(BackendState &backend,
                                      const RenderDevice *dev) noexcept {
  backend.debugLineProgram = shader_gpu_program(backend.debugLineShaderHandle);
  const std::uint32_t prog = backend.debugLineProgram;
  if (prog == 0U) {
    return false;
  }
  bool ok = true;
  backend.debugLineViewProjectionLoc =
      required_location(&ok, dev, prog, "uViewProjection");
  return ok;
}

// REQUIRED: u_sceneColor — the only uniform; the average-luminance
// reduction has no other input.
bool resolve_luminance_program_state(BackendState &backend,
                                     const RenderDevice *dev) noexcept {
  backend.luminanceProgram = shader_gpu_program(backend.luminanceShaderHandle);
  const std::uint32_t prog = backend.luminanceProgram;
  if (prog == 0U) {
    return false;
  }
  bool ok = true;
  backend.lumSceneColorLoc =
      required_location(&ok, dev, prog, "u_sceneColor");
  return ok;
}

void init_backend_post(BackendState &backend,
                       const RenderDevice *dev) noexcept {
  // FXAA shader (soft-fail: AA simply disabled if shader unavailable).
  core::cvar_register_bool("r_fxaa", true, "Enable FXAA anti-aliasing");
  const ShaderProgramHandle fxaaShader = load_configured_shader_program(
      "fullscreen.vert", "fxaa.frag");
  if (fxaaShader != kInvalidShaderProgram) {
    backend.fxaaShaderHandle = fxaaShader;
    if (!resolve_fxaa_program_state(backend, dev)) {
      backend.fxaaShaderHandle = ShaderProgramHandle{};
      destroy_shader_program(fxaaShader);
    }
  } else {
    core::log_message(core::LogLevel::Warning, "renderer",
                      "FXAA shader not available — anti-aliasing disabled");
  }

  // Bloom shaders (soft-fail: bloom simply disabled if shaders unavailable).
  core::cvar_register_bool("r_bloom", true, "Enable bloom");
  core::cvar_register_float("r_bloom_threshold", 1.0F,
                            "Bloom brightness threshold");
  core::cvar_register_float("r_bloom_intensity", 0.3F, "Bloom intensity");
  {
    const ShaderProgramHandle threshShader =
        load_configured_shader_program("fullscreen.vert",
                            "bloom_threshold.frag");
    if (threshShader != kInvalidShaderProgram) {
      backend.bloomThresholdShaderHandle = threshShader;
    }

    const ShaderProgramHandle downShader =
        load_configured_shader_program("fullscreen.vert",
                            "bloom_downsample.frag");
    if (downShader != kInvalidShaderProgram) {
      backend.bloomDownsampleShaderHandle = downShader;
    }

    const ShaderProgramHandle upShader = load_configured_shader_program(
        "fullscreen.vert", "bloom_upsample.frag");
    if (upShader != kInvalidShaderProgram) {
      backend.bloomUpsampleShaderHandle = upShader;
    }

    static_cast<void>(resolve_bloom_program_state(backend, dev));

    if (backend.bloomThresholdProgram == 0U ||
        backend.bloomDownsampleProgram == 0U ||
        backend.bloomUpsampleProgram == 0U) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "bloom shaders not fully available — bloom disabled");
    }
  }

  // SSAO shaders (soft-fail: SSAO simply disabled if shaders unavailable).
  core::cvar_register_bool("r_ssao", true, "Enable SSAO");
  core::cvar_register_float("r_ssao_radius", 0.5F, "SSAO sample radius");
  core::cvar_register_float("r_ssao_bias", 0.025F, "SSAO depth bias");
  {
    const ShaderProgramHandle ssaoShader = load_configured_shader_program(
        "fullscreen.vert", "ssao.frag");
    if (ssaoShader != kInvalidShaderProgram) {
      backend.ssaoShaderHandle = ssaoShader;
    }

    const ShaderProgramHandle ssaoBlurShader = load_configured_shader_program(
        "fullscreen.vert", "ssao_blur.frag");
    if (ssaoBlurShader != kInvalidShaderProgram) {
      backend.ssaoBlurShaderHandle = ssaoBlurShader;
    }

    const bool ssaoUniformsOk = resolve_ssao_program_state(backend, dev);

    if (ssaoUniformsOk && backend.ssaoProgram != 0U &&
        backend.ssaoBlurProgram != 0U) {
      backend.ssaoAvailable = true;
      generate_ssao_kernel(backend.ssaoKernel, 32);
      backend.ssaoNoiseTexture = create_ssao_noise_texture();
      if (backend.ssaoNoiseTexture == 0U) {
        core::log_message(core::LogLevel::Warning, "renderer",
                          "SSAO noise texture creation failed — SSAO disabled");
        backend.ssaoAvailable = false;
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "SSAO shaders not fully available — SSAO disabled");
    }
  }

  // Depth-tested debug line pass (soft-fail: debug overlays simply skipped).
  {
    const ShaderProgramHandle lineShader =
        load_configured_shader_program("debug_line.vert", "debug_line.frag");
    if (lineShader != kInvalidShaderProgram) {
      backend.debugLineShaderHandle = lineShader;
      if (resolve_debug_line_program_state(backend, dev)) {
        backend.debugLineVao = dev->create_vertex_array();
        backend.debugLineVbo = dev->create_buffer();
        if ((backend.debugLineVao != 0U) && (backend.debugLineVbo != 0U)) {
          dev->bind_vertex_array(backend.debugLineVao);
          dev->bind_array_buffer(backend.debugLineVbo);
          constexpr std::int32_t kLineStride =
              static_cast<std::int32_t>(sizeof(float) * 7U);
          dev->enable_vertex_attrib(0U);
          dev->vertex_attrib_float(0U, 3, kLineStride, nullptr);
          dev->enable_vertex_attrib(1U);
          dev->vertex_attrib_float(
              1U, 4, kLineStride,
              reinterpret_cast<const void *>(sizeof(float) * 3U));
          dev->bind_vertex_array(0U);
          dev->bind_array_buffer(0U);
          backend.debugLineAvailable = true;
        } else {
          if (backend.debugLineVao != 0U) {
            dev->destroy_vertex_array(backend.debugLineVao);
            backend.debugLineVao = 0U;
          }
          if (backend.debugLineVbo != 0U) {
            dev->destroy_buffer(backend.debugLineVbo);
            backend.debugLineVbo = 0U;
          }
        }
      } else {
        backend.debugLineShaderHandle = ShaderProgramHandle{};
        destroy_shader_program(lineShader);
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "debug line shader not available — overlays disabled");
    }
  }

  // Auto-exposure luminance shader (soft-fail: uses manual exposure).
  core::cvar_register_bool("r_auto_exposure", true,
                           "Enable automatic exposure adaptation");
  core::cvar_register_float("r_exposure", 1.0F, "Manual exposure value");
  core::cvar_register_float("r_auto_exposure_speed", 1.5F,
                            "Auto-exposure adaptation speed");
  core::cvar_register_float("r_auto_exposure_min", 0.1F,
                            "Minimum auto-exposure value");
  core::cvar_register_float("r_auto_exposure_max", 10.0F,
                            "Maximum auto-exposure value");
  {
    const ShaderProgramHandle lumShader = load_configured_shader_program(
        "fullscreen.vert", "luminance.frag");
    if (lumShader != kInvalidShaderProgram) {
      backend.luminanceShaderHandle = lumShader;
      if (resolve_luminance_program_state(backend, dev)) {
        backend.autoExposureAvailable = true;
      } else {
        backend.luminanceShaderHandle = ShaderProgramHandle{};
        destroy_shader_program(lumShader);
      }
    } else {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "luminance shader not available — "
                        "auto-exposure disabled");
    }
  }

  initialize_post_process_stack();

  static_cast<void>(initialize_gpu_profiler());
}

} // namespace engine::renderer
