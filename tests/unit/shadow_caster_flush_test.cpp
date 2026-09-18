// Verifies the production spot and point shadow passes (flush_shadow_passes)
// against a fake render device: a light flagged castShadow gets a depth
// pass (its slot's target bound, cleared, and the opaque draw list drawn
// into it; six faces for a point light), an unflagged light gets none, and
// the flush reports the passes as active for the lighting binds. Until
// #522 no producer set the flag, so these passes never ran in any scene.
// Auxiliary (camera-culled) casters are drawn into the same passes (#524).

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/core/cvar.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shadow_map.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

/// What the fake device records: every bound target in order, clears and
/// draws attributed to the target bound at the time.
struct FakeDeviceLog final {
  static constexpr std::size_t kMaxBinds = 64U;
  std::uint32_t boundTargets[kMaxBinds] = {};
  std::size_t bindCount = 0U;
  std::uint32_t currentTarget = 0U;
  std::size_t clearsOnTargets = 0U;
  std::size_t drawsOnTargets = 0U;
  std::size_t drawsOnBackBuffer = 0U;
};

FakeDeviceLog g_log{};
RenderDevice g_device{};
GpuMesh g_mesh{};

void fake_bind_render_target(RenderTargetHandle target) noexcept {
  if (g_log.bindCount < FakeDeviceLog::kMaxBinds) {
    g_log.boundTargets[g_log.bindCount] = target.value;
  }
  ++g_log.bindCount;
  g_log.currentTarget = target.value;
}
void fake_clear(ClearFlags, float, float, float, float) noexcept {
  if (g_log.currentTarget != 0U) {
    ++g_log.clearsOnTargets;
  }
}
void fake_draw(DeviceGeometryHandle, PrimitiveTopology, std::int32_t,
               std::int32_t) noexcept {
  if (g_log.currentTarget != 0U) {
    ++g_log.drawsOnTargets;
  } else {
    ++g_log.drawsOnBackBuffer;
  }
}
void fake_draw_indexed(DeviceGeometryHandle, std::int32_t) noexcept {
  fake_draw(DeviceGeometryHandle{}, PrimitiveTopology::Triangles, 0, 0);
}
void fake_bind_program(DeviceProgramHandle) noexcept {}
void fake_set_param_f32(ShaderParam, float) noexcept {}
void fake_set_param_mat4(ShaderParam, const float *) noexcept {}
void fake_set_param_vec3(ShaderParam, const float *) noexcept {}
void fake_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {}
void fake_apply_render_state(const RenderState &) noexcept {}

/// Installs the fake device table and clears its log.
void reset_fake_device() noexcept {
  g_log = FakeDeviceLog{};
  g_device = RenderDevice{};
  g_device.bind_render_target = &fake_bind_render_target;
  g_device.clear = &fake_clear;
  g_device.draw = &fake_draw;
  g_device.draw_indexed = &fake_draw_indexed;
  g_device.bind_program = &fake_bind_program;
  g_device.set_param_f32 = &fake_set_param_f32;
  g_device.set_param_mat4 = &fake_set_param_mat4;
  g_device.set_param_vec3 = &fake_set_param_vec3;
  g_device.set_viewport = &fake_set_viewport;
  g_device.apply_render_state = &fake_apply_render_state;
}

} // namespace

// Link seams for the shadow flush TU: the device, the profiler, the mesh
// registry lookup (every handle resolves to one unskinned mesh) and the
// skinning helpers the unskinned draws never reach.
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

constexpr std::uint32_t kSpotTargetBase = 100U;
constexpr std::uint32_t kPointTargetBase = 200U;

/// A backend whose spot and point shadow resources report available, with
/// distinct fake target handles per slot and face.
BackendState g_backend{};

void reset_backend() noexcept {
  g_backend = BackendState{};
  g_backend.spotShadowAvailable = true;
  g_backend.pointShadowAvailable = true;
  g_backend.shadowDepthProgram = DeviceProgramHandle{1U};
  g_backend.shadowDepthPointProgram = DeviceProgramHandle{2U};
  for (std::size_t s = 0U; s < kMaxSpotShadowLights; ++s) {
    g_backend.spotShadowState.slots[s].depthTarget =
        RenderTargetHandle{kSpotTargetBase + static_cast<std::uint32_t>(s)};
  }
  for (std::size_t s = 0U; s < kMaxPointShadowLights; ++s) {
    for (std::uint32_t face = 0U; face < 6U; ++face) {
      g_backend.pointShadowState.slots[s].faceTargets[face] = RenderTargetHandle{
          kPointTargetBase + (static_cast<std::uint32_t>(s) * 6U) + face};
    }
  }
}

DrawCommand g_draws[2] = {};

FrameFlushContext make_context(const SceneLightData &lights) noexcept {
  return FrameFlushContext{.backend = g_backend,
                           .dev = render_device(),
                           .commandBufferView = {g_draws, 2U},
                           .registry = nullptr,
                           .lights = lights,
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
                           .totalCount = 2U,
                           .opaqueBatchCount = 0U,
                           .gbufferDebugMode = 0};
}

/// Whether the fake device bound `target` at any point of the flush.
bool bound(std::uint32_t target) noexcept {
  const std::size_t count = (g_log.bindCount < FakeDeviceLog::kMaxBinds)
                                ? g_log.bindCount
                                : FakeDeviceLog::kMaxBinds;
  for (std::size_t i = 0U; i < count; ++i) {
    if (g_log.boundTargets[i] == target) {
      return true;
    }
  }
  return false;
}

