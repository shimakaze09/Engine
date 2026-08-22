// Verifies the production flush_debug_overlay pass ages core debug-draw
// primitives by exactly one frame per flush, so a lifeFrames=N primitive
// survives N flushes (audit R-2: the TU split had doubled the tick,
// halving every debug primitive's lifetime), and that submitted spheres
// reach the GPU as tessellated line segments through the shared line
// pipeline instead of being silently dropped (audit M-08). Links the real
// forward-pass TU; the sibling forward-path symbols it carries are
// satisfied by no-op stubs below. The aging test runs with the debug-line
// pipeline unavailable; the geometry tests install a counting fake device.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "command_buffer_math.h"
#include "command_buffer_sky.h"
#include "engine/core/debug_draw.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

/// Link stub: this harness compiles command_buffer_context.cpp, whose
/// projection helpers consult the live device's clip conventions; no
/// device exists here, so the GL-convention defaults apply.
const RenderDevice *render_device() noexcept { return nullptr; }


// Link stubs for the forward-path helpers referenced by the flush TU; the
// debug-overlay contract under test never reaches them (debug-line
// pipeline unavailable, no draw commands submitted).
void gpu_profiler_begin_pass(GpuPassId) noexcept {}
void gpu_profiler_end_pass(GpuPassId) noexcept {}
DeviceTextureHandle pass_resource_texture(PassResourceId) noexcept {
  return DeviceTextureHandle{1U};
}
RenderTargetHandle pass_resource_target(PassResourceId) noexcept {
  return RenderTargetHandle{1U};
}
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *,
                               MeshHandle) noexcept {
  return nullptr;
}
DeviceTextureHandle texture_device_handle(TextureHandle) noexcept {
  return kInvalidDeviceTexture;
}
SkyModel selected_sky_model() noexcept { return SkyModel::None; }
void draw_skybox(const BackendState &, const RenderDevice *,
                 const math::Mat4 &, const math::Mat4 &, DeviceTextureHandle,
                 RendererFrameStats &) noexcept {}
void draw_preetham_sky(const BackendState &, const RenderDevice *,
                       const math::Mat4 &, const math::Mat4 &,
                       const SceneLightData &,
                       RendererFrameStats &) noexcept {}
void draw_hosek_sky(const BackendState &, const RenderDevice *,
                    const math::Mat4 &, const math::Mat4 &,
                    const SceneLightData &, RendererFrameStats &) noexcept {}
void apply_pbr_ibl_uniforms(const BackendState &, const RenderDevice *,
                            bool) noexcept {}
void bind_pbr_shadow_uniforms(const BackendState &, const RenderDevice *,
                              const SceneLightData &, bool, bool,
                              bool) noexcept {}
void unbind_pbr_shadow_textures(const RenderDevice *) noexcept {}
void unbind_pbr_ibl_textures(const RenderDevice *) noexcept {}
void upload_pbr_lighting_uniforms(const BackendState &, const RenderDevice *,
                                  const SceneLightData &) noexcept {}
void upload_pbr_distance_fog_uniforms(const BackendState &,
                                      const RenderDevice *,
                                      const DistanceFogSettings &) noexcept {}
void upload_pbr_height_fog_uniforms(const BackendState &,
                                    const RenderDevice *,
                                    const HeightFogSettings &) noexcept {}
void upload_pbr_foliage_uniforms(const BackendState &, const RenderDevice *,
                                 const DrawCommand &) noexcept {}
bool upload_instance_matrices(BackendState &, const RenderDevice *,
                              const GpuMesh &, CommandBufferView,
                              const StaticMeshBatch &) noexcept {
  return false;
}
void upload_material_texture_slots(const MaterialTextureUniformLocs &,
                                   const RenderDevice *, const Material &,
                                   DeviceTextureHandle *) noexcept {}

} // namespace engine::renderer

