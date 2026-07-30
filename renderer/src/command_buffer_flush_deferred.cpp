// Implements the deferred path: G-Buffer MRT fill, SSAO with blur, CPU
// tile light culling and per-light data-texture uploads, the G-Buffer
// debug or deferred lighting fullscreen pass, sky, and the forward
// transparent tail over the deferred depth.
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

void flush_deferred_path(FrameFlushContext &ctx) noexcept {
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
  const std::uint32_t iblPrefilteredTex = ctx.iblPrefilteredTex;
  const std::uint32_t iblIrradianceTex = ctx.iblIrradianceTex;
  const std::uint32_t envSkyboxTexture = ctx.envSkyboxTexture;
  const int gbufferDebugMode = ctx.gbufferDebugMode;
  const bool shadowEnabled = ctx.shadowEnabled;
  const bool doSpotShadows = ctx.doSpotShadows;
  const bool doPointShadows = ctx.doPointShadows;
  RendererFrameStats &frameStats = ctx.frameStats;
    bool sceneDepthHasOpaque = false;
    auto ensureSceneDepthHasOpaque = [&]() noexcept -> bool {
      if (sceneDepthHasOpaque) {
        return true;
      }
      if (dev->blit_depth == nullptr) {
        return false;
      }

      const std::uint32_t gbufferFbo =
          pass_resource_framebuffer(passRes.gbufferAlbedo);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->blit_depth(gbufferFbo, sceneFbo, drawableWidth, drawableHeight);
      dev->bind_framebuffer(sceneFbo);
      sceneDepthHasOpaque = true;
      return true;
    };

    gpu_profiler_begin_pass(GpuPassId::GBuffer);
    const std::uint32_t gbufferFbo =
        pass_resource_framebuffer(passRes.gbufferAlbedo);
    dev->bind_framebuffer(gbufferFbo);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->enable_depth_test();
    dev->set_clear_color(0.0F, 0.0F, 0.0F, 0.0F);
    dev->clear_color_depth();

    dev->bind_program(backend.gbufferProgram);

    if (backend.gbufViewLoc >= 0) {
      dev->set_uniform_mat4(backend.gbufViewLoc, &viewMat.columns[0].x);
    }
    if (backend.gbufProjectionLoc >= 0) {
      dev->set_uniform_mat4(backend.gbufProjectionLoc, &projMat.columns[0].x);
    }
    if (backend.gbufTimeLoc >= 0) {
      dev->set_uniform_float(backend.gbufTimeLoc, timeSeconds);
    }

    auto drawGBufferBatches = [&]() {
      std::uint32_t boundVertexArray = 0U;
      std::uint32_t boundAlbedoTex = 0U;
      if (backend.gbufAlbedoTextureLoc >= 0) {
        dev->set_uniform_int(backend.gbufAlbedoTextureLoc, 0);
      }
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

        if (backend.gbufAlbedoLoc >= 0) {
          dev->set_uniform_vec3(backend.gbufAlbedoLoc,
                                &command.material.albedo.x);
        }
        if (backend.gbufMetallicLoc >= 0) {
          dev->set_uniform_float(backend.gbufMetallicLoc,
                                 command.material.metallic);
        }
        if (backend.gbufRoughnessLoc >= 0) {
          dev->set_uniform_float(backend.gbufRoughnessLoc,
                                 command.material.roughness);
        }
        if (backend.gbufAOLoc >= 0) {
          dev->set_uniform_float(backend.gbufAOLoc, 1.0F);
        }
        if (backend.gbufEmissiveLoc >= 0) {
          dev->set_uniform_vec3(backend.gbufEmissiveLoc,
                                &command.material.emissive.x);
        }
        const std::uint32_t albedoGpu =
            texture_gpu_id(command.material.albedoTexture);
        const bool hasAlbedoTex =
            (command.material.albedoTexture != kInvalidTextureHandle) &&
            (albedoGpu != 0U);
        if (backend.gbufHasAlbedoTextureLoc >= 0) {
          dev->set_uniform_int(backend.gbufHasAlbedoTextureLoc,
                               hasAlbedoTex ? 1 : 0);
        }
        if (hasAlbedoTex && (albedoGpu != boundAlbedoTex)) {
          dev->bind_texture(0, albedoGpu);
          boundAlbedoTex = albedoGpu;
        } else if (!hasAlbedoTex && (boundAlbedoTex != 0U)) {
          dev->bind_texture(0, 0U);
          boundAlbedoTex = 0U;
        }
        upload_gbuffer_foliage_uniforms(backend, dev, command);

        if ((batch.count > 1U) && (mesh->indexCount > 0U) &&
            upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                     batch)) {
          boundVertexArray = mesh->vertexArray;
          if (backend.gbufUseInstancingLoc >= 0) {
            dev->set_uniform_int(backend.gbufUseInstancingLoc, 1);
          }
          ++frameStats.drawCalls;
          frameStats.triangleCount +=
              (mesh->indexCount / 3U) * static_cast<std::uint64_t>(batch.count);
          dev->draw_elements_triangles_u32_instanced(
              static_cast<std::int32_t>(mesh->indexCount),
              static_cast<std::int32_t>(batch.count));
          continue;
        }

        if (backend.gbufUseInstancingLoc >= 0) {
          dev->set_uniform_int(backend.gbufUseInstancingLoc, 0);
        }
        for (std::uint32_t local = 0U; local < batch.count; ++local) {
          const std::size_t commandIndex =
              static_cast<std::size_t>(batch.first) +
              static_cast<std::size_t>(local);
          const DrawCommand &singleCommand = commandBufferView.data[commandIndex];
          upload_gbuffer_foliage_uniforms(backend, dev, singleCommand);
          const math::Mat4 model = compute_model_matrix(singleCommand);
          float normalMatrix[9] = {};
          extract_normal_matrix(model, normalMatrix);

          if (backend.gbufModelLoc >= 0) {
            dev->set_uniform_mat4(backend.gbufModelLoc, &model.columns[0].x);
          }
          if (backend.gbufNormalMatrixLoc >= 0) {
            dev->set_uniform_mat3(backend.gbufNormalMatrixLoc, normalMatrix);
          }

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
      }
    };

    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
    drawGBufferBatches();

    dev->bind_vertex_array(0U);
    dev->bind_program(0U);
    gpu_profiler_end_pass(GpuPassId::GBuffer);

    const bool ssaoEnabled =
        backend.ssaoAvailable && core::cvar_get_bool("r_ssao", true);
    if (ssaoEnabled) {
      gpu_profiler_begin_pass(GpuPassId::SSAO);
      const std::uint32_t ssaoFbo =
          pass_resource_framebuffer(passRes.ssaoTexture);
      dev->bind_framebuffer(ssaoFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.ssaoProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, backend.ssaoNoiseTexture);

      if (backend.ssaoDepthLoc >= 0)
        dev->set_uniform_int(backend.ssaoDepthLoc, 0);
      if (backend.ssaoNormalLoc >= 0)
        dev->set_uniform_int(backend.ssaoNormalLoc, 1);
      if (backend.ssaoNoiseLoc >= 0)
        dev->set_uniform_int(backend.ssaoNoiseLoc, 2);

      if (backend.ssaoProjectionLoc >= 0)
        dev->set_uniform_mat4(backend.ssaoProjectionLoc, &projMat.columns[0].x);
      if (backend.ssaoViewLoc >= 0)
        dev->set_uniform_mat4(backend.ssaoViewLoc, &viewMat.columns[0].x);

      if (backend.ssaoNoiseScaleLoc >= 0) {
        const float noiseScale[2] = {static_cast<float>(drawableWidth) / 4.0F,
                                     static_cast<float>(drawableHeight) / 4.0F};
        dev->set_uniform_vec2(backend.ssaoNoiseScaleLoc, noiseScale);
      }
      if (backend.ssaoRadiusLoc >= 0)
        dev->set_uniform_float(backend.ssaoRadiusLoc,
                               core::cvar_get_float("r_ssao_radius"));
      if (backend.ssaoBiasLoc >= 0)
        dev->set_uniform_float(backend.ssaoBiasLoc,
                               core::cvar_get_float("r_ssao_bias"));

      for (int i = 0; i < 32; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (backend.ssaoSampleLocs[idx] >= 0) {
          dev->set_uniform_vec3(backend.ssaoSampleLocs[idx],
                                &backend.ssaoKernel[i * 3]);
        }
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);

      const std::uint32_t ssaoBlurFbo =
          pass_resource_framebuffer(passRes.ssaoBlurTexture);
      dev->bind_framebuffer(ssaoBlurFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);

      dev->bind_program(backend.ssaoBlurProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.ssaoTexture));
      if (backend.ssaoBlurInputLoc >= 0)
        dev->set_uniform_int(backend.ssaoBlurInputLoc, 0);
      if (backend.ssaoBlurTexelSizeLoc >= 0) {
        const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                    1.0F / static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.ssaoBlurTexelSizeLoc, texelSize);
      }

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::SSAO);
    }

    const std::size_t tileBufferSize =
        compute_tile_buffer_size(drawableWidth, drawableHeight);
    if (backend.tileBuffer.size() < tileBufferSize) {
      backend.tileBuffer.resize(tileBufferSize, 0.0F);
    }

    TileLightData tileData{};
    tileData.data = backend.tileBuffer.data();
    tileData.dataSize = backend.tileBuffer.size();

    cull_lights_tiled(lights, &viewMat.columns[0].x, &projMat.columns[0].x,
                      drawableWidth, drawableHeight, tileData);

    if ((backend.tileLightTex != 0U) &&
        (tileData.totalTiles > backend.tileLightTexRows)) {
      dev->destroy_texture(backend.tileLightTex);
      backend.tileLightTex = 0U;
      backend.tileLightTexRows = 0;
    }
    if (backend.tileLightTex == 0U) {
      backend.tileLightTex = dev->create_texture_2d_r32f(
          kTileDataWidth, tileData.totalTiles, backend.tileBuffer.data());
      if (backend.tileLightTex != 0U) {
        backend.tileLightTexRows = tileData.totalTiles;
      }
    } else {
      dev->update_texture_2d_r32f(backend.tileLightTex, kTileDataWidth,
                                  tileData.totalTiles,
                                  backend.tileBuffer.data());
    }

    static_cast<void>(pack_light_data(lights, backend.lightDataBuffer.data(),
                                      backend.lightDataBuffer.size()));
    if (backend.lightDataTex == 0U) {
      backend.lightDataTex = dev->create_texture_2d_r32f(
          kLightDataTexWidth, kLightDataTexHeight,
          backend.lightDataBuffer.data());
    } else {
      dev->update_texture_2d_r32f(backend.lightDataTex, kLightDataTexWidth,
                                  kLightDataTexHeight,
                                  backend.lightDataBuffer.data());
    }

    if (gbufferDebugMode > 0 && backend.gbufferDebugProgram != 0U) {
      gpu_profiler_begin_pass(GpuPassId::GBufferDebug);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.gbufferDebugProgram);

      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));

      if (backend.dbgGBufAlbedoLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufAlbedoLoc, 0);
      if (backend.dbgGBufNormalLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufNormalLoc, 1);
      if (backend.dbgGBufEmissiveLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufEmissiveLoc, 2);
      if (backend.dbgGBufDepthLoc >= 0)
        dev->set_uniform_int(backend.dbgGBufDepthLoc, 3);
      // Debug mode:
      // 0=albedo,1=normals,2=metallic,3=roughness,4=emissive,5=AO,6=depth CVar
      // value 1..7 maps to shader 0..6.
      if (backend.dbgModeLoc >= 0)
        dev->set_uniform_int(backend.dbgModeLoc, gbufferDebugMode - 1);

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_texture(3, 0U);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::GBufferDebug);
    } else {
      gpu_profiler_begin_pass(GpuPassId::DeferredLighting);
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->disable_depth_test();

      dev->bind_program(backend.deferredLightProgram);

      // Bind G-Buffer textures on units 0-3, tile on unit 4, SSAO on unit 5,
      // per-light data on unit 18 (units 6-17 hold shadow maps).
      dev->bind_texture(0, pass_resource_gpu_texture(passRes.gbufferAlbedo));
      dev->bind_texture(1, pass_resource_gpu_texture(passRes.gbufferNormal));
      dev->bind_texture(2, pass_resource_gpu_texture(passRes.gbufferEmissive));
      dev->bind_texture(3, pass_resource_gpu_texture(passRes.gbufferDepth));
      dev->bind_texture(4, backend.tileLightTex);
      dev->bind_texture(18, backend.lightDataTex);

      if (ssaoEnabled) {
        dev->bind_texture(5,
                          pass_resource_gpu_texture(passRes.ssaoBlurTexture));
      }

      if (backend.dlGBufAlbedoLoc >= 0)
        dev->set_uniform_int(backend.dlGBufAlbedoLoc, 0);
      if (backend.dlGBufNormalLoc >= 0)
        dev->set_uniform_int(backend.dlGBufNormalLoc, 1);
      if (backend.dlGBufEmissiveLoc >= 0)
        dev->set_uniform_int(backend.dlGBufEmissiveLoc, 2);
      if (backend.dlGBufDepthLoc >= 0)
        dev->set_uniform_int(backend.dlGBufDepthLoc, 3);
      if (backend.dlTileLightTexLoc >= 0)
        dev->set_uniform_int(backend.dlTileLightTexLoc, 4);
      if (backend.dlLightDataTexLoc >= 0)
        dev->set_uniform_int(backend.dlLightDataTexLoc, 18);

      // Sampler units are assigned even when IBL is off: a samplerCube
      // uniform left at its default unit 0 aliases the sampler2D G-buffer
      // there, which is a draw-time GL_INVALID_OPERATION that corrupts
      // every deferred draw.
      if (backend.dlIrradianceMapLoc >= 0) {
        dev->set_uniform_int(backend.dlIrradianceMapLoc, kIblIrradianceUnit);
      }
      if (backend.dlPrefilteredMapLoc >= 0) {
        dev->set_uniform_int(backend.dlPrefilteredMapLoc, kIblPrefilteredUnit);
      }
      if (backend.dlBrdfLutLoc >= 0) {
        dev->set_uniform_int(backend.dlBrdfLutLoc, kIblBrdfLutUnit);
      }
      const bool dlIblEnabled = iblAvailable &&
                                (backend.dlIblEnabledLoc >= 0) &&
                                (dev->bind_texture_cubemap != nullptr);
      if (backend.dlIblEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlIblEnabledLoc, dlIblEnabled ? 1 : 0);
      }
      if (dlIblEnabled) {
        dev->bind_texture_cubemap(kIblIrradianceUnit, iblIrradianceTex);
        dev->bind_texture_cubemap(kIblPrefilteredUnit, iblPrefilteredTex);
        dev->bind_texture(kIblBrdfLutUnit, backend.brdfLutTexture);
        if (backend.dlPrefilteredMipsLoc >= 0) {
          dev->set_uniform_float(
              backend.dlPrefilteredMipsLoc,
              static_cast<float>(backend.prefilteredEnvironmentMipLevels));
        }
      }

      if (backend.dlSsaoTextureLoc >= 0)
        dev->set_uniform_int(backend.dlSsaoTextureLoc, 5);
      if (backend.dlSsaoEnabledLoc >= 0)
        dev->set_uniform_int(backend.dlSsaoEnabledLoc, ssaoEnabled ? 1 : 0);

      if (shadowEnabled) {
        for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
          const int texUnit = 6 + static_cast<int>(c);
          dev->bind_texture(texUnit, backend.shadowState.depthTextures[c]);
          if (backend.dlShadowMapLocs[c] >= 0) {
            dev->set_uniform_int(backend.dlShadowMapLocs[c], texUnit);
          }
          if (backend.dlShadowMatrixLocs[c] >= 0) {
            dev->set_uniform_mat4(backend.dlShadowMatrixLocs[c],
                                  &backend.shadowState.cascades[c]
                                       .lightViewProjection.columns[0]
                                       .x);
          }
          if (backend.dlCascadeSplitLocs[c] >= 0) {
            dev->set_uniform_float(
                backend.dlCascadeSplitLocs[c],
                backend.shadowState.cascades[c].splitDistance);
          }
        }
      }
      if (backend.dlShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlShadowEnabledLoc, shadowEnabled ? 1 : 0);
      }

      const bool spotShadowEnabled = doSpotShadows;
      if (spotShadowEnabled) {
        for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
          const auto &slot = backend.spotShadowState.slots[s];
          const int texUnit = 10 + static_cast<int>(s);
          dev->bind_texture(texUnit, slot.depthTexture);
          if (backend.dlSpotShadowMapLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlSpotShadowMapLocs[s], texUnit);
          }
          if (backend.dlSpotShadowMatrixLocs[s] >= 0) {
            dev->set_uniform_mat4(backend.dlSpotShadowMatrixLocs[s],
                                  &slot.lightViewProjection.columns[0].x);
          }
          if (backend.dlSpotShadowLightIdxLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlSpotShadowLightIdxLocs[s],
                                 slot.lightIndex);
          }
        }
      }
      if (backend.dlSpotShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlSpotShadowEnabledLoc,
                             spotShadowEnabled ? 1 : 0);
      }

      // Bind point shadow cubemaps on texture units 14-17. The samplerCube
      // uniforms must point at their units even when point shadows are off:
      // left at the default unit 0 they alias the sampler2D G-buffer binding,
      // which makes the whole draw GL_INVALID_OPERATION on conformant
      // drivers.
      const bool pointShadowEnabled = doPointShadows;
      for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
        const int texUnit = 14 + static_cast<int>(s);
        if (backend.dlPointShadowMapLocs[s] >= 0) {
          dev->set_uniform_int(backend.dlPointShadowMapLocs[s], texUnit);
        }
      }
      if (pointShadowEnabled) {
        for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
          const auto &slot = backend.pointShadowState.slots[s];
          const int texUnit = 14 + static_cast<int>(s);
          if (dev->bind_texture_cubemap != nullptr) {
            dev->bind_texture_cubemap(texUnit, slot.depthCubemap);
          }
          if (backend.dlPointShadowLightPosLocs[s] >= 0) {
            const auto &lp = lights
                                 .pointLights[static_cast<std::size_t>(
                                     std::max(slot.lightIndex, 0))]
                                 .position;
            dev->set_uniform_vec3(backend.dlPointShadowLightPosLocs[s], &lp.x);
          }
          if (backend.dlPointShadowFarPlaneLocs[s] >= 0) {
            dev->set_uniform_float(backend.dlPointShadowFarPlaneLocs[s],
                                   slot.farPlane);
          }
          if (backend.dlPointShadowLightIdxLocs[s] >= 0) {
            dev->set_uniform_int(backend.dlPointShadowLightIdxLocs[s],
                                 slot.lightIndex);
          }
        }
      }
      if (backend.dlPointShadowEnabledLoc >= 0) {
        dev->set_uniform_int(backend.dlPointShadowEnabledLoc,
                             pointShadowEnabled ? 1 : 0);
      }

      if (backend.dlTileCountXLoc >= 0)
        dev->set_uniform_int(backend.dlTileCountXLoc, tileData.tileCountX);
      if (backend.dlTileCountYLoc >= 0)
        dev->set_uniform_int(backend.dlTileCountYLoc, tileData.tileCountY);

      math::Mat4 invProj{};
      if (math::inverse(projMat, &invProj)) {
        if (backend.dlInvProjectionLoc >= 0)
          dev->set_uniform_mat4(backend.dlInvProjectionLoc,
                                &invProj.columns[0].x);
      }
      math::Mat4 invView{};
      if (math::inverse(viewMat, &invView)) {
        if (backend.dlInvViewLoc >= 0)
          dev->set_uniform_mat4(backend.dlInvViewLoc, &invView.columns[0].x);
      }

      // Directional light (use first if available). Always upload: the
      // shader evaluates the light unconditionally, so a zero-light scene
      // must overwrite stale values with a black color and a valid (unit)
      // direction — a zero direction would NaN inside normalize().
      {
        const bool hasDirLight = lights.directionalLightCount > 0U;
        const math::Vec3 kNoLightDir(0.0F, -1.0F, 0.0F);
        const math::Vec3 kNoLightColor(0.0F, 0.0F, 0.0F);
        if (backend.dlDirLightDirLoc >= 0) {
          const math::Vec3 &dir = hasDirLight
                                      ? lights.directionalLights[0].direction
                                      : kNoLightDir;
          dev->set_uniform_vec3(backend.dlDirLightDirLoc, &dir.x);
        }
        if (backend.dlDirLightColorLoc >= 0) {
          const math::Vec3 &color = hasDirLight
                                        ? lights.directionalLights[0].color
                                        : kNoLightColor;
          dev->set_uniform_vec3(backend.dlDirLightColorLoc, &color.x);
        }
      }

      if (backend.dlCameraPosLoc >= 0) {
        dev->set_uniform_vec3(backend.dlCameraPosLoc,
                              &renderer_context().activeCamera.position.x);
      }
      if (backend.dlScreenSizeLoc >= 0) {
        const float screenSize[2] = {static_cast<float>(drawableWidth),
                                     static_cast<float>(drawableHeight)};
        dev->set_uniform_vec2(backend.dlScreenSizeLoc, screenSize);
      }
      upload_deferred_distance_fog_uniforms(backend, dev, fogSettings);
      upload_deferred_height_fog_uniforms(backend, dev, heightFogSettings);

      const auto plCount = static_cast<int>(std::min(
          lights.pointLightCount, static_cast<std::size_t>(kMaxPointLights)));
      if (backend.dlPointLightCountLoc >= 0)
        dev->set_uniform_int(backend.dlPointLightCountLoc, plCount);
      const auto slCount = static_cast<int>(std::min(
          lights.spotLightCount, static_cast<std::size_t>(kMaxSpotLights)));
      if (backend.dlSpotLightCountLoc >= 0)
        dev->set_uniform_int(backend.dlSpotLightCountLoc, slCount);

      dev->bind_vertex_array(backend.emptyVao);
      dev->draw_arrays_triangles(0, 3);

      dev->bind_texture(0, 0U);
      dev->bind_texture(1, 0U);
      dev->bind_texture(2, 0U);
      dev->bind_texture(3, 0U);
      dev->bind_texture(4, 0U);
      dev->bind_texture(18, 0U);
      if (dlIblEnabled) {
        dev->bind_texture_cubemap(kIblIrradianceUnit, 0U);
        dev->bind_texture_cubemap(kIblPrefilteredUnit, 0U);
        dev->bind_texture(kIblBrdfLutUnit, 0U);
      }
      if (ssaoEnabled) {
        dev->bind_texture(5, 0U);
      }
      if (shadowEnabled) {
        for (int c = 0; c < static_cast<int>(kShadowCascadeCount); ++c) {
          dev->bind_texture(6 + c, 0U);
        }
      }
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
      gpu_profiler_end_pass(GpuPassId::DeferredLighting);
    }

    const SkyModel skyModel = selected_sky_model();
    const std::uint32_t skyboxTexture = envSkyboxTexture;
    if (skyboxTexture != 0U) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_skybox(backend, dev, viewMat, projMat, skyboxTexture, frameStats);
      }
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_hosek_sky(backend, dev, viewMat, projMat, lights, frameStats);
      }
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_preetham_sky(backend, dev, viewMat, projMat, lights, frameStats);
      }
    }

    if (opaqueCount < totalCount) {
      const std::uint32_t sceneFbo =
          pass_resource_framebuffer(passRes.sceneColor);
      dev->bind_framebuffer(sceneFbo);
      dev->enable_depth_test();

      // Carry opaque deferred depth into the scene FBO so forward transparent
      // draws depth-test against G-Buffer geometry.
      static_cast<void>(ensureSceneDepthHasOpaque());
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
      bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled,
                               doSpotShadows, doPointShadows);
      if (backend.pbrAlbedoMapLocation >= 0)
        dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);

      const math::Mat4 &vp = viewProjection;

      auto drawForwardTransparent = [&](std::size_t start, std::size_t end) {
        std::uint32_t boundVA = 0U;
        std::uint32_t boundAlbedoTex = 0U;
        for (std::size_t i = start; i < end; ++i) {
          const DrawCommand &cmd = commandBufferView.data[i];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
              (mesh->vertexCount == 0U))
            continue;
          if (mesh->vertexArray != boundVA) {
            dev->bind_vertex_array(mesh->vertexArray);
            boundVA = mesh->vertexArray;
          }
          if (backend.pbrAlbedoLocation >= 0)
            dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                                  &cmd.material.albedo.x);
          if (backend.pbrRoughnessLocation >= 0)
            dev->set_uniform_float(backend.pbrRoughnessLocation,
                                   cmd.material.roughness);
          if (backend.pbrMetallicLocation >= 0)
            dev->set_uniform_float(backend.pbrMetallicLocation,
                                   cmd.material.metallic);
          if (backend.pbrOpacityLocation >= 0)
            dev->set_uniform_float(backend.pbrOpacityLocation,
                                   cmd.material.opacity);
          upload_pbr_foliage_uniforms(backend, dev, cmd);
          const std::uint32_t albedoGpu =
              texture_gpu_id(cmd.material.albedoTexture);
          const bool hasTex =
              (cmd.material.albedoTexture != kInvalidTextureHandle) &&
              (albedoGpu != 0U);
          if (backend.pbrHasAlbedoTextureLocation >= 0)
            dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                                 hasTex ? 1 : 0);
          if (hasTex && albedoGpu != boundAlbedoTex) {
            dev->bind_texture(0, albedoGpu);
            boundAlbedoTex = albedoGpu;
          } else if (!hasTex && boundAlbedoTex != 0U) {
            dev->bind_texture(0, 0U);
            boundAlbedoTex = 0U;
          }
          const math::Mat4 model = compute_model_matrix(cmd);
          const math::Mat4 mvp = compute_mvp(model, vp);
          float nm[9] = {};
          extract_normal_matrix(model, nm);
          if (backend.pbrModelLocation >= 0)
            dev->set_uniform_mat4(backend.pbrModelLocation,
                                  &model.columns[0].x);
          dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
          dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, nm);
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

      dev->set_depth_mask(false);
      dev->enable_blending();
      dev->set_blend_func_alpha();
      dev->disable_face_culling();
      drawForwardTransparent(opaqueCount, totalCount);
      dev->set_depth_mask(true);
      dev->disable_blending();
      dev->enable_face_culling();
      dev->bind_texture(0, 0U);
      unbind_pbr_shadow_textures(dev);
      unbind_pbr_ibl_textures(dev);
      dev->bind_vertex_array(0U);
      dev->bind_program(0U);
    }

    frameStats.gpuGBufferMs = gpu_profiler_pass_ms(GpuPassId::GBuffer);
    frameStats.gpuDeferredLightMs =
        gpu_profiler_pass_ms(GpuPassId::DeferredLighting);
    frameStats.gpuSsaoMs = gpu_profiler_pass_ms(GpuPassId::SSAO);
}

} // namespace engine::renderer
