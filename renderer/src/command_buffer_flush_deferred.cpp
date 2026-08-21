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

namespace {

/// R32F data texture (tile/per-light lookup tables): exact texel fetches,
/// so nearest filtering and edge clamping.
DeviceTextureHandle create_r32f_data_texture(const RenderDevice *dev,
                                             std::int32_t width,
                                             std::int32_t height,
                                             const float *data) noexcept {
  if ((dev == nullptr) || (dev->create_texture == nullptr)) {
    return kInvalidDeviceTexture;
  }
  TextureDesc desc{};
  desc.kind = TextureKind::Tex2D;
  desc.format = TextureFormat::R32F;
  desc.width = width;
  desc.height = height;
  desc.filter = TextureFilter::Nearest;
  desc.wrap = TextureWrap::ClampEdge;
  desc.pixelData = TexelData::F32;
  desc.pixels = data;
  return dev->create_texture(desc);
}

} // namespace

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
  const DeviceTextureHandle iblPrefilteredTex = ctx.iblPrefilteredTex;
  const DeviceTextureHandle iblIrradianceTex = ctx.iblIrradianceTex;
  const DeviceTextureHandle envSkyboxTexture = ctx.envSkyboxTexture;
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
      if (dev->copy_depth == nullptr) {
        return false;
      }

      const RenderTargetHandle gbufferTarget =
          pass_resource_target(passRes.gbufferAlbedo);
      const RenderTargetHandle sceneTarget =
          pass_resource_target(passRes.sceneColor);
      dev->copy_depth(gbufferTarget, sceneTarget, drawableWidth,
                      drawableHeight);
      dev->bind_render_target(sceneTarget);
      sceneDepthHasOpaque = true;
      return true;
    };

    gpu_profiler_begin_pass(GpuPassId::GBuffer);
    const RenderTargetHandle gbufferTarget =
        pass_resource_target(passRes.gbufferAlbedo);
    dev->bind_render_target(gbufferTarget);
    dev->set_viewport(0, 0, drawableWidth, drawableHeight);
    dev->apply_render_state(RenderState{DepthTest::Less, true,
                                        BlendMode::Disabled, CullMode::Back});
    dev->clear(ClearFlags::ColorDepth, 0.0F, 0.0F, 0.0F, 0.0F);

    dev->bind_program(backend.gbufferProgram);

    if (backend.gbufViewLoc.valid()) {
      dev->set_param_mat4(backend.gbufViewLoc, &viewMat.columns[0].x);
    }
    if (backend.gbufProjectionLoc.valid()) {
      dev->set_param_mat4(backend.gbufProjectionLoc, &projMat.columns[0].x);
    }
    if (backend.gbufTimeLoc.valid()) {
      dev->set_param_f32(backend.gbufTimeLoc, timeSeconds);
    }

    auto drawGBufferBatches = [&]() {
      DeviceTextureHandle boundAlbedoTex{};
      DeviceTextureHandle boundMaterialTex[4] = {};
      if (backend.gbufAlbedoTextureLoc.valid()) {
        dev->set_param_i32(backend.gbufAlbedoTextureLoc, 0);
      }
      const MaterialTextureUniformLocs materialTexLocs{
          backend.gbufHasMetallicRoughnessTextureLoc,
          backend.gbufMetallicRoughnessTextureLoc,
          backend.gbufHasEmissiveTextureLoc,
          backend.gbufEmissiveTextureLoc,
          backend.gbufHasOcclusionTextureLoc,
          backend.gbufOcclusionTextureLoc,
          backend.gbufHasOpacityTextureLoc,
          backend.gbufOpacityTextureLoc,
          backend.gbufAlphaModeLoc,
          backend.gbufAlphaCutoffLoc,
          backend.gbufUvTilingLoc,
          backend.gbufUvOffsetLoc};
      for (std::size_t batchIndex = 0U; batchIndex < opaqueBatchCount;
           ++batchIndex) {
        const StaticMeshBatch &batch = backend.staticMeshBatches[batchIndex];
        const DrawCommand &command = commandBufferView.data[batch.first];
        const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
        if ((mesh == nullptr) || (mesh->geometry == kInvalidDeviceGeometry) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }

        if (backend.gbufAlbedoLoc.valid()) {
          dev->set_param_vec3(backend.gbufAlbedoLoc,
                                &command.material.albedo.x);
        }
        if (backend.gbufMetallicLoc.valid()) {
          dev->set_param_f32(backend.gbufMetallicLoc,
                                 command.material.metallic);
        }
        if (backend.gbufRoughnessLoc.valid()) {
          dev->set_param_f32(backend.gbufRoughnessLoc,
                                 command.material.roughness);
        }
        if (backend.gbufAOLoc.valid()) {
          dev->set_param_f32(backend.gbufAOLoc, 1.0F);
        }
        if (backend.gbufEmissiveLoc.valid()) {
          dev->set_param_vec3(backend.gbufEmissiveLoc,
                                &command.material.emissive.x);
        }
        const DeviceTextureHandle albedoTex =
            texture_device_handle(command.material.albedoTexture);
        const bool hasAlbedoTex =
            (command.material.albedoTexture != kInvalidTextureHandle) &&
            (albedoTex != kInvalidDeviceTexture);
        if (backend.gbufHasAlbedoTextureLoc.valid()) {
          dev->set_param_i32(backend.gbufHasAlbedoTextureLoc,
                               hasAlbedoTex ? 1 : 0);
        }
        if (hasAlbedoTex && (albedoTex != boundAlbedoTex)) {
          dev->bind_texture_slot(0U, albedoTex);
          boundAlbedoTex = albedoTex;
        } else if (!hasAlbedoTex && (boundAlbedoTex != kInvalidDeviceTexture)) {
          dev->bind_texture_slot(0U, kInvalidDeviceTexture);
          boundAlbedoTex = kInvalidDeviceTexture;
        }
        upload_material_texture_slots(materialTexLocs, dev, command.material,
                                      boundMaterialTex);
        upload_gbuffer_foliage_uniforms(backend, dev, command);

        if ((batch.count > 1U) && !mesh->hasSkin && (mesh->indexCount > 0U) &&
            upload_instance_matrices(backend, dev, *mesh, commandBufferView,
                                     batch)) {
          if (backend.gbufUseInstancingLoc.valid()) {
            dev->set_param_i32(backend.gbufUseInstancingLoc, 1);
          }
          ++frameStats.drawCalls;
          frameStats.triangleCount +=
              (mesh->indexCount / 3U) * static_cast<std::uint64_t>(batch.count);
          dev->draw_indexed_instanced(
              mesh->geometry, static_cast<std::int32_t>(mesh->indexCount),
              static_cast<std::int32_t>(batch.count));
          continue;
        }

        if (backend.gbufUseInstancingLoc.valid()) {
          dev->set_param_i32(backend.gbufUseInstancingLoc, 0);
        }
        for (std::uint32_t local = 0U; local < batch.count; ++local) {
          const std::size_t commandIndex =
              static_cast<std::size_t>(batch.first) +
              static_cast<std::size_t>(local);
          const DrawCommand &singleCommand = commandBufferView.data[commandIndex];
          const math::Mat4 model = compute_model_matrix(singleCommand);
          float normalMatrix[9] = {};
          extract_normal_matrix(model, normalMatrix);

          const bool skinnedDraw =
              mesh->hasSkin &&
              (singleCommand.skinPalette != kInvalidSkinPalette) &&
              upload_bone_palette(backend, dev, singleCommand.skinPalette);
          if (skinnedDraw) {
            dev->bind_program(backend.gbufferSkinnedProgram);
            upload_skinned_gbuffer_uniforms(backend, dev, viewMat, projMat,
                                            timeSeconds, singleCommand, model,
                                            normalMatrix, &boundAlbedoTex,
                                            boundMaterialTex);
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
            dev->bind_program(backend.gbufferProgram);
            continue;
          }

          upload_gbuffer_foliage_uniforms(backend, dev, singleCommand);
          if (backend.gbufModelLoc.valid()) {
            dev->set_param_mat4(backend.gbufModelLoc, &model.columns[0].x);
          }
          if (backend.gbufNormalMatrixLoc.valid()) {
            dev->set_param_mat3(backend.gbufNormalMatrixLoc, normalMatrix);
          }

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
      }
    };

    drawGBufferBatches();

    dev->bind_program(kInvalidDeviceProgram);
    gpu_profiler_end_pass(GpuPassId::GBuffer);

    const bool ssaoEnabled =
        backend.ssaoAvailable && core::cvar_get_bool("r_ssao", true);
    if (ssaoEnabled) {
      gpu_profiler_begin_pass(GpuPassId::SSAO);
      dev->bind_render_target(pass_resource_target(passRes.ssaoTexture));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->apply_render_state(RenderState{DepthTest::Disabled, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});

      dev->bind_program(backend.ssaoProgram);

      dev->bind_texture_slot(0U, pass_resource_texture(passRes.gbufferDepth));
      dev->bind_texture_slot(1U, pass_resource_texture(passRes.gbufferNormal));
      dev->bind_texture_slot(2U, backend.ssaoNoiseTexture);

      if (backend.ssaoDepthLoc.valid())
        dev->set_param_i32(backend.ssaoDepthLoc, 0);
      if (backend.ssaoNormalLoc.valid())
        dev->set_param_i32(backend.ssaoNormalLoc, 1);
      if (backend.ssaoNoiseLoc.valid())
        dev->set_param_i32(backend.ssaoNoiseLoc, 2);

      if (backend.ssaoProjectionLoc.valid())
        dev->set_param_mat4(backend.ssaoProjectionLoc, &projMat.columns[0].x);
      if (backend.ssaoViewLoc.valid())
        dev->set_param_mat4(backend.ssaoViewLoc, &viewMat.columns[0].x);

      if (backend.ssaoNoiseScaleLoc.valid()) {
        const float noiseScale[2] = {static_cast<float>(drawableWidth) / 4.0F,
                                     static_cast<float>(drawableHeight) / 4.0F};
        dev->set_param_vec2(backend.ssaoNoiseScaleLoc, noiseScale);
      }
      if (backend.ssaoRadiusLoc.valid())
        dev->set_param_f32(backend.ssaoRadiusLoc,
                               core::cvar_get_float("r_ssao_radius"));
      if (backend.ssaoBiasLoc.valid())
        dev->set_param_f32(backend.ssaoBiasLoc,
                               core::cvar_get_float("r_ssao_bias"));

      for (int i = 0; i < 32; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (backend.ssaoSampleLocs[idx].valid()) {
          dev->set_param_vec3(backend.ssaoSampleLocs[idx],
                                &backend.ssaoKernel[i * 3]);
        }
      }

      dev->draw(backend.emptyGeometry, PrimitiveTopology::Triangles, 0, 3);

      dev->bind_texture_slot(0U, kInvalidDeviceTexture);
      dev->bind_texture_slot(1U, kInvalidDeviceTexture);
      dev->bind_texture_slot(2U, kInvalidDeviceTexture);
      dev->bind_program(kInvalidDeviceProgram);

      dev->bind_render_target(pass_resource_target(passRes.ssaoBlurTexture));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);

      dev->bind_program(backend.ssaoBlurProgram);

      dev->bind_texture_slot(0U, pass_resource_texture(passRes.ssaoTexture));
      if (backend.ssaoBlurInputLoc.valid())
        dev->set_param_i32(backend.ssaoBlurInputLoc, 0);
      if (backend.ssaoBlurTexelSizeLoc.valid()) {
        const float texelSize[2] = {1.0F / static_cast<float>(drawableWidth),
                                    1.0F / static_cast<float>(drawableHeight)};
        dev->set_param_vec2(backend.ssaoBlurTexelSizeLoc, texelSize);
      }

      dev->draw(backend.emptyGeometry, PrimitiveTopology::Triangles, 0, 3);

      dev->bind_texture_slot(0U, kInvalidDeviceTexture);
      dev->bind_program(kInvalidDeviceProgram);
      gpu_profiler_end_pass(GpuPassId::SSAO);
    }

    const std::size_t tileBufferSize =
        compute_tile_buffer_size(drawableWidth, drawableHeight);
    if (backend.tileBuffer.size() < tileBufferSize) {
      // A failed grow leaves the buffer at zero capacity (audit #204:
      // nothrow instead of a terminating std::vector throw); the
      // dataSize < requiredSize check inside cull_lights_tiled below
      // already treats an undersized buffer as a graceful cull failure.
      static_cast<void>(backend.tileBuffer.allocate(tileBufferSize));
    }

    TileLightData tileData{};
    tileData.data = backend.tileBuffer.data();
    tileData.dataSize = backend.tileBuffer.size();

    const bool tileDataValid =
        cull_lights_tiled(lights, &viewMat.columns[0].x, &projMat.columns[0].x,
                          drawableWidth, drawableHeight, tileData);
    if (!tileDataValid) {
      static bool warnedCullFailure = false;
      if (!warnedCullFailure) {
        core::log_message(core::LogLevel::Warning, "renderer",
                          "tiled light culling failed; deferred lighting "
                          "renders without local lights");
        warnedCullFailure = true;
      }
      if (backend.tileLightTex != kInvalidDeviceTexture) {
        dev->destroy_texture(backend.tileLightTex);
        backend.tileLightTex = kInvalidDeviceTexture;
        backend.tileLightTexRows = 0;
      }
    } else {
      if ((backend.tileLightTex != kInvalidDeviceTexture) &&
          (tileData.totalTiles > backend.tileLightTexRows)) {
        dev->destroy_texture(backend.tileLightTex);
        backend.tileLightTex = kInvalidDeviceTexture;
        backend.tileLightTexRows = 0;
      }
      if (backend.tileLightTex == kInvalidDeviceTexture) {
        backend.tileLightTex = create_r32f_data_texture(
            dev, kTileDataWidth, tileData.totalTiles,
            backend.tileBuffer.data());
        if (backend.tileLightTex != kInvalidDeviceTexture) {
          backend.tileLightTexRows = tileData.totalTiles;
        } else {
          static bool warnedTileTexFailure = false;
          if (!warnedTileTexFailure) {
            core::log_message(core::LogLevel::Warning, "renderer",
                              "tile light texture creation failed; deferred "
                              "lighting renders without local lights");
            warnedTileTexFailure = true;
          }
        }
      } else {
        dev->update_texture(backend.tileLightTex, backend.tileBuffer.data(),
                            kTileDataWidth, tileData.totalTiles);
      }
    }

    if (!pack_light_data(lights, backend.lightDataBuffer.data(),
                         backend.lightDataBuffer.size())) {
      static bool warnedPackFailure = false;
      if (!warnedPackFailure) {
        core::log_message(core::LogLevel::Warning, "renderer",
                          "light data packing failed; per-light texture "
                          "keeps its previous contents");
        warnedPackFailure = true;
      }
    } else if (backend.lightDataTex == kInvalidDeviceTexture) {
      backend.lightDataTex = create_r32f_data_texture(
          dev, kLightDataTexWidth, kLightDataTexHeight,
          backend.lightDataBuffer.data());
    } else {
      dev->update_texture(backend.lightDataTex,
                          backend.lightDataBuffer.data(), kLightDataTexWidth,
                          kLightDataTexHeight);
    }

    if (gbufferDebugMode > 0 &&
        backend.gbufferDebugProgram != kInvalidDeviceProgram) {
      gpu_profiler_begin_pass(GpuPassId::GBufferDebug);
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->apply_render_state(RenderState{DepthTest::Disabled, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});

      dev->bind_program(backend.gbufferDebugProgram);

      dev->bind_texture_slot(0U, pass_resource_texture(passRes.gbufferAlbedo));
      dev->bind_texture_slot(1U, pass_resource_texture(passRes.gbufferNormal));
      dev->bind_texture_slot(2U,
                             pass_resource_texture(passRes.gbufferEmissive));
      dev->bind_texture_slot(3U, pass_resource_texture(passRes.gbufferDepth));

      if (backend.dbgGBufAlbedoLoc.valid())
        dev->set_param_i32(backend.dbgGBufAlbedoLoc, 0);
      if (backend.dbgGBufNormalLoc.valid())
        dev->set_param_i32(backend.dbgGBufNormalLoc, 1);
      if (backend.dbgGBufEmissiveLoc.valid())
        dev->set_param_i32(backend.dbgGBufEmissiveLoc, 2);
      if (backend.dbgGBufDepthLoc.valid())
        dev->set_param_i32(backend.dbgGBufDepthLoc, 3);
      // Debug mode:
      // 0=albedo,1=normals,2=metallic,3=roughness,4=emissive,5=AO,6=depth CVar
      // value 1..7 maps to shader 0..6.
      if (backend.dbgModeLoc.valid())
        dev->set_param_i32(backend.dbgModeLoc, gbufferDebugMode - 1);

      dev->draw(backend.emptyGeometry, PrimitiveTopology::Triangles, 0, 3);

      dev->bind_texture_slot(0U, kInvalidDeviceTexture);
      dev->bind_texture_slot(1U, kInvalidDeviceTexture);
      dev->bind_texture_slot(2U, kInvalidDeviceTexture);
      dev->bind_texture_slot(3U, kInvalidDeviceTexture);
      dev->bind_program(kInvalidDeviceProgram);
      gpu_profiler_end_pass(GpuPassId::GBufferDebug);
    } else {
      gpu_profiler_begin_pass(GpuPassId::DeferredLighting);
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      dev->apply_render_state(RenderState{DepthTest::Disabled, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});

      dev->bind_program(backend.deferredLightProgram);

      // Bind G-Buffer textures on slots 0-3, tile on slot 4, SSAO on slot
      // 5, per-light data on slot 18 (slots 6-17 hold shadow maps).
      dev->bind_texture_slot(0U, pass_resource_texture(passRes.gbufferAlbedo));
      dev->bind_texture_slot(1U, pass_resource_texture(passRes.gbufferNormal));
      dev->bind_texture_slot(2U,
                             pass_resource_texture(passRes.gbufferEmissive));
      dev->bind_texture_slot(3U, pass_resource_texture(passRes.gbufferDepth));
      dev->bind_texture_slot(4U, backend.tileLightTex);
      dev->bind_texture_slot(18U, backend.lightDataTex);

      if (ssaoEnabled) {
        dev->bind_texture_slot(5U,
                               pass_resource_texture(passRes.ssaoBlurTexture));
      }

      if (backend.dlGBufAlbedoLoc.valid())
        dev->set_param_i32(backend.dlGBufAlbedoLoc, 0);
      if (backend.dlGBufNormalLoc.valid())
        dev->set_param_i32(backend.dlGBufNormalLoc, 1);
      if (backend.dlGBufEmissiveLoc.valid())
        dev->set_param_i32(backend.dlGBufEmissiveLoc, 2);
      if (backend.dlGBufDepthLoc.valid())
        dev->set_param_i32(backend.dlGBufDepthLoc, 3);
      if (backend.dlTileLightTexLoc.valid())
        dev->set_param_i32(backend.dlTileLightTexLoc, 4);
      if (backend.dlLightDataTexLoc.valid())
        dev->set_param_i32(backend.dlLightDataTexLoc, 18);

      // Sampler units are assigned even when IBL is off: a samplerCube
      // uniform left at its default unit 0 aliases the sampler2D G-buffer
      // there, which is a draw-time GL_INVALID_OPERATION that corrupts
      // every deferred draw.
      if (backend.dlIrradianceMapLoc.valid()) {
        dev->set_param_i32(backend.dlIrradianceMapLoc, kIblIrradianceUnit);
      }
      if (backend.dlPrefilteredMapLoc.valid()) {
        dev->set_param_i32(backend.dlPrefilteredMapLoc, kIblPrefilteredUnit);
      }
      if (backend.dlBrdfLutLoc.valid()) {
        dev->set_param_i32(backend.dlBrdfLutLoc, kIblBrdfLutUnit);
      }
      const bool dlIblEnabled = iblAvailable &&
                                (backend.dlIblEnabledLoc.valid()) &&
                                (dev->bind_texture_slot != nullptr);
      if (backend.dlIblEnabledLoc.valid()) {
        dev->set_param_i32(backend.dlIblEnabledLoc, dlIblEnabled ? 1 : 0);
      }
      if (dlIblEnabled) {
        dev->bind_texture_slot(kIblIrradianceUnit, iblIrradianceTex);
        dev->bind_texture_slot(kIblPrefilteredUnit, iblPrefilteredTex);
        dev->bind_texture_slot(kIblBrdfLutUnit, backend.brdfLutTexture);
        if (backend.dlPrefilteredMipsLoc.valid()) {
          dev->set_param_f32(
              backend.dlPrefilteredMipsLoc,
              static_cast<float>(backend.prefilteredEnvironmentMipLevels));
        }
      }

      if (backend.dlSsaoTextureLoc.valid())
        dev->set_param_i32(backend.dlSsaoTextureLoc, 5);
      if (backend.dlSsaoEnabledLoc.valid())
        dev->set_param_i32(backend.dlSsaoEnabledLoc, ssaoEnabled ? 1 : 0);

      // #138 flat vocabulary: per-slot samplers, one mat4 array per
      // shadow kind, splits/indices/pos+far as packed vec4 payloads.
      if (shadowEnabled) {
        float shadowMatrices[kShadowCascadeCount * 16U] = {};
        float cascadeSplits[4] = {};
        for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
          const auto texUnit = static_cast<std::uint32_t>(6U + c);
          dev->bind_texture_slot(texUnit, backend.shadowState.depthTextures[c]);
          if (backend.dlShadowMapLocs[c].valid()) {
            dev->set_param_i32(backend.dlShadowMapLocs[c],
                               static_cast<std::int32_t>(texUnit));
          }
          std::memcpy(&shadowMatrices[c * 16U],
                      &backend.shadowState.cascades[c]
                           .lightViewProjection.columns[0]
                           .x,
                      sizeof(float) * 16U);
          cascadeSplits[c] = backend.shadowState.cascades[c].splitDistance;
        }
        if ((dev->set_param_mat4_array != nullptr) &&
            backend.dlShadowMatrixParam.valid()) {
          dev->set_param_mat4_array(
              backend.dlShadowMatrixParam, shadowMatrices,
              static_cast<std::int32_t>(kShadowCascadeCount));
        }
        if (backend.dlCascadeSplitsParam.valid()) {
          dev->set_param_vec4(backend.dlCascadeSplitsParam, cascadeSplits);
        }
      }
      if (backend.dlShadowEnabledLoc.valid()) {
        dev->set_param_i32(backend.dlShadowEnabledLoc, shadowEnabled ? 1 : 0);
      }

      const bool spotShadowEnabled = doSpotShadows;
      if (spotShadowEnabled) {
        float spotMatrices[kMaxSpotShadowLights * 16U] = {};
        float spotLightIdx[4] = {};
        for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
          const auto &slot = backend.spotShadowState.slots[s];
          const auto texUnit = static_cast<std::uint32_t>(10U + s);
          dev->bind_texture_slot(texUnit, slot.depthTexture);
          if (backend.dlSpotShadowMapLocs[s].valid()) {
            dev->set_param_i32(backend.dlSpotShadowMapLocs[s],
                               static_cast<std::int32_t>(texUnit));
          }
          std::memcpy(&spotMatrices[s * 16U],
                      &slot.lightViewProjection.columns[0].x,
                      sizeof(float) * 16U);
          spotLightIdx[s] = static_cast<float>(slot.lightIndex);
        }
        if ((dev->set_param_mat4_array != nullptr) &&
            backend.dlSpotShadowMatrixParam.valid()) {
          dev->set_param_mat4_array(
              backend.dlSpotShadowMatrixParam, spotMatrices,
              static_cast<std::int32_t>(kMaxSpotShadowLights));
        }
        if (backend.dlSpotShadowLightIdxParam.valid()) {
          dev->set_param_vec4(backend.dlSpotShadowLightIdxParam,
                              spotLightIdx);
        }
      }
      if (backend.dlSpotShadowEnabledLoc.valid()) {
        dev->set_param_i32(backend.dlSpotShadowEnabledLoc,
                             spotShadowEnabled ? 1 : 0);
      }

      // Bind point shadow cubemaps on texture units 14-17. The samplerCube
      // uniforms must point at their units even when point shadows are off:
      // left at the default unit 0 they alias the sampler2D G-buffer binding,
      // which makes the whole draw GL_INVALID_OPERATION on conformant
      // drivers.
      const bool pointShadowEnabled = doPointShadows;
      for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
        if (backend.dlPointShadowMapLocs[s].valid()) {
          dev->set_param_i32(backend.dlPointShadowMapLocs[s],
                             static_cast<std::int32_t>(14U + s));
        }
      }
      if (pointShadowEnabled) {
        float pointPosFar[kMaxPointShadowLights * 4U] = {};
        float pointLightIdx[4] = {};
        for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
          const auto &slot = backend.pointShadowState.slots[s];
          const auto texUnit = static_cast<std::uint32_t>(14U + s);
          if (dev->bind_texture_slot != nullptr) {
            dev->bind_texture_slot(texUnit, slot.depthCubemap);
          }
          const math::Vec3 lp =
              point_shadow_slot_light_position(slot.lightIndex, lights);
          pointPosFar[s * 4U + 0U] = lp.x;
          pointPosFar[s * 4U + 1U] = lp.y;
          pointPosFar[s * 4U + 2U] = lp.z;
          pointPosFar[s * 4U + 3U] = slot.farPlane;
          pointLightIdx[s] = static_cast<float>(slot.lightIndex);
        }
        if ((dev->set_param_vec4_array != nullptr) &&
            backend.dlPointShadowPosFarParam.valid()) {
          dev->set_param_vec4_array(
              backend.dlPointShadowPosFarParam, pointPosFar,
              static_cast<std::int32_t>(kMaxPointShadowLights));
        }
        if (backend.dlPointShadowLightIdxParam.valid()) {
          dev->set_param_vec4(backend.dlPointShadowLightIdxParam,
                              pointLightIdx);
        }
      }
      if (backend.dlPointShadowEnabledLoc.valid()) {
        dev->set_param_i32(backend.dlPointShadowEnabledLoc,
                             pointShadowEnabled ? 1 : 0);
      }

      if (backend.dlTileCountXLoc.valid())
        dev->set_param_i32(backend.dlTileCountXLoc, tileData.tileCountX);
      if (backend.dlTileCountYLoc.valid())
        dev->set_param_i32(backend.dlTileCountYLoc, tileData.tileCountY);

      math::Mat4 invProj{};
      if (math::inverse(projMat, &invProj)) {
        if (backend.dlInvProjectionLoc.valid())
          dev->set_param_mat4(backend.dlInvProjectionLoc,
                                &invProj.columns[0].x);
      }
      math::Mat4 invView{};
      if (math::inverse(viewMat, &invView)) {
        if (backend.dlInvViewLoc.valid())
          dev->set_param_mat4(backend.dlInvViewLoc, &invView.columns[0].x);
      }

      // Directional light (use first if available). Always upload: the
      // shader evaluates the light unconditionally, so a zero-light scene
      // must overwrite stale values with a black color and a valid (unit)
      // direction — a zero direction would NaN inside normalize().
      {
        const bool hasDirLight = lights.directionalLightCount > 0U;
        const math::Vec3 kNoLightDir(0.0F, -1.0F, 0.0F);
        const math::Vec3 kNoLightColor(0.0F, 0.0F, 0.0F);
        if (backend.dlDirLightDirLoc.valid()) {
          const math::Vec3 &dir = hasDirLight
                                      ? lights.directionalLights[0].direction
                                      : kNoLightDir;
          dev->set_param_vec3(backend.dlDirLightDirLoc, &dir.x);
        }
        if (backend.dlDirLightColorLoc.valid()) {
          const math::Vec3 &color = hasDirLight
                                        ? lights.directionalLights[0].color
                                        : kNoLightColor;
          dev->set_param_vec3(backend.dlDirLightColorLoc, &color.x);
        }
      }

      if (backend.dlCameraPosLoc.valid()) {
        dev->set_param_vec3(backend.dlCameraPosLoc,
                              &renderer_context().activeCamera.position.x);
      }
      if (backend.dlCameraForwardOrthoLoc.valid()) {
        // xyz = normalized view direction, w = 1 when orthographic (#221):
        // the shader switches its view vector to the constant camera
        // forward under ortho — parallel rays have no per-pixel eye vector.
        const CameraState &activeCam = renderer_context().activeCamera;
        const math::Vec3 fwd = math::normalize(
            math::sub(activeCam.target, activeCam.position));
        const float forwardOrtho[4] = {
            fwd.x, fwd.y, fwd.z,
            (activeCam.projection == CameraState::kProjectionOrthographic)
                ? 1.0F
                : 0.0F};
        dev->set_param_vec4(backend.dlCameraForwardOrthoLoc, forwardOrtho);
      }
      if (backend.dlScreenSizeLoc.valid()) {
        const float screenSize[2] = {static_cast<float>(drawableWidth),
                                     static_cast<float>(drawableHeight)};
        dev->set_param_vec2(backend.dlScreenSizeLoc, screenSize);
      }
      upload_deferred_distance_fog_uniforms(backend, dev, fogSettings);
      upload_deferred_height_fog_uniforms(backend, dev, heightFogSettings);

      const auto plCount = static_cast<int>(std::min(
          lights.pointLightCount, static_cast<std::size_t>(kMaxPointLights)));
      if (backend.dlPointLightCountLoc.valid())
        dev->set_param_i32(backend.dlPointLightCountLoc, plCount);
      const auto slCount = static_cast<int>(std::min(
          lights.spotLightCount, static_cast<std::size_t>(kMaxSpotLights)));
      if (backend.dlSpotLightCountLoc.valid())
        dev->set_param_i32(backend.dlSpotLightCountLoc, slCount);

      dev->draw(backend.emptyGeometry, PrimitiveTopology::Triangles, 0, 3);

      for (std::uint32_t slot = 0U; slot <= 4U; ++slot) {
        dev->bind_texture_slot(slot, kInvalidDeviceTexture);
      }
      dev->bind_texture_slot(18U, kInvalidDeviceTexture);
      if (dlIblEnabled) {
        dev->bind_texture_slot(kIblIrradianceUnit, kInvalidDeviceTexture);
        dev->bind_texture_slot(kIblPrefilteredUnit, kInvalidDeviceTexture);
        dev->bind_texture_slot(kIblBrdfLutUnit, kInvalidDeviceTexture);
      }
      if (ssaoEnabled) {
        dev->bind_texture_slot(5U, kInvalidDeviceTexture);
      }
      if (shadowEnabled) {
        for (std::uint32_t c = 0U; c < kShadowCascadeCount; ++c) {
          dev->bind_texture_slot(6U + c, kInvalidDeviceTexture);
        }
      }
      dev->bind_program(kInvalidDeviceProgram);
      gpu_profiler_end_pass(GpuPassId::DeferredLighting);
    }

    const SkyModel skyModel = selected_sky_model();
    const DeviceTextureHandle skyboxTexture = envSkyboxTexture;
    const math::Mat4 skyProj = sky_projection_matrix(
        renderer_context().activeCamera,
        (drawableHeight > 0)
            ? (static_cast<float>(drawableWidth) /
               static_cast<float>(drawableHeight))
            : 1.0F);
    if (skyboxTexture != kInvalidDeviceTexture) {
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_skybox(backend, dev, viewMat, skyProj, skyboxTexture, frameStats);
      }
    } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_hosek_sky(backend, dev, viewMat, skyProj, lights, frameStats);
      }
    } else if (((skyModel == SkyModel::Preetham) ||
                (skyModel == SkyModel::Hosek)) &&
               backend.preethamSkyAvailable) {
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));
      dev->set_viewport(0, 0, drawableWidth, drawableHeight);
      if (ensureSceneDepthHasOpaque()) {
        draw_preetham_sky(backend, dev, viewMat, skyProj, lights, frameStats);
      }
    }

    if (opaqueCount < totalCount) {
      dev->bind_render_target(pass_resource_target(passRes.sceneColor));

      // Carry opaque deferred depth into the scene target so forward
      // transparent draws depth-test against G-Buffer geometry.
      static_cast<void>(ensureSceneDepthHasOpaque());
      dev->bind_program(backend.pbrProgram);

      if (backend.pbrTimeLocation.valid()) {
        dev->set_param_f32(backend.pbrTimeLocation, timeSeconds);
      }
      if (backend.pbrCameraPosLocation.valid()) {
        dev->set_param_vec3(backend.pbrCameraPosLocation,
                              &renderer_context().activeCamera.position.x);
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
      bind_pbr_shadow_uniforms(backend, dev, lights, shadowEnabled,
                               doSpotShadows, doPointShadows);
      if (backend.pbrAlbedoMapLocation.valid())
        dev->set_param_i32(backend.pbrAlbedoMapLocation, 0);

      const math::Mat4 &vp = viewProjection;
      const MaterialTextureUniformLocs transparentMaterialTexLocs{
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

      auto drawForwardTransparent = [&](std::size_t start, std::size_t end) {
        DeviceTextureHandle boundAlbedoTex{};
        DeviceTextureHandle boundMaterialTex[4] = {};
        for (std::size_t i = start; i < end; ++i) {
          const DrawCommand &cmd = commandBufferView.data[i];
          const GpuMesh *mesh = lookup_gpu_mesh(registry, cmd.mesh);
          if ((mesh == nullptr) ||
              (mesh->geometry == kInvalidDeviceGeometry) ||
              (mesh->vertexCount == 0U))
            continue;
          if (backend.pbrAlbedoLocation.valid())
            dev->set_param_vec3(backend.pbrAlbedoLocation,
                                  &cmd.material.albedo.x);
          if (backend.pbrRoughnessLocation.valid())
            dev->set_param_f32(backend.pbrRoughnessLocation,
                                   cmd.material.roughness);
          if (backend.pbrMetallicLocation.valid())
            dev->set_param_f32(backend.pbrMetallicLocation,
                                   cmd.material.metallic);
          if (backend.pbrOpacityLocation.valid())
            dev->set_param_f32(backend.pbrOpacityLocation,
                                   cmd.material.opacity);
          if (backend.pbrEmissiveLocation.valid())
            dev->set_param_vec3(backend.pbrEmissiveLocation,
                                  &cmd.material.emissive.x);
          upload_pbr_foliage_uniforms(backend, dev, cmd);
          const DeviceTextureHandle albedoTex =
              texture_device_handle(cmd.material.albedoTexture);
          const bool hasTex =
              (cmd.material.albedoTexture != kInvalidTextureHandle) &&
              (albedoTex != kInvalidDeviceTexture);
          if (backend.pbrHasAlbedoTextureLocation.valid())
            dev->set_param_i32(backend.pbrHasAlbedoTextureLocation,
                                 hasTex ? 1 : 0);
          if (hasTex && albedoTex != boundAlbedoTex) {
            dev->bind_texture_slot(0U, albedoTex);
            boundAlbedoTex = albedoTex;
          } else if (!hasTex && (boundAlbedoTex != kInvalidDeviceTexture)) {
            dev->bind_texture_slot(0U, kInvalidDeviceTexture);
            boundAlbedoTex = kInvalidDeviceTexture;
          }
          upload_material_texture_slots(transparentMaterialTexLocs, dev,
                                        cmd.material, boundMaterialTex);
          const math::Mat4 model = compute_model_matrix(cmd);
          const math::Mat4 mvp = compute_mvp(model, vp);
          float nm[9] = {};
          extract_normal_matrix(model, nm);
          if (backend.pbrModelLocation.valid())
            dev->set_param_mat4(backend.pbrModelLocation,
                                  &model.columns[0].x);
          dev->set_param_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
          dev->set_param_mat3(backend.pbrNormalMatrixLocation, nm);
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

      dev->apply_render_state(RenderState{DepthTest::Less, false,
                                          BlendMode::Alpha, CullMode::None});
      drawForwardTransparent(opaqueCount, totalCount);
      dev->apply_render_state(RenderState{DepthTest::Less, true,
                                          BlendMode::Disabled,
                                          CullMode::Back});
      dev->bind_texture_slot(0U, kInvalidDeviceTexture);
      unbind_pbr_shadow_textures(dev);
      unbind_pbr_ibl_textures(dev);
      dev->bind_program(kInvalidDeviceProgram);
    }

    frameStats.gpuGBufferMs = gpu_profiler_pass_ms(GpuPassId::GBuffer);
    frameStats.gpuDeferredLightMs =
        gpu_profiler_pass_ms(GpuPassId::DeferredLighting);
    frameStats.gpuSsaoMs = gpu_profiler_pass_ms(GpuPassId::SSAO);
}

} // namespace engine::renderer
