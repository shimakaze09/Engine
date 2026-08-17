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

    if ((commandBufferView.data == nullptr) || (totalCount == 0U)) {
      continue;
    }

    const float captureAspect = static_cast<float>(captureWidth) /
                                static_cast<float>(captureHeight);
    const math::Mat4 captureView = math::look_at(
        request.camera.position, request.camera.target, request.camera.up);
    const math::Mat4 captureProj =
        math::perspective(request.camera.fovRadians, captureAspect,
                          request.camera.nearPlane, request.camera.farPlane);
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

    auto drawCaptureRange = [&](std::size_t start, std::size_t end) {
      DeviceTextureHandle boundAlbedoTexture{};
      for (std::size_t i = start; i < end; ++i) {
        const DrawCommand &command = commandBufferView.data[i];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->geometry == kInvalidDeviceGeometry) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (backend.pbrAlbedoLocation.valid()) {
          dev->set_param_vec3(backend.pbrAlbedoLocation,
                                &command.material.albedo.x);
        }
        if (backend.pbrRoughnessLocation.valid()) {
          dev->set_param_f32(backend.pbrRoughnessLocation,
                                 command.material.roughness);
        }
        if (backend.pbrMetallicLocation.valid()) {
          dev->set_param_f32(backend.pbrMetallicLocation,
                                 command.material.metallic);
        }
        if (backend.pbrOpacityLocation.valid()) {
          dev->set_param_f32(backend.pbrOpacityLocation,
                                 command.material.opacity);
        }
        upload_pbr_foliage_uniforms(backend, dev, command);

        const DeviceTextureHandle albedoTex =
            texture_device_handle(command.material.albedoTexture);
        const bool hasAlbedoTex =
            (command.material.albedoTexture != kInvalidTextureHandle) &&
            (albedoTex != kInvalidDeviceTexture);
        if (backend.pbrHasAlbedoTextureLocation.valid()) {
          dev->set_param_i32(backend.pbrHasAlbedoTextureLocation,
                               hasAlbedoTex ? 1 : 0);
        }
        if (hasAlbedoTex && (albedoTex != boundAlbedoTexture)) {
          dev->bind_texture_slot(0U, albedoTex);
          boundAlbedoTexture = albedoTex;
        } else if (!hasAlbedoTex &&
                   (boundAlbedoTexture != kInvalidDeviceTexture)) {
          dev->bind_texture_slot(0U, kInvalidDeviceTexture);
          boundAlbedoTexture = kInvalidDeviceTexture;
        }

        const math::Mat4 model = compute_model_matrix(command);
        const math::Mat4 mvp = compute_mvp(model, captureViewProjection);
        float normalMatrix[9] = {};
        extract_normal_matrix(model, normalMatrix);
        if (backend.pbrModelLocation.valid()) {
          dev->set_param_mat4(backend.pbrModelLocation, &model.columns[0].x);
        }
        dev->set_param_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
        dev->set_param_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

        if (mesh->indexCount > 0U) {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->indexCount / 3U);
          dev->draw_indexed(mesh->geometry,
                            static_cast<std::int32_t>(mesh->indexCount));
        } else {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->vertexCount / 3U);
          dev->draw(mesh->geometry, PrimitiveTopology::Triangles, 0,
                    static_cast<std::int32_t>(mesh->vertexCount));
        }
      }
    };

    drawCaptureRange(0U, opaqueCount);

    if (opaqueCount < totalCount) {
      dev->apply_render_state(RenderState{DepthTest::Less, false,
                                          BlendMode::Alpha, CullMode::None});
      drawCaptureRange(opaqueCount, totalCount);
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
