// Verifies the production flush_debug_overlay pass ages core debug-draw
// primitives by exactly one frame per flush, so a lifeFrames=N primitive
// survives N flushes (audit R-2: the TU split had doubled the tick,
// halving every debug primitive's lifetime). Links the real forward-pass
// TU; the sibling forward-path symbols it carries are satisfied by no-op
// stubs below, and the overlay runs with the debug-line pipeline
// unavailable so only the aging contract executes.

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

// Link stubs for the forward-path helpers referenced by the flush TU; the
// debug-overlay contract under test never reaches them (debug-line
// pipeline unavailable, no draw commands submitted).
void gpu_profiler_begin_pass(GpuPassId) noexcept {}
void gpu_profiler_end_pass(GpuPassId) noexcept {}
std::uint32_t pass_resource_gpu_texture(PassResourceId) noexcept { return 1U; }
std::uint32_t pass_resource_framebuffer(PassResourceId) noexcept { return 1U; }
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *,
                               MeshHandle) noexcept {
  return nullptr;
}
std::uint32_t texture_gpu_id(TextureHandle) noexcept { return 0U; }
SkyModel selected_sky_model() noexcept { return SkyModel::None; }
void draw_skybox(const BackendState &, const RenderDevice *,
                 const math::Mat4 &, const math::Mat4 &, std::uint32_t,
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
                           .envSkyboxTexture = 0U,
                           .iblPrefilteredTex = 0U,
                           .iblIrradianceTex = 0U,
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

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer Debug Overlay Unit Tests ===\n");

  test_one_age_step_per_flush();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
