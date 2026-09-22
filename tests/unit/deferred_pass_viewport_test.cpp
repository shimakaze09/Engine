// Holds the deferred path (flush_deferred_path) to one device-call
// invariant: a pass that binds a render target sets that target's
// viewport before it draws anything.
//
// The invariant is not cosmetic. Each bind claims a fresh bgfx view, and
// a view's rect persists per id across frames, so a pass that binds and
// never sets a rect draws with whatever rect its id last carried. Which
// id a pass lands on is not stable either: it shifts by however many
// views the passes before it claimed, and the directional shadow cache
// alone moves every later id by four cascades whenever the camera moves
// and invalidates it. The two together put a pass's draws wherever an
// unrelated pass was drawing, for the frames around that shift, with
// nothing reporting it (#640, and #632 has the same shape).
//
// Recording the device call order is the whole test: no view ids are
// simulated, because the defect is visible one level up as a draw issued
// after a bind with no viewport in between.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/core/cvar.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/texture_loader.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

/// The ordered device calls this test cares about.
enum class Call : std::uint8_t { Bind, Viewport, Draw };

struct FakeDeviceLog final {
  static constexpr std::size_t kMaxCalls = 512U;
  Call calls[kMaxCalls] = {};
  std::uint32_t targets[kMaxCalls] = {};
  std::size_t count = 0U;
  bool overflowed = false;
  std::uint32_t currentTarget = 0U;

  void record(Call call) noexcept {
    if (count >= kMaxCalls) {
      overflowed = true;
      return;
    }
    calls[count] = call;
    targets[count] = currentTarget;
    ++count;
  }
};

FakeDeviceLog g_log{};
RenderDevice g_device{};
GpuMesh g_mesh{};

void fake_bind_render_target(RenderTargetHandle target) noexcept {
  g_log.currentTarget = target.value;
  g_log.record(Call::Bind);
}
void fake_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {
  g_log.record(Call::Viewport);
}
void fake_draw(DeviceGeometryHandle, PrimitiveTopology, std::int32_t,
               std::int32_t) noexcept {
  g_log.record(Call::Draw);
}
void fake_draw_indexed(DeviceGeometryHandle, std::int32_t) noexcept {
  g_log.record(Call::Draw);
}
void fake_copy_depth(RenderTargetHandle, RenderTargetHandle, std::int32_t,
                     std::int32_t) noexcept {}
void fake_clear(ClearFlags, float, float, float, float) noexcept {}
void fake_bind_program(DeviceProgramHandle) noexcept {}
void fake_bind_texture_slot(std::uint32_t, DeviceTextureHandle) noexcept {}
void fake_set_param_f32(ShaderParam, float) noexcept {}
void fake_set_param_i32(ShaderParam, std::int32_t) noexcept {}
void fake_set_param_mat4(ShaderParam, const float *) noexcept {}
void fake_set_param_vec3(ShaderParam, const float *) noexcept {}
void fake_set_param_vec4(ShaderParam, const float *) noexcept {}
void fake_set_param_vec4_array(ShaderParam, const float *,
                               std::int32_t) noexcept {}
void fake_set_param_mat4_array(ShaderParam, const float *,
                               std::int32_t) noexcept {}
void fake_set_param_mat3(ShaderParam, const float *) noexcept {}
void fake_set_param_vec2(ShaderParam, const float *) noexcept {}
void fake_apply_render_state(const RenderState &) noexcept {}

/// Installs the fake device table and clears its log. `depthBlit` picks
/// which arm of ensureSceneDepthHasOpaque the path takes, because each
/// arm binds the scene target on its own.
void reset_fake_device(bool depthBlit) noexcept {
  g_log = FakeDeviceLog{};
  g_device = RenderDevice{};
  g_device.caps.depthBlit = depthBlit;
  g_device.bind_render_target = &fake_bind_render_target;
  g_device.set_viewport = &fake_set_viewport;
  g_device.draw = &fake_draw;
  g_device.draw_indexed = &fake_draw_indexed;
  g_device.copy_depth = &fake_copy_depth;
  g_device.clear = &fake_clear;
  g_device.bind_program = &fake_bind_program;
  g_device.bind_texture_slot = &fake_bind_texture_slot;
  g_device.set_param_f32 = &fake_set_param_f32;
  g_device.set_param_i32 = &fake_set_param_i32;
  g_device.set_param_mat4 = &fake_set_param_mat4;
  g_device.set_param_vec3 = &fake_set_param_vec3;
  g_device.set_param_vec4 = &fake_set_param_vec4;
  g_device.set_param_vec4_array = &fake_set_param_vec4_array;
  g_device.set_param_mat4_array = &fake_set_param_mat4_array;
  g_device.set_param_mat3 = &fake_set_param_mat3;
  g_device.set_param_vec2 = &fake_set_param_vec2;
  g_device.apply_render_state = &fake_apply_render_state;
}

} // namespace

// Link seams for the deferred flush TU.
const RenderDevice *render_device() noexcept { return &g_device; }
void gpu_profiler_begin_pass(GpuPassId) noexcept {}
void gpu_profiler_end_pass(GpuPassId) noexcept {}
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *, MeshHandle) noexcept {
  return &g_mesh;
}
bool upload_bone_palette(BackendState &, const RenderDevice *, std::uint32_t,
                         ShaderParam, std::uint32_t *) noexcept {
  return false;
}
std::size_t skin_palette_count() noexcept { return 0U; }
DeviceTextureHandle texture_device_handle(TextureHandle) noexcept {
  return kInvalidDeviceTexture;
}
bool is_texture_cubemap(TextureHandle) noexcept { return false; }
void destroy_shader_program(ShaderProgramHandle) noexcept {}
bool initialize_backend() noexcept { return true; }
float gpu_profiler_pass_ms(GpuPassId) noexcept { return 0.0F; }
void upload_skinned_gbuffer_uniforms(const BackendState &,
                                     const RenderDevice *, const math::Mat4 &,
                                     const math::Mat4 &, float,
                                     const DrawCommand &, const math::Mat4 &,
                                     const float *, DeviceTextureHandle *,
                                     DeviceTextureHandle[4]) noexcept {}
