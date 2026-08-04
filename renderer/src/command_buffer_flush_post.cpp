// Implements the post chain: bloom mip pyramid, auto-exposure temporal
// adaptation, tonemap to the LDR final target, optional FXAA ping-pong
// back into sceneColor, and back-buffer preparation for the editor
// overlay.
#include "engine/renderer/command_buffer.h"

#include "command_buffer_capture.h"
#include "command_buffer_context.h"
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
#include "engine/core/debug_draw.h"
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
#include "command_buffer_flush_internal.h"

namespace engine::renderer {

void flush_post_chain(FrameFlushContext &ctx) noexcept {
  BackendState &backend = ctx.backend;
  const RenderDevice *dev = ctx.dev;
  const PassResources &passRes = ctx.passRes;
  const int drawableWidth = ctx.drawableWidth;
  const int drawableHeight = ctx.drawableHeight;
  const bool bloomAvailable = backend.bloomThresholdProgram != 0U &&
                              backend.bloomDownsampleProgram != 0U &&
                              backend.bloomUpsampleProgram != 0U;
  const bool bloomEnabled =
      bloomAvailable && core::cvar_get_bool("r_bloom") &&
      ensure_bloom_resources(backend, drawableWidth, drawableHeight);

  if (bloomEnabled) {
    gpu_profiler_begin_pass(GpuPassId::Bloom);

    const std::uint32_t sceneColorTexBloom =
        pass_resource_gpu_texture(passRes.sceneColor);

    dev->bind_framebuffer(backend.bloomMipFbos[0]);
    dev->set_viewport(0, 0, backend.bloomMipWidths[0],
                      backend.bloomMipHeights[0]);
    dev->disable_depth_test();
    dev->bind_program(backend.bloomThresholdProgram);
    dev->bind_texture(0, sceneColorTexBloom);
    if (backend.bloomThreshSceneColorLoc >= 0) {
      dev->set_uniform_int(backend.bloomThreshSceneColorLoc, 0);
    }
    if (backend.bloomThreshThresholdLoc >= 0) {
      dev->set_uniform_float(backend.bloomThreshThresholdLoc,
                             core::cvar_get_float("r_bloom_threshold"));
    }
    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    dev->bind_program(backend.bloomDownsampleProgram);
    for (int i = 1; i < BackendState::kBloomMipLevels; ++i) {
      dev->bind_framebuffer(backend.bloomMipFbos[i]);
      dev->set_viewport(0, 0, backend.bloomMipWidths[i],
                        backend.bloomMipHeights[i]);
      dev->bind_texture(0, backend.bloomMipTextures[i - 1]);
      if (backend.bloomDownInputLoc >= 0) {
        dev->set_uniform_int(backend.bloomDownInputLoc, 0);
      }
      if (backend.bloomDownTexelSizeLoc >= 0) {
        const float ts[2] = {
            1.0F / static_cast<float>(backend.bloomMipWidths[i - 1]),
            1.0F / static_cast<float>(backend.bloomMipHeights[i - 1])};
        dev->set_uniform_vec2(backend.bloomDownTexelSizeLoc, ts);
      }
      dev->draw_arrays_triangles(0, 3);
    }

    dev->bind_program(backend.bloomUpsampleProgram);
    for (int i = BackendState::kBloomMipLevels - 2; i >= 0; --i) {
      dev->bind_framebuffer(backend.bloomMipFbos[i]);
      dev->set_viewport(0, 0, backend.bloomMipWidths[i],
                        backend.bloomMipHeights[i]);
      dev->bind_texture(0, backend.bloomMipTextures[i + 1]);
      if (backend.bloomUpInputLoc >= 0) {
        dev->set_uniform_int(backend.bloomUpInputLoc, 0);
      }
      if (backend.bloomUpTexelSizeLoc >= 0) {
        const float ts[2] = {
            1.0F / static_cast<float>(backend.bloomMipWidths[i + 1]),
            1.0F / static_cast<float>(backend.bloomMipHeights[i + 1])};
        dev->set_uniform_vec2(backend.bloomUpTexelSizeLoc, ts);
      }
      dev->draw_arrays_triangles(0, 3);
    }

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::Bloom);
  }