namespace {

using namespace engine::renderer;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Builds a minimal flush context over the shared backend for the overlay.
FrameFlushContext make_context(const SceneLightData &lights,
                               const PassResources &passRes,
                               const RenderDevice *dev) noexcept {
  return FrameFlushContext{.backend = backend_state(),
                           .dev = dev,
                           .commandBufferView = {},
                           .registry = nullptr,
                           .lights = lights,
                           .timeSeconds = 0.0F,
                           .passRes = passRes,
                           .drawableWidth = 640,
                           .drawableHeight = 480,
                           .fogSettings = {},
                           .heightFogSettings = {},
                           .envSkyboxTexture = {},
                           .iblPrefilteredTex = {},
                           .iblIrradianceTex = {},
                           .iblAvailable = false,
                           .viewMat = {},
                           .projMat = {},
                           .viewProjection = {},
                           .nearP = 0.1F,
                           .farP = 100.0F,
                           .opaqueCount = 0U,
                           .totalCount = 0U,
                           .opaqueBatchCount = 0U,
                           .gbufferDebugMode = 0};
}

// Counters recorded by the fake device while the overlay draws.
struct FakeOverlayStats final {
  std::size_t drawCalls = 0U;
  std::int32_t vertexTotal = 0;
};

FakeOverlayStats g_stats{};

void fake_bind_render_target(RenderTargetHandle) noexcept {}
void fake_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {}
void fake_apply_render_state(const RenderState &) noexcept {}
void fake_bind_program(DeviceProgramHandle) noexcept {}
void fake_update_buffer(DeviceBufferHandle, const void *,
                        std::ptrdiff_t) noexcept {}
void fake_draw(DeviceGeometryHandle, PrimitiveTopology topology, std::int32_t,
               std::int32_t count) noexcept {
  if (topology != PrimitiveTopology::Lines) {
    return;
  }
  ++g_stats.drawCalls;
  g_stats.vertexTotal += count;
}

/// Builds a device table stubbing exactly the entry points the overlay uses.
RenderDevice make_fake_device() noexcept {
  RenderDevice device{};
  device.bind_render_target = &fake_bind_render_target;
  device.set_viewport = &fake_set_viewport;
  device.apply_render_state = &fake_apply_render_state;
  device.bind_program = &fake_bind_program;
  device.update_buffer = &fake_update_buffer;
  device.draw = &fake_draw;
  return device;
}

/// EXPECTATION (audit R-2): one flush ages debug primitives exactly one
/// frame, so a lifeFrames=3 line is still returned after two flushes and
/// expires only after the third.
void test_one_age_step_per_flush() noexcept {
  CHECK(engine::core::initialize_debug_draw(), "debug draw initializes");
  backend_state() = BackendState{};
  static const SceneLightData lights{};
  static const PassResources passRes{};
  static const RenderDevice device{};

  engine::core::debug_draw_line({}, {1.0F, 0.0F, 0.0F}, {}, 3U);
  engine::core::DebugLine lines[4]{};
  CHECK(engine::core::debug_draw_get_lines(lines, 4U) == 1U,
        "submitted line is queued");

  FrameFlushContext first = make_context(lights, passRes, &device);
  flush_debug_overlay(first);
  CHECK(engine::core::debug_draw_get_lines(lines, 4U) == 1U,
        "line survives the first flush");

  FrameFlushContext second = make_context(lights, passRes, &device);
  flush_debug_overlay(second);
  CHECK(engine::core::debug_draw_get_lines(lines, 4U) == 1U,
        "line survives the second flush");

  FrameFlushContext third = make_context(lights, passRes, &device);
  flush_debug_overlay(third);
  CHECK(engine::core::debug_draw_get_lines(lines, 4U) == 0U,
        "line expires after the third flush");

  engine::core::shutdown_debug_draw();
}

/// EXPECTATION (audit M-08): a submitted sphere is drawn as three great
/// circles of 16 segments each through the line pipeline (96 vertices),
/// not silently dropped.
void test_sphere_renders_through_line_pipeline() noexcept {
  CHECK(engine::core::initialize_debug_draw(), "debug draw initializes");
  backend_state() = BackendState{};
  backend_state().debugLineAvailable = true;
  backend_state().debugLineProgram = DeviceProgramHandle{1U};
  backend_state().debugLineGeometry = DeviceGeometryHandle{1U};
  backend_state().debugLineVbo = DeviceBufferHandle{1U};
  backend_state().debugLineViewProjectionLoc = kInvalidShaderParam;
  static const SceneLightData lights{};
  static const PassResources passRes{};
  const RenderDevice device = make_fake_device();
  g_stats = FakeOverlayStats{};

  engine::core::debug_draw_sphere({}, 2.0F, {}, 1U);
  FrameFlushContext ctx = make_context(lights, passRes, &device);
  flush_debug_overlay(ctx);

  CHECK(g_stats.drawCalls == 1U, "sphere produces one line draw call");
  CHECK(g_stats.vertexTotal == 96, "sphere tessellates to 48 segments");

  engine::core::shutdown_debug_draw();
}

/// EXPECTATION (audit M-08): lines and spheres share one vertex stream, and
/// text submission does not disturb the drawn geometry.
void test_lines_and_spheres_share_stream() noexcept {
  CHECK(engine::core::initialize_debug_draw(), "debug draw initializes");
  backend_state() = BackendState{};
  backend_state().debugLineAvailable = true;
  backend_state().debugLineProgram = DeviceProgramHandle{1U};
  backend_state().debugLineGeometry = DeviceGeometryHandle{1U};
  backend_state().debugLineVbo = DeviceBufferHandle{1U};
  backend_state().debugLineViewProjectionLoc = kInvalidShaderParam;
  static const SceneLightData lights{};
  static const PassResources passRes{};
  const RenderDevice device = make_fake_device();
  g_stats = FakeOverlayStats{};

  engine::core::debug_draw_line({}, {1.0F, 0.0F, 0.0F}, {}, 1U);
  engine::core::debug_draw_line({}, {0.0F, 1.0F, 0.0F}, {}, 1U);
  engine::core::debug_draw_sphere({}, 1.0F, {}, 1U);
  engine::core::debug_draw_text({}, "label", {}, 1U);
  FrameFlushContext ctx = make_context(lights, passRes, &device);
  flush_debug_overlay(ctx);

  CHECK(g_stats.drawCalls == 1U, "combined batch fits one chunk");
  CHECK(g_stats.vertexTotal == 100, "two lines plus 48 sphere segments");

  engine::core::shutdown_debug_draw();
}

/// EXPECTATION (audit M-08): geometry beyond one 1024-segment chunk is
/// drawn in additional chunks rather than truncated.
void test_segment_chunking_draws_everything() noexcept {
  CHECK(engine::core::initialize_debug_draw(), "debug draw initializes");
  backend_state() = BackendState{};
  backend_state().debugLineAvailable = true;
  backend_state().debugLineProgram = DeviceProgramHandle{1U};
  backend_state().debugLineGeometry = DeviceGeometryHandle{1U};
  backend_state().debugLineVbo = DeviceBufferHandle{1U};
  backend_state().debugLineViewProjectionLoc = kInvalidShaderParam;
  static const SceneLightData lights{};
  static const PassResources passRes{};
  const RenderDevice device = make_fake_device();
  g_stats = FakeOverlayStats{};

  for (std::size_t i = 0U; i < 1024U; ++i) {
    engine::core::debug_draw_line({}, {1.0F, 0.0F, 0.0F}, {}, 1U);
  }
  engine::core::debug_draw_sphere({}, 1.0F, {}, 1U);
  FrameFlushContext ctx = make_context(lights, passRes, &device);
  flush_debug_overlay(ctx);

  CHECK(g_stats.drawCalls == 2U, "overflow spills into a second chunk");
  CHECK(g_stats.vertexTotal == 2144, "all 1072 segments are drawn");

  engine::core::shutdown_debug_draw();
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer Debug Overlay Unit Tests ===\n");

  test_one_age_step_per_flush();
  test_sphere_renders_through_line_pipeline();
  test_lines_and_spheres_share_stream();
  test_segment_chunking_draws_everything();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
