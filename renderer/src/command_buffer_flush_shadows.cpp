// Implements the shadow-map passes: cached directional cascades, and the
// nearest-caster selection plus depth rendering for spot and point
// (cubemap) shadow lights.
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

namespace {
// Nearest-caster selection entry: light index + squared camera distance.
struct ShadowCandidate final {
  std::size_t lightIndex = 0U;
  float distSq = 0.0F;
};
} // namespace

void flush_shadow_passes(FrameFlushContext &ctx) noexcept {
  BackendState &backend = ctx.backend;
  const RenderDevice *dev = ctx.dev;
  const SceneLightData &lights = ctx.lights;
  const CommandBufferView &commandBufferView = ctx.commandBufferView;
  const GpuMeshRegistry *registry = ctx.registry;
  const std::size_t opaqueCount = ctx.opaqueCount;
  RendererFrameStats &frameStats = ctx.frameStats;
  const math::Mat4 &viewMat = ctx.viewMat;
  const math::Mat4 &projMat = ctx.projMat;
  const float nearP = ctx.nearP;
  const float farP = ctx.farP;
  const bool shadowEnabled = backend.shadowAvailable &&
                             core::cvar_get_bool("r_shadows", true) &&
                             lights.directionalLightCount > 0U;

  CascadeSplits cascadeSplits{};
  bool directionalShadowCacheReused = false;
  if (shadowEnabled && (commandBufferView.data != nullptr) &&
      (opaqueCount > 0U)) {
    const float lambda = core::cvar_get_float("r_shadow_lambda", 0.75F);
    cascadeSplits = compute_cascade_splits(nearP, farP, lambda);

    const math::Vec3 &lightDir = lights.directionalLights[0].direction;
    std::array<math::Mat4, kShadowCascadeCount> lightMatrices{};

    for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
      const int shadowResolution = (backend.shadowState.resolutions[c] > 0)
                                       ? backend.shadowState.resolutions[c]
                                       : shadow_cascade_resolution(c);
      math::Mat4 lightVP = compute_cascade_matrix(
          viewMat, projMat, lightDir, cascadeSplits.distances[c],
          cascadeSplits.distances[c + 1], shadowResolution);
      lightVP = snap_to_texel(lightVP, shadowResolution);

      backend.shadowState.cascades[c].lightViewProjection = lightVP;
      backend.shadowState.cascades[c].splitDistance =
          cascadeSplits.distances[c + 1];
      lightMatrices[c] = lightVP;
    }

    const std::uint64_t cacheKey = directional_shadow_cache_key(
        commandBufferView, opaqueCount, lights.directionalLights[0],
        cascadeSplits, lightMatrices);
    // Skinned poses change every frame, so cached maps would freeze a
    // character's shadow mid-animation.
    const bool cacheEnabled = core::cvar_get_bool("r_shadow_cache", true) &&
                              (skin_palette_count() == 0U);
    directionalShadowCacheReused =
        cacheEnabled && backend.directionalShadowCacheValid &&
        (backend.directionalShadowCacheKey == cacheKey);

    if (directionalShadowCacheReused) {
      if (core::cvar_get_bool("r_shadow_debug", false)) {
        core::log_message(core::LogLevel::Info, "renderer",
                          "reused directional shadow maps");
      }
    } else {
      gpu_profiler_begin_pass(GpuPassId::ShadowMap);

      for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
        const math::Mat4 &lightVP = lightMatrices[c];
        const int shadowResolution = (backend.shadowState.resolutions[c] > 0)
                                         ? backend.shadowState.resolutions[c]
                                         : shadow_cascade_resolution(c);

        dev->bind_framebuffer(backend.shadowState.depthFbos[c]);
        dev->set_viewport(0, 0, shadowResolution, shadowResolution);
        dev->enable_depth_test();
        dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
        dev->clear_color_depth();

        dev->bind_program(backend.shadowDepthProgram);

        std::uint32_t boundVertexArray = 0U;
        for (std::size_t i = 0U; i < opaqueCount; ++i) {
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

          const math::Mat4 lightMvp = math::mul(lightVP, command.modelMatrix);
          const bool skinnedDraw =
              mesh->hasSkin && (command.skinPalette != kInvalidSkinPalette) &&
              (backend.shadowDepthSkinnedProgram != 0U) &&
              upload_bone_palette(backend, dev, command.skinPalette);
          if (skinnedDraw) {
            dev->bind_program(backend.shadowDepthSkinnedProgram);
            if (backend.shadowSkinnedLightMvpLoc >= 0) {
              dev->set_uniform_mat4(backend.shadowSkinnedLightMvpLoc,
                                    &lightMvp.columns[0].x);
            }
          } else {
            if (backend.shadowLightMvpLoc >= 0) {
              dev->set_uniform_mat4(backend.shadowLightMvpLoc,
                                    &lightMvp.columns[0].x);
            }
            if (backend.shadowModelLoc >= 0) {
              dev->set_uniform_mat4(backend.shadowModelLoc,
                                    &command.modelMatrix.columns[0].x);
            }
          }

          if (mesh->indexCount > 0U) {
            dev->draw_elements_triangles_u32(
                static_cast<std::int32_t>(mesh->indexCount));
            frameStats.triangleCount += mesh->indexCount / 3U;
          } else {
            dev->draw_arrays_triangles(
                0, static_cast<std::int32_t>(mesh->vertexCount));
            frameStats.triangleCount += mesh->vertexCount / 3U;
          }
          ++frameStats.drawCalls;
          if (skinnedDraw) {
            dev->bind_program(backend.shadowDepthProgram);
          }
        }

        dev->bind_vertex_array(0U);
        dev->bind_program(0U);
      }

      gpu_profiler_end_pass(GpuPassId::ShadowMap);
      backend.directionalShadowCacheKey = cacheKey;
      backend.directionalShadowCacheValid = true;
    }
  } else {
    backend.directionalShadowCacheKey = 0U;
    backend.directionalShadowCacheValid = false;
  }

  const bool doSpotShadows =
      backend.spotShadowAvailable && core::cvar_get_bool("r_spot_shadows");
  if (doSpotShadows && (lights.spotLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::SpotShadowMap);

    for (std::size_t i = 0U; i < kMaxSpotShadowLights; ++i) {
      backend.spotShadowState.slots[i].lightIndex = -1;
    }

    std::array<ShadowCandidate, kMaxSpotLights> spotCandidates{};
    std::size_t spotCandidateCount = 0U;
    const math::Vec3 &camPos = renderer_context().activeCamera.position;
    for (std::size_t li = 0U; li < lights.spotLightCount; ++li) {
      if (!lights.spotLights[li].castShadow) {
        continue;
      }
      const math::Vec3 &p = lights.spotLights[li].position;
      const float dx = p.x - camPos.x;
      const float dy = p.y - camPos.y;
      const float dz = p.z - camPos.z;
      spotCandidates[spotCandidateCount++] = {li, dx * dx + dy * dy + dz * dz};
    }
    std::sort(spotCandidates.data(), spotCandidates.data() + spotCandidateCount,
              [](const ShadowCandidate &a, const ShadowCandidate &b) noexcept {
                return a.distSq < b.distSq;
              });
    if ((spotCandidateCount > kMaxSpotShadowLights) &&
        core::cvar_get_bool("r_shadow_debug")) {
      core::log_message(core::LogLevel::Warning, "shadow",
                        "spot shadow casters dropped: only 4 slots available");
    }
    const std::size_t activeSpotShadows =
        std::min(spotCandidateCount, kMaxSpotShadowLights);
    for (std::size_t s = 0U; s < activeSpotShadows; ++s) {
      const std::size_t li = spotCandidates[s].lightIndex;
      auto &slot = backend.spotShadowState.slots[s];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = lights.spotLights[li].radius;
      slot.lightViewProjection = compute_spot_shadow_matrix(
          lights.spotLights[li].position, lights.spotLights[li].direction,
          lights.spotLights[li].outerConeAngle, lights.spotLights[li].radius);
    }

    dev->bind_program(backend.shadowDepthProgram);
    for (std::size_t s = 0U; s < activeSpotShadows; ++s) {
      const auto &slot = backend.spotShadowState.slots[s];
      dev->bind_framebuffer(slot.depthFbo);
      dev->set_viewport(0, 0, kSpotShadowMapResolution,
                        kSpotShadowMapResolution);
      dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
      dev->clear_color_depth();
      dev->enable_depth_test();

      std::uint32_t boundVao = 0U;
      for (std::size_t ci = 0U; ci < opaqueCount; ++ci) {
        const DrawCommand &cmd = commandBufferView.data[ci];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
        if ((mesh == nullptr) || (mesh->vertexArray == 0U)) {
          continue;
        }

        const math::Mat4 mvp =
            math::mul(slot.lightViewProjection, cmd.modelMatrix);
        const bool skinnedDraw =
            mesh->hasSkin && (cmd.skinPalette != kInvalidSkinPalette) &&
            (backend.shadowDepthSkinnedProgram != 0U) &&
            upload_bone_palette(backend, dev, cmd.skinPalette);
        if (skinnedDraw) {
          dev->bind_program(backend.shadowDepthSkinnedProgram);
          if (backend.shadowSkinnedLightMvpLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowSkinnedLightMvpLoc,
                                  &mvp.columns[0].x);
          }
        } else {
          if (backend.shadowLightMvpLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowLightMvpLoc, &mvp.columns[0].x);
          }
          if (backend.shadowModelLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowModelLoc,
                                  &cmd.modelMatrix.columns[0].x);
          }
        }

        if (mesh->vertexArray != boundVao) {
          dev->bind_vertex_array(mesh->vertexArray);
          boundVao = mesh->vertexArray;
        }
        if (mesh->indexCount > 0U) {
          dev->draw_elements_triangles_u32(
              static_cast<std::int32_t>(mesh->indexCount));
        } else {
          dev->draw_arrays_triangles(
              0, static_cast<std::int32_t>(mesh->vertexCount));
        }
        if (skinnedDraw) {
          dev->bind_program(backend.shadowDepthProgram);
        }
      }
    }

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::SpotShadowMap);
  }

  const bool doPointShadows =
      backend.pointShadowAvailable && core::cvar_get_bool("r_point_shadows");
  if (doPointShadows && (lights.pointLightCount > 0U)) {
    gpu_profiler_begin_pass(GpuPassId::PointShadowMap);

    for (std::size_t i = 0U; i < kMaxPointShadowLights; ++i) {
      backend.pointShadowState.slots[i].lightIndex = -1;
    }

    std::array<ShadowCandidate, kMaxPointLights> pointCandidates{};
    std::size_t pointCandidateCount = 0U;
    const math::Vec3 &camPos = renderer_context().activeCamera.position;
    for (std::size_t li = 0U; li < lights.pointLightCount; ++li) {
      if (!lights.pointLights[li].castShadow) {
        continue;
      }
      const math::Vec3 &p = lights.pointLights[li].position;
      const float dx = p.x - camPos.x;
      const float dy = p.y - camPos.y;
      const float dz = p.z - camPos.z;
      pointCandidates[pointCandidateCount++] = {li,
                                                dx * dx + dy * dy + dz * dz};
    }
    std::sort(pointCandidates.data(),
              pointCandidates.data() + pointCandidateCount,
              [](const ShadowCandidate &a, const ShadowCandidate &b) noexcept {
                return a.distSq < b.distSq;
              });
    if ((pointCandidateCount > kMaxPointShadowLights) &&
        core::cvar_get_bool("r_shadow_debug")) {
      core::log_message(core::LogLevel::Warning, "shadow",
                        "point shadow casters dropped: only 4 slots available");
    }
    const std::size_t activePointShadows =
        std::min(pointCandidateCount, kMaxPointShadowLights);
    for (std::size_t s = 0U; s < activePointShadows; ++s) {
      const std::size_t li = pointCandidates[s].lightIndex;
      auto &slot = backend.pointShadowState.slots[s];
      slot.lightIndex = static_cast<int>(li);
      slot.farPlane = std::max(lights.pointLights[li].radius, 1.0F);
      compute_point_shadow_matrices(lights.pointLights[li].position,
                                    lights.pointLights[li].radius,
                                    slot.faceViewProjections);
    }

    dev->bind_program(backend.shadowDepthPointProgram);
    for (std::size_t s = 0U; s < activePointShadows; ++s) {
      const auto &slot = backend.pointShadowState.slots[s];
      const math::Vec3 &lightPos =
          lights.pointLights[static_cast<std::size_t>(slot.lightIndex)]
              .position;

      if (backend.shadowPointLightPosLoc >= 0) {
        dev->set_uniform_vec3(backend.shadowPointLightPosLoc, &lightPos.x);
      }
      if (backend.shadowPointFarPlaneLoc >= 0) {
        dev->set_uniform_float(backend.shadowPointFarPlaneLoc, slot.farPlane);
      }

      for (int face = 0; face < 6; ++face) {
        dev->framebuffer_cubemap_face(slot.depthFbo, slot.depthCubemap, face);
        dev->bind_framebuffer(slot.depthFbo);
        dev->set_viewport(0, 0, kPointShadowMapResolution,
                          kPointShadowMapResolution);
        dev->set_clear_color(1.0F, 1.0F, 1.0F, 1.0F);
        dev->clear_color_depth();
        dev->enable_depth_test();

        std::uint32_t boundVao = 0U;
        for (std::size_t ci = 0U; ci < opaqueCount; ++ci) {
          const DrawCommand &cmd = commandBufferView.data[ci];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U)) {
            continue;
          }

          // The point shader multiplies u_lightMVP by the world-space
          // position (u_model * aPosition), so upload the face VP alone —
          // including the model here would apply it twice.
          if (backend.shadowPointLightMvpLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowPointLightMvpLoc,
                                  &slot.faceViewProjections[face].columns[0].x);
          }
          if (backend.shadowPointModelLoc >= 0) {
            dev->set_uniform_mat4(backend.shadowPointModelLoc,
                                  &cmd.modelMatrix.columns[0].x);
          }

          if (mesh->vertexArray != boundVao) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVao = mesh->vertexArray;
          }
          if (mesh->indexCount > 0U) {
            dev->draw_elements_triangles_u32(
                static_cast<std::int32_t>(mesh->indexCount));
          } else {
            dev->draw_arrays_triangles(
                0, static_cast<std::int32_t>(mesh->vertexCount));
          }
        }
      }
    }

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::PointShadowMap);
  }
  ctx.shadowEnabled = shadowEnabled;
  ctx.directionalShadowCacheReused = directionalShadowCacheReused;
  ctx.doSpotShadows = doSpotShadows;
  ctx.doPointShadows = doPointShadows;
}

} // namespace engine::renderer
