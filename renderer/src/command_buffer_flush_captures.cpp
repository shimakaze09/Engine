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

OffscreenSceneInputs offscreen_scene_inputs(FrameFlushContext &ctx) noexcept {
  OffscreenSceneInputs inputs{};
  inputs.backend = &ctx.backend;
  inputs.dev = ctx.dev;
  inputs.registry = ctx.registry;
  inputs.mainView = ctx.commandBufferView;
  inputs.mainOpaqueCount = ctx.opaqueCount;
  inputs.mainTotalCount = ctx.totalCount;
  inputs.auxiliaryView = ctx.auxiliaryView;
  inputs.auxiliaryOpaqueCount = ctx.auxiliaryOpaqueCount;
  inputs.lights = &ctx.lights;
  inputs.timeSeconds = ctx.timeSeconds;
  inputs.fogSettings = ctx.fogSettings;
  inputs.heightFogSettings = ctx.heightFogSettings;
  inputs.frameStats = &ctx.frameStats;
  return inputs;
}

void draw_offscreen_scene(const OffscreenSceneInputs &inputs,
                          const OffscreenCamera &camera,
                          const IblSelection &ibl) noexcept {
  BackendState &backend = *inputs.backend;
  const RenderDevice *dev = inputs.dev;
  const std::size_t auxiliaryTotal =
      static_cast<std::size_t>(inputs.auxiliaryView.count);
  if (((inputs.mainView.data == nullptr) || (inputs.mainTotalCount == 0U)) &&
      (auxiliaryTotal == 0U)) {
    return;
  }

  const math::Mat4 viewProjection = math::mul(camera.projection, camera.view);
  dev->bind_program(backend.pbrProgram);
  if (backend.pbrTimeLocation.valid()) {
    dev->set_param_f32(backend.pbrTimeLocation, inputs.timeSeconds);
  }
  if (backend.pbrCameraPosLocation.valid()) {
    dev->set_param_vec3(backend.pbrCameraPosLocation, &camera.position.x);
  }
  if (backend.pbrViewLocation.valid()) {
    dev->set_param_mat4(backend.pbrViewLocation, &camera.view.columns[0].x);
  }
  if (backend.pbrViewProjectionLocation.valid()) {
    dev->set_param_mat4(backend.pbrViewProjectionLocation,
                        &viewProjection.columns[0].x);
  }
  if (backend.pbrUseInstancingLocation.valid()) {
    dev->set_param_i32(backend.pbrUseInstancingLocation, 0);
  }
  upload_pbr_lighting_uniforms(backend, dev, *inputs.lights);
  upload_pbr_distance_fog_uniforms(backend, dev, inputs.fogSettings);
  upload_pbr_height_fog_uniforms(backend, dev, inputs.heightFogSettings);
  bind_pbr_shadow_uniforms(backend, dev, *inputs.lights, false, false, false);
  apply_pbr_ibl_uniforms(backend, dev, ibl);
  if (backend.pbrAlbedoMapLocation.valid()) {
    dev->set_param_i32(backend.pbrAlbedoMapLocation, 0);
  }

  // Offscreen renders share their programs, and their GL uniform state,
  // with the main forward pass, so every draw here sets its own material
  // uniforms even when its materials never use them; otherwise a draw
  // would silently keep whatever texture the last forward draw left bound.
  // Parameter locations are registry-global by name, so one location set
  // serves every shading program.
  const ForwardDrawProgram program = pbr_forward_draw_program(backend);

  // A capture shades each material with its own program, as the main view
  // does: the key groups draws by program, so each run binds its program
  // once. A newly bound program gets its sampler units again, since a
  // sampler left at its default unit aliases whatever sits there.
  DeviceProgramHandle boundProgram = backend.pbrProgram;
  auto bindProgramForRun = [&](DeviceProgramHandle runProgram) {
    if (runProgram == boundProgram) {
      return;
    }
    dev->bind_program(runProgram);
    boundProgram = runProgram;
    apply_pbr_ibl_uniforms(backend, dev, ibl);
    if (backend.pbrAlbedoMapLocation.valid()) {
      dev->set_param_i32(backend.pbrAlbedoMapLocation, 0);
    }
  };

  // Commands render prep culled for the main camera but flagged for this
  // camera ride in the auxiliary list.
  auto drawRange = [&](const CommandBufferView &view, std::size_t start,
                       std::size_t end, std::uint16_t requiredMask) {
    // A mesh showing the target being rendered is drawn without it rather
    // than sampling it.
    ForwardDrawBindings bindings{};
    bindings.passTarget = camera.renderTarget;
    ShadingProgramRun run{};
    for (std::size_t cursor = start;
         (view.data != nullptr) &&
         next_program_run(view, &cursor, end, &run);) {
      bool bound = false;
      for (std::size_t i = run.first; i < (run.first + run.count); ++i) {
        const DrawCommand &command = view.data[i];
        if ((requiredMask != 0U) && ((command.passMask & requiredMask) == 0U)) {
          continue;
        }
        const GpuMesh *mesh = lookup_gpu_mesh(inputs.registry, command.mesh);
        if ((mesh == nullptr) || (mesh->geometry == kInvalidDeviceGeometry) ||
            (mesh->vertexCount == 0U)) {
          continue;
        }
        if (!bound) {
          bindProgramForRun(shading_program(backend, run.programId));
          bound = true;
        }
        upload_forward_material(program, backend, dev, command, &bindings);
        draw_forward_command(program, backend, dev, run.programId, command,
                             *mesh, viewProjection, inputs.frameStats);
      }
    }
  };

  dev->apply_render_state(
      RenderState{DepthTest::Less, true, BlendMode::Disabled, camera.cull});
  drawRange(inputs.mainView, 0U, inputs.mainOpaqueCount, 0U);
  if (camera.auxiliaryMask != 0U) {
    drawRange(inputs.auxiliaryView, 0U, inputs.auxiliaryOpaqueCount,
              camera.auxiliaryMask);
  }

  if ((inputs.mainOpaqueCount < inputs.mainTotalCount) ||
      (inputs.auxiliaryOpaqueCount < auxiliaryTotal)) {
    dev->apply_render_state(
        RenderState{DepthTest::Less, false, BlendMode::Alpha, CullMode::None});
    drawRange(inputs.mainView, inputs.mainOpaqueCount, inputs.mainTotalCount,
              0U);
    if (camera.auxiliaryMask != 0U) {
      drawRange(inputs.auxiliaryView, inputs.auxiliaryOpaqueCount,
                auxiliaryTotal, camera.auxiliaryMask);
    }
  }
  dev->apply_render_state(
      RenderState{DepthTest::Less, true, BlendMode::Disabled, CullMode::Back});

  dev->bind_texture_slot(0U, kInvalidDeviceTexture);
  dev->bind_program(kInvalidDeviceProgram);
}

void flush_scene_captures(FrameFlushContext &ctx) noexcept {
  BackendState &backend = ctx.backend;
  const RenderDevice *dev = ctx.dev;
  const OffscreenSceneInputs inputs = offscreen_scene_inputs(ctx);
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

    const float captureAspect = static_cast<float>(captureWidth) /
                                static_cast<float>(captureHeight);
    OffscreenCamera camera{};
    camera.view = math::look_at(request.camera.position, request.camera.target,
                                request.camera.up);
    camera.projection = camera_projection_matrix(request.camera, captureAspect);
    camera.position = request.camera.position;
    camera.auxiliaryMask = static_cast<std::uint16_t>(
        kPassCaptureBase << static_cast<unsigned int>(captureIndex));
    camera.renderTarget = target.colorTexture;
    // Captures skip sky and IBL by design.
    draw_offscreen_scene(inputs, camera, IblSelection{});
  }
  if ((captureCount > 0U) && (dev != nullptr) &&
      (dev->bind_render_target != nullptr)) {
    dev->bind_render_target(kBackBufferTarget);
  }
}

} // namespace engine::renderer