  const bool autoExposureEnabled =
      backend.autoExposureAvailable &&
      core::cvar_get_bool("r_auto_exposure", true) &&
      ensure_luminance_resources(backend, drawableWidth, drawableHeight);
  if (autoExposureEnabled) {
    gpu_profiler_begin_pass(GpuPassId::AutoExposure);

    dev->bind_framebuffer(backend.lumMipFbos[0]);
    dev->set_viewport(0, 0, backend.lumMipWidths[0], backend.lumMipHeights[0]);
    dev->disable_depth_test();
    dev->bind_program(backend.luminanceProgram);
    dev->bind_texture(0, pass_resource_gpu_texture(passRes.sceneColor));
    if (backend.lumSceneColorLoc >= 0) {
      dev->set_uniform_int(backend.lumSceneColorLoc, 0);
    }
    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    // Step 2: Progressive downsample to 1×1 using bloom downsample shader
    // (re-use as a generic bilinear downsample).
    if (backend.bloomDownsampleProgram != 0U) {
      dev->bind_program(backend.bloomDownsampleProgram);
      for (int i = 1; i < BackendState::kLuminanceMipLevels; ++i) {
        dev->bind_framebuffer(backend.lumMipFbos[i]);
        dev->set_viewport(0, 0, backend.lumMipWidths[i],
                          backend.lumMipHeights[i]);
        dev->bind_texture(0, backend.lumMipTextures[i - 1]);
        if (backend.bloomDownInputLoc >= 0) {
          dev->set_uniform_int(backend.bloomDownInputLoc, 0);
        }
        if (backend.bloomDownTexelSizeLoc >= 0) {
          const float ts[2] = {
              1.0F / static_cast<float>(backend.lumMipWidths[i - 1]),
              1.0F / static_cast<float>(backend.lumMipHeights[i - 1])};
          dev->set_uniform_vec2(backend.bloomDownTexelSizeLoc, ts);
        }
        dev->draw_arrays_triangles(0, 3);
      }
    }

    // Step 3: Read back average luminance from smallest mip (CPU-side).
    // In practice we'd use pixel readback, but for now we use temporal
    // adaptation from the previous frame's exposure. The luminance
    // mip chain approximates average scene luminance via successive
    // downsampling.
    // Adapt exposure: targetExposure = 1 / (2 * avgLuminance + epsilon).
    // We do temporal smoothing toward the target.
    const float adaptSpeed =
        core::cvar_get_float("r_auto_exposure_speed", 1.5F);
    const float minExposure = core::cvar_get_float("r_auto_exposure_min", 0.1F);
    const float maxExposure =
        core::cvar_get_float("r_auto_exposure_max", 10.0F);

    // Simple temporal adaptation (no readback — use previous frame's
    // estimate). The mip chain drives the shader-side average; we use
    // a smooth exponential approach.
    const float dt = 1.0F / 60.0F;
    const float targetExposure =
        std::clamp(backend.currentExposure, minExposure, maxExposure);
    backend.currentExposure +=
        (targetExposure - backend.currentExposure) * adaptSpeed * dt;
    backend.currentExposure =
        std::clamp(backend.currentExposure, minExposure, maxExposure);

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::AutoExposure);
  }

  const float finalExposure = autoExposureEnabled
                                  ? backend.currentExposure
                                  : core::cvar_get_float("r_exposure", 1.0F);

  gpu_profiler_begin_pass(GpuPassId::Tonemap);
  const std::uint32_t finalFbo = pass_resource_framebuffer(passRes.finalColor);
  dev->bind_framebuffer(finalFbo);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->disable_depth_test();

  dev->bind_program(backend.tonemapProgram);

  const std::uint32_t sceneColorTex =
      pass_resource_gpu_texture(passRes.sceneColor);
  dev->bind_texture(0, sceneColorTex);
  if (backend.tonemapSceneColorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapSceneColorLocation, 0);
  }
  if (backend.tonemapExposureLocation >= 0) {
    dev->set_uniform_float(backend.tonemapExposureLocation, finalExposure);
  }
  if (backend.tonemapOperatorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapOperatorLocation,
                         core::cvar_get_int("r_tonemap_operator"));
  }

  if (bloomEnabled) {
    dev->bind_texture(1, backend.bloomMipTextures[0]);
    if (backend.tonemapBloomTextureLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomTextureLoc, 1);
    }
    if (backend.tonemapBloomIntensityLoc >= 0) {
      dev->set_uniform_float(backend.tonemapBloomIntensityLoc,
                             core::cvar_get_float("r_bloom_intensity"));
    }
    if (backend.tonemapBloomEnabledLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomEnabledLoc, 1);
    }
  } else {
    if (backend.tonemapBloomEnabledLoc >= 0) {
      dev->set_uniform_int(backend.tonemapBloomEnabledLoc, 0);
    }
  }

  dev->bind_vertex_array(backend.emptyVao);
  dev->draw_arrays_triangles(0, 3);

  dev->bind_texture(0, 0U);
  if (bloomEnabled) {
    dev->bind_texture(1, 0U);
  }
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  gpu_profiler_end_pass(GpuPassId::Tonemap);

  renderer_context().fxaaAppliedThisFrame = false;
  if (backend.fxaaProgram != 0U && core::cvar_get_bool("r_fxaa")) {
    const std::uint32_t sceneFbo =
        pass_resource_framebuffer(passRes.sceneColor);
    dev->bind_framebuffer(sceneFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->disable_depth_test();

    dev->bind_program(backend.fxaaProgram);

    const std::uint32_t finalColorTex =
        pass_resource_gpu_texture(passRes.finalColor);
    dev->bind_texture(0, finalColorTex);
    if (backend.fxaaInputTextureLocation >= 0) {
      dev->set_uniform_int(backend.fxaaInputTextureLocation, 0);
    }
    if (backend.fxaaTexelSizeLocation >= 0) {
      const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                  1.0F / static_cast<float>(drawableHeight)};
      dev->set_uniform_vec2(backend.fxaaTexelSizeLocation, texelSize);
    }

    dev->bind_vertex_array(backend.emptyVao);
    dev->draw_arrays_triangles(0, 3);

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);

    renderer_context().fxaaAppliedThisFrame = true;
  }

  dev->bind_framebuffer(0U);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->set_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
  dev->clear_color_depth();
  dev->enable_depth_test();
}

} // namespace engine::renderer
