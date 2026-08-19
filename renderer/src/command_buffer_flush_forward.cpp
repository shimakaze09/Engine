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
  const math::Mat4 &viewProjection = ctx.viewProjection;
  const bool iblAvailable = ctx.iblAvailable;
  const DeviceTextureHandle envSkyboxTexture = ctx.envSkyboxTexture;
  const bool shadowEnabled = ctx.shadowEnabled;
  const bool doSpotShadows = ctx.doSpotShadows;
  const bool doPointShadows = ctx.doPointShadows;
  RendererFrameStats &frameStats = ctx.frameStats;
    const RenderTargetHandle sceneTarget =
        pass_resource_target(passRes.sceneColor);

    gpu_profiler_begin_pass(GpuPassId::Scene);
    dev->bind_render_target(sceneTarget);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->apply_render_state(RenderState{DepthTest::Less, true,
                                        BlendMode::Disabled, CullMode::Back});
    dev->clear(ClearFlags::ColorDepth, kClearRed, kClearGreen, kClearBlue,
               1.0F);

    dev->bind_program(backend.pbrProgram);

    if (backend.pbrTimeLocation.valid()) {
      dev->set_param_f32(backend.pbrTimeLocation, timeSeconds);
    }
    if (backend.pbrCameraPosLocation.valid()) {
      dev->set_param_vec3(backend.pbrCameraPosLocation,
                            &renderer_context().activeCamera.position.x);
    }
    if (backend.pbrCameraForwardOrthoLocation.valid()) {
      // xyz = normalized view direction, w = 1 when orthographic: the
      // shaders switch the view vector to the constant camera forward
      // under ortho (#221) — parallel rays have no per-pixel eye vector.
      const CameraState &activeCam = renderer_context().activeCamera;
      const math::Vec3 fwd = math::normalize(
          math::sub(activeCam.target, activeCam.position));
      const float forwardOrtho[4] = {
          fwd.x, fwd.y, fwd.z,
          (activeCam.projection == CameraState::kProjectionOrthographic)
              ? 1.0F
              : 0.0F};
      dev->set_param_vec4(backend.pbrCameraForwardOrthoLocation,
                          forwardOrtho);
    }
    if (backend.pbrViewLocation.valid()) {
      dev->set_param_mat4(backend.pbrViewLocation, &viewMat.columns[0].x);
    }
    if (backend.pbrViewProjectionLocation.valid()) {
      dev->set_param_mat4(backend.pbrViewProjectionLocation,
                            &viewProjection.columns[0].x);
    }
    if (backend.pbrUseInstancingLocation.valid()) {
      dev->set_param_i32(backend.pbrUseInstancingLocation, 0);
    }
    upload_pbr_lighting_uniforms(backend, dev, lights);
    apply_pbr_ibl_uniforms(backend, dev, iblAvailable);
    upload_pbr_distance_fog_uniforms(backend, dev, fogSettings);
    upload_pbr_height_fog_uniforms(backend, dev, heightFogSettings);
    bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled, doSpotShadows,
                             doPointShadows);

    if (backend.pbrAlbedoMapLocation.valid()) {
      dev->set_param_i32(backend.pbrAlbedoMapLocation, 0);
    }

    const MaterialTextureUniformLocs forwardMaterialTexLocs{
        backend.pbrHasMetallicRoughnessTextureLocation,
        backend.pbrMetallicRoughnessMapLocation,
        backend.pbrHasEmissiveTextureLocation,
        backend.pbrEmissiveMapLocation,
        backend.pbrHasOcclusionTextureLocation,
        backend.pbrOcclusionMapLocation,
        backend.pbrHasOpacityTextureLocation,
        backend.pbrOpacityMapLocation,
        backend.pbrAlphaModeLocation,
        backend.pbrAlphaCutoffLocation,
        backend.pbrUvTilingLocation,
        backend.pbrUvOffsetLocation};

    auto drawForwardCommand = [&](const DrawCommand &command,
                                  const GpuMesh &mesh) {
      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);

      if (backend.pbrUseInstancingLocation.valid()) {
        dev->set_param_i32(backend.pbrUseInstancingLocation, 0);
      }
      upload_pbr_foliage_uniforms(backend, dev, command);
      if (backend.pbrModelLocation.valid()) {
        dev->set_param_mat4(backend.pbrModelLocation, &model.columns[0].x);
      }
      dev->set_param_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
      dev->set_param_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

      if (mesh.indexCount > 0U) {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.indexCount / 3U);
        dev->draw_indexed(mesh.geometry,
                          static_cast<std::int32_t>(mesh.indexCount));
      } else {
        ++frameStats.drawCalls;
        frameStats.triangleCount += (mesh.vertexCount / 3U);
        dev->draw(mesh.geometry, PrimitiveTopology::Triangles, 0,
                  static_cast<std::int32_t>(mesh.vertexCount));
      }
    };

    auto uploadForwardMaterial = [&](const Material &material,
                                     DeviceTextureHandle *boundAlbedoTexture,
                                     DeviceTextureHandle *boundMaterialTex) {
      if (backend.pbrAlbedoLocation.valid()) {
        dev->set_param_vec3(backend.pbrAlbedoLocation, &material.albedo.x);
      }
      if (backend.pbrRoughnessLocation.valid()) {
        dev->set_param_f32(backend.pbrRoughnessLocation,
                               material.roughness);
      }
      if (backend.pbrMetallicLocation.valid()) {
        dev->set_param_f32(backend.pbrMetallicLocation, material.metallic);
      }
      if (backend.pbrOpacityLocation.valid()) {
        dev->set_param_f32(backend.pbrOpacityLocation, material.opacity);
      }
      if (backend.pbrEmissiveLocation.valid()) {
        dev->set_param_vec3(backend.pbrEmissiveLocation,
                              &material.emissive.x);
      }

      const DeviceTextureHandle albedoTex =
          texture_device_handle(material.albedoTexture);
      const bool hasAlbedoTex =
          (material.albedoTexture != kInvalidTextureHandle) &&
          (albedoTex != kInvalidDeviceTexture);
      if (backend.pbrHasAlbedoTextureLocation.valid()) {
        dev->set_param_i32(backend.pbrHasAlbedoTextureLocation,
                             hasAlbedoTex ? 1 : 0);
      }
      if (hasAlbedoTex && (albedoTex != *boundAlbedoTexture)) {
        dev->bind_texture_slot(0U, albedoTex);
        *boundAlbedoTexture = albedoTex;
      } else if (!hasAlbedoTex &&
                 (*boundAlbedoTexture != kInvalidDeviceTexture)) {
        dev->bind_texture_slot(0U, kInvalidDeviceTexture);
        *boundAlbedoTexture = kInvalidDeviceTexture;
      }
      upload_material_texture_slots(forwardMaterialTexLocs, dev, material,
                                    boundMaterialTex);
    };

    auto drawRange = [&](std::size_t start, std::size_t end) {
      DeviceTextureHandle boundAlbedoTexture{};
      DeviceTextureHandle boundMaterialTex[4] = {};

      if ((start == 0U) && (end == opaqueCount)) {
        for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
             ++batchIndex) {
          const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
          const DrawCommand &command = commandBufferView.data[batch.first];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
          if ((mesh == nullptr) ||
              (mesh->geometry == kInvalidDeviceGeometry) ||
              (mesh->vertexCount == 0U)) {
            continue;
          }

          uploadForwardMaterial(command.material, &boundAlbedoTexture,
                                boundMaterialTex);
          upload_pbr_foliage_uniforms(backend, dev, command);

          if ((batch.count > 1U) && !mesh->hasSkin &&
              (mesh->indexCount > 0U) &&
              upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                       batch)) {
            if (backend.pbrUseInstancingLocation.valid()) {
              dev->set_param_i32(backend.pbrUseInstancingLocation, 1);
            }
            ++frameStats.drawCalls;
            frameStats.triangleCount += (mesh->indexCount / 3U) *
                                        static_cast<std::uint64_t>(batch.count);
            dev->draw_indexed_instanced(
                mesh->geometry, static_cast<std::int32_t>(mesh->indexCount),
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
        if ((mesh == nullptr) || (mesh->geometry == kInvalidDeviceGeometry) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        uploadForwardMaterial(command.material, &boundAlbedoTexture,
                                boundMaterialTex);
        drawForwardCommand(command, *mesh);
      }
    };

    drawRange(0U, opaqueCount);

    const SkyModel skyModel = selected_sky_model();
    const DeviceTextureHandle skyboxTexture = envSkyboxTexture;
    const math::Mat4 skyProj = sky_projection_matrix(
        renderer_context().activeCamera,
        (drawableHeight > 0)
            ? (static_cast<float>(drawableWidth) /
               static_cast<float>(drawableHeight))
            : 1.0F);
    if (skyboxTexture != kInvalidDeviceTexture) {
      dev->bind_render_target(sceneTarget);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      draw_skybox(backend, dev, viewMat, skyProj, skyboxTexture, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      draw_hosek_sky(backend, dev, viewMat, skyProj, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      draw_preetham_sky(backend, dev, viewMat, skyProj, lights, frameStats);
      dev->bind_program(backend.pbrProgram);
    }

    if (opaqueCount < totalCount) {
      dev->apply_render_state(RenderState{DepthTest::Less, false,
                                          BlendMode::Alpha, CullMode::None});
      drawRange(opaqueCount, totalCount);

      dev->apply_render_state(RenderState{DepthTest::Less, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});
    }

    dev->bind_texture_slot(0U, kInvalidDeviceTexture);
    unbind_pbr_shadow_textures(dev);
    unbind_pbr_ibl_textures(dev);
    dev->bind_program(kInvalidDeviceProgram);
    gpu_profiler_end_pass(GpuPassId::Scene);
}

namespace {

constexpr std::size_t kMaxDebugLineDraws = 1024U;
constexpr std::size_t kMaxDebugSphereDraws = 512U;
constexpr std::size_t kDebugSphereCircleSegments = 16U;
constexpr std::size_t kDebugSegmentChunk = 1024U;
constexpr std::size_t kDebugFloatsPerSegment = 14U;

/// Uploads and draws one accumulated chunk of debug line segments.
void draw_debug_segment_chunk(const RenderDevice *dev,
                              const BackendState &backend,
                              const float *vertices,
                              std::size_t segmentCount) noexcept {
  dev->update_buffer(backend.debugLineVbo, vertices,
                     static_cast<std::ptrdiff_t>(
                         segmentCount * kDebugFloatsPerSegment *
                         sizeof(float)));
  dev->draw(backend.debugLineGeometry, PrimitiveTopology::Lines, 0,
            static_cast<std::int32_t>(segmentCount * 2U));
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
          draw_debug_segment_chunk(dev, backend, chunkVertices.data(),
                                   chunkCount);
          chunkCount = 0U;
        }
      };

      const math::Mat4 debugLineViewProjection = math::mul(projMat, viewMat);
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      // Depth-tested alpha-blended lines over the scene; depth writes stay
      // on as before (line-over-line occlusion in one frame is acceptable).
      dev->apply_render_state(RenderState{DepthTest::Less, true,
                                          BlendMode::Alpha, CullMode::Back});

      dev->bind_program(backend.debugLineProgram);
      if (backend.debugLineViewProjectionLoc.valid()) {
        dev->set_param_mat4(backend.debugLineViewProjectionLoc,
                              &debugLineViewProjection.columns[0].x);
      }

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
        draw_debug_segment_chunk(dev, backend, chunkVertices.data(),
                                 chunkCount);
      }

      dev->bind_program(kInvalidDeviceProgram);
      dev->apply_render_state(RenderState{DepthTest::Less, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});
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
