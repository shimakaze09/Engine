// Implements the forward path (opaque batches, sky, transparent pass)
// and the depth-tested debug overlay both paths share; the overlay draws
// lines directly and wire spheres as three tessellated great circles
// through the same line pipeline, while text primitives log a one-time
// unsupported diagnostic (audit M-08).
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
#include <numbers>
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

void flush_forward_path(FrameFlushContext &ctx) noexcept {
  BackendState &backend = ctx.backend;
  const RenderDevice *dev = ctx.dev;
  const SceneLightData &lights = ctx.lights;
  const CommandBufferView &commandBufferView = ctx.commandBufferView;
  const GpuMeshRegistry *registry = ctx.registry;
  const PassResources &passRes = ctx.passRes;
  const int drawableWidth = ctx.drawableWidth;
  const int drawableHeight = ctx.drawableHeight;
  const std::size_t opaqueCount = ctx.opaqueCount;
  const std::size_t totalCount = ctx.totalCount;
  const std::size_t opaqueBatchCount = ctx.opaqueBatchCount;
  const float timeSeconds = ctx.timeSeconds;
  const DistanceFogSettings &fogSettings = ctx.fogSettings;
  const HeightFogSettings &heightFogSettings = ctx.heightFogSettings;
  const math::Mat4 &viewMat = ctx.viewMat;
  const math::Mat4 &projMat = ctx.projMat;
  const math::Mat4 &viewProjection = ctx.viewProjection;
  const bool iblAvailable = ctx.iblAvailable;
  const std::uint32_t envSkyboxTexture = ctx.envSkyboxTexture;
  const bool shadowEnabled = ctx.shadowEnabled;
  const bool doSpotShadows = ctx.doSpotShadows;
  const bool doPointShadows = ctx.doPointShadows;
  RendererFrameStats &frameStats = ctx.frameStats;
    const std::uint32_t sceneFbo =
        pass_resource_framebuffer(passRes.sceneColor);

    gpu_profiler_begin_pass(GpuPassId::Scene);
    dev->bind_framebuffer(sceneFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->enable_depth_test();
    dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
    dev->clear_color_depth();

    dev->bind_program(backend.pbrProgram);

    if (backend.pbrTimeLocation >= 0) {
      dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation >= 0) {
      dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                            &renderer_context().activeCamera.position.x);
    }
    if (backend.pbrViewLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewLocation, &viewMat.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation >= 0) {
      dev->set_uniform_mat4(backend.pbrViewProjectionLocation,
                            &viewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation >= 0) {
      dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    apply_pbr_ibl_uniforms(backend, dev, iblAvailable);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled, doSpotShadows,
                             doPointShadows);

    if (backend.pbrAlbedoMapLocation >= 0) {
      dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
    }

    auto drawForwardCommand = [&](const DrawCommand &command,
                                  const GpuMesh &mesh) {
      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);

      if (backend.pbrUseInstancingLocation >= 0) {
        dev->set_uniform_int(backend.pbrUseInstancingLocation, 0);
      }
      upload_pbr_foliage_uniforms(backend, dev, command);
      if (backend.pbrModelLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
      }
      dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
      dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

      if (mesh.indexCount > 0U) {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.indexCount / 3U);
        dev->draw_elements_triangles_u32(
            static_cast<std::int32_t>(mesh.indexCount));
      } else {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.vertexCount / 3U);
        dev->draw_arrays_triangles(0,
                                   static_cast<std::int32_t>(mesh.vertexCount));
      }
    };

    auto uploadForwardMaterial = [&](const Material &material,
                                     std::uint32_t *boundAlbedoTexture) {
      if (backend.pbrAlbedoLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrAlbedoLocation, &material.albedo.x);
      }
      if (backend.pbrRoughnessLocation >= 0) {
        dev->set_uniform_float(backend.pbrRoughnessLocation,
                               material.roughness);
      }
      if (backend.pbrMetallicLocation >= 0) {
        dev->set_uniform_float(backend.pbrMetallicLocation, material.metallic);
      }
      if (backend.pbrOpacityLocation >= 0) {
        dev->set_uniform_float(backend.pbrOpacityLocation, material.opacity);
      }

      const std::uint32_t albedoGpuId = texture_gpu_id(material.albedoTexture);
      const bool hasAlbedoTex =
          (material.albedoTexture != kInvalidTextureHandle) &&
          (albedoGpuId != 0U);
      if (backend.pbrHasAlbedoTextureLocation >= 0) {
        dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                             hasAlbedoTex ? 1 : 0);
      }
      if (hasAlbedoTex && (albedoGpuId != *boundAlbedoTexture)) {
        dev->bind_texture(0, albedoGpuId);
        *boundAlbedoTexture = albedoGpuId;
      } else if (!hasAlbedoTex && (*boundAlbedoTexture != 0U)) {
        dev->bind_texture(0, 0U);
        *boundAlbedoTexture = 0U;
      }
    };

    auto drawRange = [&](std::size_t start, std::size_t end) {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTexture = 0U;

      if ((start == 0U) && (end == opaqueCount)) {
        for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
             ++batchIndex) {
          const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
          const DrawCommand &command = commandBufferView.data[batch.first];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U)) {
            continue;
          }

          if (mesh->vertexArray != boundVertexArray) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVertexArray = mesh->vertexArray;
          }

          uploadForwardMaterial(command.material, &boundAlbedoTexture);
          upload_pbr_foliage_uniforms(backend, dev, command);

          if ((batch.count > 1U) && !mesh->hasSkin &&
              (mesh->indexCount > 0U) &&
              upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                       batch)) {
            boundVertexArray = mesh->vertexArray;
            if (backend.pbrUseInstancingLocation >= 0) {
              dev->set_uniform_int(backend.pbrUseInstancingLocation, 1);
            }
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U) *
                                        static_cast<std::uint64_t>(batch.count);
            dev->draw_elements_triangles_u32_instanced(
                static_cast<std::int32_t>(mesh->indexCount),
                static_cast<std::int32_t>(batch.count));
            continue;
          }

          for (std::uint32_t local = 0U; local < batch.count; ++local) {
            const std::size_t commandIndex =
                static_cast<std::size_t>(batch.first) +
                static_cast<std::size_t>(local);
            drawForwardCommand(commandBufferView.data[commandIndex], *mesh);
          }
        }
        return;
      }

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

        uploadForwardMaterial(command.material, &boundAlbedoTexture);
        drawForwardCommand(command, *mesh);
      }
    };

    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawRange(0U, opaqueCount);

    const SkyModel skyModel = selected_sky_model();
    const std::uint32_t skyboxTexture = envSkyboxTexture;
    if (skyboxTexture != 0U) {
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      draw_skybox(backend, dev, viewMat, projMat, skyboxTexture, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      draw_hosek_sky(backend, dev, viewMat, projMat, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      draw_preetham_sky(backend, dev, viewMat, projMat, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    }

    if (opaqueCount < totalCount) {
      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawRange(opaqueCount, totalCount);

      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
    }

    dev->bind_texture(0, 0U);
    unbind_pbr_shadow_textures(dev);
    unbind_pbr_ibl_textures(dev);
    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::Scene);
}

namespace {

constexpr std::size_t kMaxDebugLineDraws = 1024U;
constexpr std::size_t kMaxDebugSphereDraws = 512U;
constexpr std::size_t kDebugSphereCircleSegments = 16U;
constexpr std::size_t kDebugSegmentChunk = 1024U;
constexpr std::size_t kDebugFloatsPerSegment = 14U;

/// Uploads and draws one accumulated chunk of debug line segments.
void draw_debug_segment_chunk(const RenderDevice *dev, const float *vertices,
                              std::size_t segmentCount) noexcept {
  dev->buffer_data_array(vertices,
                         static_cast<std::ptrdiff_t>(
                             segmentCount * kDebugFloatsPerSegment *
                             sizeof(float)));
  dev->draw_arrays_lines(0, static_cast<std::int32_t>(segmentCount * 2U));
}

/// Returns a point on one of a sphere's three axis-aligned great circles.
core::DebugVec3 debug_sphere_circle_point(const core::DebugSphere &sphere,
                                          std::size_t plane,
                                          std::size_t segment) noexcept {
  const float angle =
      (2.0F * std::numbers::pi_v<float> * static_cast<float>(segment)) /
      static_cast<float>(kDebugSphereCircleSegments);
  const float c = sphere.radius * std::cos(angle);
  const float s = sphere.radius * std::sin(angle);
  if (plane == 0U) {
    return {sphere.center.x + c, sphere.center.y + s, sphere.center.z};
  }
  if (plane == 1U) {
    return {sphere.center.x + c, sphere.center.y, sphere.center.z + s};
  }
  return {sphere.center.x, sphere.center.y + c, sphere.center.z + s};
}

} // namespace

void flush_debug_overlay(FrameFlushContext &ctx) noexcept {
  BackendState &backend = ctx.backend;
  const RenderDevice *dev = ctx.dev;
  const PassResources &passRes = ctx.passRes;
  const int drawableWidth = ctx.drawableWidth;
  const int drawableHeight = ctx.drawableHeight;
  const math::Mat4 &viewMat = ctx.viewMat;
  const math::Mat4 &projMat = ctx.projMat;
  if (backend.debugLineAvailable) {
    thread_local static std::array<core::DebugLine, kMaxDebugLineDraws>
        debugLines{};
    thread_local static std::array<core::DebugSphere, kMaxDebugSphereDraws>
        debugSpheres{};
    const std::size_t debugLineCount =
        core::debug_draw_get_lines(debugLines.data(), kMaxDebugLineDraws);
    const std::size_t debugSphereCount =
        core::debug_draw_get_spheres(debugSpheres.data(), kMaxDebugSphereDraws);
    if ((debugLineCount > 0U) || (debugSphereCount > 0U)) {
      thread_local static std::array<
          float, kDebugSegmentChunk * kDebugFloatsPerSegment>
          chunkVertices{};
      std::size_t chunkCount = 0U;
      const auto appendSegment = [&](const core::DebugVec3 &from,
                                     const core::DebugVec3 &to,
                                     const core::DebugColor &color) noexcept {
        const std::size_t base = chunkCount * kDebugFloatsPerSegment;
        const float endpoints[kDebugFloatsPerSegment] = {
            from.x,  from.y,  from.z,  color.r, color.g, color.b, color.a,
            to.x,    to.y,    to.z,    color.r, color.g, color.b, color.a};
        for (std::size_t f = 0U; f < kDebugFloatsPerSegment; ++f) {
          chunkVertices[base + f] = endpoints[f];
        }
        ++chunkCount;
        if (chunkCount == kDebugSegmentChunk) {
          draw_debug_segment_chunk(dev, chunkVertices.data(), chunkCount);
          chunkCount = 0U;
        }
      };

      const math::Mat4 debugLineViewProjection = math::mul(projMat, viewMat);
      dev->bind_framebuffer(pass_resource_framebuffer(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->enable_depth_test();
      dev->enable_blending();
      dev->set_blend_func_alpha();

      dev->bind_program(backend.debugLineProgram);
      if (backend.debugLineViewProjectionLoc >= 0) {
        dev->set_uniform_mat4(backend.debugLineViewProjectionLoc,
                              &debugLineViewProjection.columns[0].x);
      }
      dev->bind_vertex_array(backend.debugLineVao);
      dev->bind_array_buffer(backend.debugLineVbo);

      for (std::size_t i = 0U; i < debugLineCount; ++i) {
        appendSegment(debugLines[i].from, debugLines[i].to,
                      debugLines[i].color);
      }
      for (std::size_t i = 0U; i < debugSphereCount; ++i) {
        const core::DebugSphere &sphere = debugSpheres[i];
        for (std::size_t plane = 0U; plane < 3U; ++plane) {
          core::DebugVec3 prev = debug_sphere_circle_point(sphere, plane, 0U);
          for (std::size_t seg = 1U; seg <= kDebugSphereCircleSegments;
               ++seg) {
            const core::DebugVec3 next = debug_sphere_circle_point(
                sphere, plane, seg % kDebugSphereCircleSegments);
            appendSegment(prev, next, sphere.color);
            prev = next;
          }
        }
      }
      if (chunkCount > 0U) {
        draw_debug_segment_chunk(dev, chunkVertices.data(), chunkCount);
      }

      dev->bind_array_buffer(0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      dev->disable_blending();
    }
  }
  static bool warnedTextUnsupported = false;
  if (!warnedTextUnsupported) {
    core::DebugText textProbe[1]{};
    if (core::debug_draw_get_texts(textProbe, 1U) > 0U) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "debug text primitives are collected but not "
                        "rendered; no font pipeline exists yet");
      warnedTextUnsupported = true;
    }
  }
  core::debug_draw_tick();
}

} // namespace engine::renderer
