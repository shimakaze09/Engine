// Implements the scene-capture render-to-texture passes: forward-lit
// opaque and transparent geometry per capture request into dedicated LDR
// targets, deliberately without sky, shadows, or the post stack.
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

void flush_scene_captures(FrameFlushContext &ctx) noexcept {
  BackendState &backend = ctx.backend;
  const RenderDevice *dev = ctx.dev;
  const SceneLightData &lights = ctx.lights;
  const CommandBufferView &commandBufferView = ctx.commandBufferView;
  const GpuMeshRegistry *registry = ctx.registry;
  const std::size_t opaqueCount = ctx.opaqueCount;
  const std::size_t totalCount = ctx.totalCount;
  const float timeSeconds = ctx.timeSeconds;
  const DistanceFogSettings &fogSettings = ctx.fogSettings;
  const HeightFogSettings &heightFogSettings = ctx.heightFogSettings;
  RendererFrameStats &frameStats = ctx.frameStats;
  const std::size_t captureCount = scene_capture_request_count();
  for (std::size_t captureIndex = 0U; captureIndex < captureCount;
       ++captureIndex) {
    const SceneCaptureRequest &request =
        renderer_context().sceneCaptureRequests[captureIndex];
    const int captureWidth = static_cast<int>(request.width);
    const int captureHeight = static_cast<int>(request.height);
    if (!ensure_scene_capture_target(backend, dev, captureIndex, captureWidth,
                                     captureHeight)) {
      continue;
    }

    const SceneCaptureTarget &target =
        backend.sceneCaptureTargets[captureIndex];
    dev->bind_render_target(target.target);
    dev->set_viewport(0, 0, captureWidth, captureHeight);
    dev->apply_render_state(RenderState{DepthTest::Less, true,
                                        BlendMode::Disabled, CullMode::Back});
    dev->clear(ClearFlags::ColorDepth, kClearRed, kClearGreen, kClearBlue,
               1.0F);

    if (((commandBufferView.data == nullptr) || (totalCount == 0U)) &&
        (ctx.auxiliaryView.count == 0U)) {
      continue;
    }

    const float captureAspect = static_cast<float>(captureWidth) /
                                static_cast<float>(captureHeight);
    const math::Mat4 captureView = math::look_at(
        request.camera.position, request.camera.target, request.camera.up);
    const math::Mat4 captureProj =
        camera_projection_matrix(request.camera, captureAspect);
    const math::Mat4 captureViewProjection =
        math::mul(captureProj, captureView);

    dev->bind_program(backend.pbrProgram);
    if (backend.pbrTimeLocation.valid()) {
      dev->set_param_f32(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation.valid()) {
      dev->set_param_vec3(backend.pbrCameraPosLocation,
                            &request.camera.position.x);
    }
    if (backend.pbrViewLocation.valid()) {
      dev->set_param_mat4(backend.pbrViewLocation,
                            &captureView.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation.valid()) {
      dev->set_param_mat4(backend.pbrViewProjectionLocation,
                            &captureViewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation.valid()) {
      dev->set_param_i32(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, false, false, false);
    // Captures skip sky and IBL by design.
    apply_pbr_ibl_uniforms(backend, dev, false);
    if (backend.pbrAlbedoMapLocation.valid()) {
      dev->set_param_i32(backend.pbrAlbedoMapLocation, 0);
    }

    // Scene captures share pbrProgram, and its GL uniform state, with the
    // main forward pass, so every draw here sets its own material
    // uniforms even when a capture's own materials never use them —
    // otherwise a capture would silently keep whatever texture the last
    // forward draw left bound.
    const ForwardDrawProgram captureProgram =
        pbr_forward_draw_program(backend);

    // Commands render prep culled for the main camera but flagged for
    // this capture ride in the auxiliary list.
    const std::uint16_t captureBit = static_cast<std::uint16_t>(
        kPassCaptureBase << static_cast<unsigned int>(captureIndex));
    auto drawCaptureRange = [&](const CommandBufferView &view,
                                std::size_t start, std::size_t end,
                                std::uint16_t requiredMask) {
      // A mesh showing this capture is drawn without it rather than
      // sampling the target being rendered.
      ForwardDrawBindings bindings{};
      bindings.passTarget = target.colorTexture;
      for (std::size_t i = start; (view.data != nullptr) && (i < end); ++i) {
        const DrawCommand &command = view.data[i];
        if ((requiredMask != 0U) &&
            ((command.passMask & requiredMask) == 0U)) {
          continue;
        }
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->geometry == kInvalidDeviceGeometry) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        upload_forward_material(captureProgram, backend, dev, command,
                                &bindings);
        draw_forward_command(captureProgram, dev, command, *mesh,
                             captureViewProjection, &frameStats);
      }
    };

    const std::size_t auxiliaryTotal =
        static_cast<std::size_t>(ctx.auxiliaryView.count);
    drawCaptureRange(commandBufferView, 0U, opaqueCount, 0U);
    drawCaptureRange(ctx.auxiliaryView, 0U, ctx.auxiliaryOpaqueCount,
                     captureBit);

    if ((opaqueCount < totalCount) ||
        (ctx.auxiliaryOpaqueCount < auxiliaryTotal)) {
      dev->apply_render_state(RenderState{DepthTest::Less, false,
                                          BlendMode::Alpha, CullMode::None});
      drawCaptureRange(commandBufferView, opaqueCount, totalCount, 0U);
      drawCaptureRange(ctx.auxiliaryView, ctx.auxiliaryOpaqueCount,
                       auxiliaryTotal, captureBit);
      dev->apply_render_state(RenderState{DepthTest::Less, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});
    }

    dev->bind_texture_slot(0U, kInvalidDeviceTexture);
    dev->bind_program(kInvalidDeviceProgram);
  }
  if ((captureCount > 0U) && (dev != nullptr) &&
      (dev->bind_render_target != nullptr)) {
    dev->bind_render_target(kBackBufferTarget);
  }
}

} // namespace engine::renderer