/// EXPECTATION: an unflagged spot light and an unflagged point light get
/// no depth pass at all; the passes are reported inactive-with-no-slots.
void test_unflagged_lights_cast_nothing() noexcept {
  reset_backend();
  reset_fake_device();
  SceneLightData lights{};
  lights.spotLightCount = 1U;
  lights.pointLightCount = 1U;
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  CHECK(g_log.bindCount == 0U, "no target bound for unflagged lights");
  CHECK(g_log.drawsOnTargets == 0U, "no depth draws for unflagged lights");
  CHECK(g_backend.spotShadowState.slots[0].lightIndex == -1,
        "no spot slot assigned");
  CHECK(g_backend.pointShadowState.slots[0].lightIndex == -1,
        "no point slot assigned");
}

/// EXPECTATION: a spot light flagged castShadow gets slot 0: its depth
/// target is bound and cleared and both opaque draws land in it.
void test_flagged_spot_light_renders_depth() noexcept {
  reset_backend();
  reset_fake_device();
  SceneLightData lights{};
  lights.spotLightCount = 2U;
  lights.spotLights[1].castShadow = true;
  lights.spotLights[1].position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  lights.spotLights[1].radius = 12.0F;
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  CHECK(ctx.doSpotShadows, "spot shadow pass reported active");
  CHECK(g_backend.spotShadowState.slots[0].lightIndex == 1,
        "the flagged light owns slot 0");
  CHECK(g_backend.spotShadowState.slots[1].lightIndex == -1,
        "the unflagged light owns nothing");
  CHECK(bound(kSpotTargetBase), "slot 0 depth target bound");
  CHECK(!bound(kSpotTargetBase + 1U), "slot 1 depth target untouched");
  CHECK(g_log.clearsOnTargets == 1U, "one depth clear");
  CHECK(g_log.drawsOnTargets == 2U, "both opaque draws rendered into depth");
  CHECK(g_log.drawsOnBackBuffer == 0U, "nothing drawn to the back buffer");
  CHECK(g_backend.spotShadowState.slots[0].farPlane == 12.0F,
        "slot far plane is the light radius");
}

/// EXPECTATION: a point light flagged castShadow renders all six cube
/// faces of slot 0, each cleared and each carrying both opaque draws.
void test_flagged_point_light_renders_six_faces() noexcept {
  reset_backend();
  reset_fake_device();
  SceneLightData lights{};
  lights.pointLightCount = 1U;
  lights.pointLights[0].castShadow = true;
  lights.pointLights[0].radius = 8.0F;
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  CHECK(ctx.doPointShadows, "point shadow pass reported active");
  CHECK(g_backend.pointShadowState.slots[0].lightIndex == 0,
        "the flagged light owns slot 0");
  for (std::uint32_t face = 0U; face < 6U; ++face) {
    CHECK(bound(kPointTargetBase + face), "cube face target bound");
  }
  CHECK(!bound(kPointTargetBase + 6U), "slot 1 faces untouched");
  CHECK(g_log.clearsOnTargets == 6U, "six face clears");
  CHECK(g_log.drawsOnTargets == 12U, "both draws rendered into every face");
}

/// EXPECTATION (#524): casters render prep culled for the camera but kept
/// in the auxiliary list are drawn into the depth passes too, and an
/// auxiliary command flagged only for a capture is not.
void test_auxiliary_casters_are_drawn() noexcept {
  reset_backend();
  reset_fake_device();
  DrawCommand auxiliary[2] = {};
  auxiliary[0].mesh = MeshHandle{3U};
  auxiliary[0].passMask = kPassShadowCaster;
  auxiliary[1].mesh = MeshHandle{4U};
  auxiliary[1].passMask = kPassCaptureBase;
  SceneLightData lights{};
  lights.spotLightCount = 1U;
  lights.spotLights[0].castShadow = true;
  FrameFlushContext ctx = make_context(lights);
  ctx.auxiliaryView = {auxiliary, 2U};
  ctx.auxiliaryOpaqueCount = 2U;
  flush_shadow_passes(ctx);
  CHECK(g_log.drawsOnTargets == 3U,
        "two visible draws plus the off-screen caster rendered into depth");
  CHECK(g_log.drawsOnBackBuffer == 0U, "nothing drawn to the back buffer");
}

/// EXPECTATION: the cvar gate still holds — with r_point_shadows off a
/// flagged light renders nothing and the pass reports inactive.
void test_cvar_gate_still_disables() noexcept {
  reset_backend();
  reset_fake_device();
  engine::core::cvar_set_bool("r_point_shadows", false);
  SceneLightData lights{};
  lights.pointLightCount = 1U;
  lights.pointLights[0].castShadow = true;
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  CHECK(!ctx.doPointShadows, "point pass inactive under the cvar");
  CHECK(g_log.bindCount == 0U, "no target bound under the cvar");
  engine::core::cvar_set_bool("r_point_shadows", true);
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::core::cvar_register_bool("r_spot_shadows", true, "test");
  engine::core::cvar_register_bool("r_point_shadows", true, "test");
  g_mesh.geometry = DeviceGeometryHandle{7U};
  g_mesh.vertexCount = 3U;
  g_mesh.indexCount = 3U;
  g_draws[0].mesh = MeshHandle{1U};
  g_draws[1].mesh = MeshHandle{2U};

  test_unflagged_lights_cast_nothing();
  test_flagged_spot_light_renders_depth();
  test_flagged_point_light_renders_six_faces();
  test_auxiliary_casters_are_drawn();
  test_cvar_gate_still_disables();

  if (g_failures != 0) {
    std::fprintf(stderr, "shadow_caster_flush_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::puts("shadow_caster_flush_test passed");
  return 0;
}
