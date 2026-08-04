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
    dev->bind_framebuffer(target.framebuffer);
    dev->set_viewport(0, 0, captureWidth, captureHeight);
    dev->enable_depth_test();
    dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
    dev->clear_color_depth();

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
    if (backend.pbrTimeLocation >= 0) {
      dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation >= 0) {
      dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                            &request.camera.position.x);
    }
    if (backend.pbrViewLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewLocation,
                            &captureView.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                            &captureViewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation >= 0) {
      dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, false, false, false);
    // Captures skip sky and IBL by design.
    apply_pbr_ibl_uniforms(backend, dev, false);
    if (backend.pbrAlbedoMapLocation >= 0) {
      dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
    }

    auto drawCaptureRange = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTexture = 0U;
      for (std::size_t i = start; i < end; ++i) {
        const DrawCommand &command = commandBufferView.data[i];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (mesh->vertexArray != boundVertexArray) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVertexArray = mesh->vertexArray;
        }

        if (backend.pbrAlbedoLocation >= 0) {
          dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                                &command.material.albedo.x);
        }
        if (backend.pbrRoughnessLocation >= 0) {
          dev->set_uniform_float(backend.pbrRoughnessLocation,
                                 command.material.roughness);
        }
        if (backend.pbrMetallicLocation >= 0) {
          dev->set_uniform_float(backend.pbrMetallicLocation,
                                 command.material.metallic);
        }
        if (backend.pbrOpacityLocation >= 0) {
          dev->set_uniform_float(backend.pbrOpacityLocation,
                                 command.material.opacity);
        }
        upload_pbr_foliage_uniforms(backend, dev, command);

        const std::uint32_t albedoGpuId =
            texture_gpu_id(command.material.albedoTexture);
        const bool hasAlbedoTex =
            (command.material.albedoTexture != kInvalidTextureHandle) &&
            (albedoGpuId != 0U);
        if (backend.pbrHasAlbedoTextureLocation >= 0) {
          dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                               hasAlbedoTex ? 1 : 0);
        }
        if (hasAlbedoTex && (albedoGpuId != boundAlbedoTexture)) {
          dev->bind_texture(0, albedoGpuId);
          boundAlbedoTexture = albedoGpuId;
        } else if (!hasAlbedoTex && (boundAlbedoTexture != 0U)) {
          dev->bind_texture(0, 0U);
          boundAlbedoTexture = 0U;
        }

        const math::Mat4 model = compute_model_matrix(command);
        const math::Mat4 mvp = compute_mvp(model, captureViewProjection);
        float normalMatrix[9] = {};
        extract_normal_matrix(model, normalMatrix);
        if (backend.pbrModelLocation >= 0) {
          dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
        }
        dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
        dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

        if (mesh->indexCount > 0U) {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->indexCount / 3U);
          dev->draw_elements_triangles_u32(
              static_cast<std::int32_t>(mesh->indexCount));
        } else {
          ++frameStats.drawCalls;
          frameStats.triangleCount += (mesh->vertexCount / 3U);
          dev->draw_arrays_triangles(
              0, static_cast<std::int32_t>(mesh->vertexCount));
        }
      }
    };

    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawCaptureRange(0U, opaqueCount);

    if (opaqueCount < totalCount) {
      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawCaptureRange(opaqueCount, totalCount);
      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
    }

    dev->bind_texture(0, 0U);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
  }
  if ((captureCount > 0U) && (dev != nullptr) &&
      (dev->bind_framebuffer != nullptr)) {
    dev->bind_framebuffer(0U);
  }
}

} // namespace engine::renderer