bool compute_tile_texture_layout(int, int, int,
                                 TileTextureLayout &outLayout) noexcept {
  outLayout = TileTextureLayout{};
  return false;
}
std::size_t compute_tile_buffer_size(int, int) noexcept { return 0U; }
bool cull_lights_tiled(const SceneLightData &, const float *, const float *,
                       int, int, TileLightData &outData) noexcept {
  outData = TileLightData{};
  return false;
}
bool pack_light_data(const SceneLightData &, float *, std::size_t) noexcept {
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

BackendState g_backend{};
DrawCommand g_draws[3] = {};

/// A backend whose deferred resources all report usable, so the path runs
/// its passes rather than bailing out early.
void reset_backend() noexcept {
  // A usable mesh, so the forward tail actually issues its draws rather
  // than skipping every command for want of geometry.
  g_mesh = GpuMesh{};
  g_mesh.geometry = DeviceGeometryHandle{7U};
  g_mesh.vertexCount = 3U;
  g_backend = BackendState{};
  g_backend.gbufferProgram = DeviceProgramHandle{1U};
  g_backend.deferredLightProgram = DeviceProgramHandle{2U};
  g_backend.depthCopyProgram = DeviceProgramHandle{3U};
  g_backend.pbrProgram = DeviceProgramHandle{4U};
  g_backend.emptyGeometry = DeviceGeometryHandle{5U};
  for (std::size_t i = 0U; i < kShadingModelCount; ++i) {
    g_backend.shadingPrograms[i] = DeviceProgramHandle{
        static_cast<std::uint32_t>(10U + i)};
  }
}

/// One opaque physically-based draw plus a transparent tail, which is the
/// shape that sends work through the forward-over-deferred-depth pass.
void reset_draws() noexcept {
  for (DrawCommand &command : g_draws) {
    command = DrawCommand{};
    command.mesh = MeshHandle{1U};
  }
  g_draws[1].sortKey =
      DrawKey{draw_key_shading_model_bits(ShadingModel::Toon)};
  g_draws[1].material.shadingModel = ShadingModel::Toon;
  g_draws[2].sortKey = DrawKey{kDrawKeyTransparentBit};
}

FrameFlushContext make_context() noexcept {
  return FrameFlushContext{.backend = g_backend,
                           .dev = render_device(),
                           .commandBufferView = {g_draws, 3U},
                           .registry = nullptr,
                           .lights = {},
                           .timeSeconds = 0.0F,
                           .passRes = get_pass_resources(),
                           .drawableWidth = 64,
                           .drawableHeight = 64,
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
                           .opaqueCount = 2U,
                           .totalCount = 3U,
                           .opaqueBatchCount = 0U,
                           .gbufferDebugMode = 0};
}

/// EXPECTATION: every draw the deferred path issues belongs to a target
/// whose viewport that pass set after binding it. A draw with no viewport
/// between it and its bind is the defect.
void check_every_draw_follows_a_viewport(const char *what) noexcept {
  CHECK(!g_log.overflowed, "the call log held the whole flush");
  std::size_t draws = 0U;
  std::size_t unscoped = 0U;
  bool viewportSinceBind = false;
  for (std::size_t i = 0U; i < g_log.count; ++i) {
    switch (g_log.calls[i]) {
    case Call::Bind:
      viewportSinceBind = false;
      break;
    case Call::Viewport:
      viewportSinceBind = true;
      break;
    case Call::Draw:
      ++draws;
      if (!viewportSinceBind) {
        ++unscoped;
        std::fprintf(stderr,
                     "  %s: draw on target %u with no viewport since its "
                     "bind (call %zu)\n",
                     what, g_log.targets[i], i);
      }
      break;
    }
  }
  std::printf("deferred_pass_viewport_test: %s — %zu draw(s), %zu without a "
              "viewport\n",
              what, draws, unscoped);
  // A flush that drew nothing would satisfy the invariant vacuously.
  CHECK(draws > 0U, "the deferred path issued draws at all");
  CHECK(unscoped == 0U, "every draw has its own pass viewport");
}

/// The depth-blit arm of ensureSceneDepthHasOpaque binds the scene target
/// and returns, so the forward tail's draws run on that bind.
void test_depth_blit_path() noexcept {
  reset_backend();
  reset_draws();
  reset_fake_device(true);
  FrameFlushContext ctx = make_context();
  flush_deferred_path(ctx);
  check_every_draw_follows_a_viewport("depth blit");
}

/// Without the blit capability the same lambda seeds depth with its own
/// fullscreen draw, which must also be scoped.
void test_depth_seed_path() noexcept {
  reset_backend();
  reset_draws();
  reset_fake_device(false);
  FrameFlushContext ctx = make_context();
  flush_deferred_path(ctx);
  check_every_draw_follows_a_viewport("depth seed");
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_depth_blit_path();
  test_depth_seed_path();
  if (g_failures != 0) {
    std::fprintf(stderr, "deferred_pass_viewport_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("deferred_pass_viewport_test: all checks passed\n");
  return 0;
}
