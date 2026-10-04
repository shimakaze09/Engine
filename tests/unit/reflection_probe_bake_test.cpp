// Verifies the reflection-probe passes against a fake render device: a
// probe is captured into the six faces of its own cube at its position
// and capture distance, then baked into its own environment; probes bake
// one per call and again only when their request, the sky or the bake
// generation changes; a view is lit by the baked probe whose box holds its
// camera (the smaller box winning) and by the sky otherwise; a device that
// cannot capture is not asked again every frame; a probe no longer
// requested gives its textures back; and no capture is taken on a frame a
// swapchain reset applies to.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "command_buffer_ibl.h"
#include "command_buffer_reflection_probes.h"
#include "command_buffer_sky.h"
#include "engine/core/cvar.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"

#include "../fake_render_device.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

/// What one capture face drew.
struct RecordedFace final {
  OffscreenCamera camera{};
  IblSelection ibl{};
};

std::array<RecordedFace, 64> g_faces{};
int g_faceCount = 0;
int g_skyDraws = 0;
SkyModel g_skyModel = SkyModel::Cubemap;

/// Every render target the capture created, in order.
std::array<RenderTargetDesc, 64> g_targets{};
int g_targetCount = 0;

RenderTargetHandle record_render_target(const RenderTargetDesc &desc) noexcept {
  if (g_targetCount < static_cast<int>(g_targets.size())) {
    g_targets[static_cast<std::size_t>(g_targetCount)] = desc;
  }
  ++g_targetCount;
  return RenderTargetHandle{tests::fake_create(tests::FakeKind::RenderTarget)};
}

void ignore_clear(ClearFlags, float, float, float, float) noexcept {}
void ignore_render_state(const RenderState &) noexcept {}

} // namespace

// Link stubs for the siblings the probe and IBL TUs reference. The scene
// and sky draws record what they were asked to draw.
void draw_offscreen_scene(const OffscreenSceneInputs &,
                          const OffscreenCamera &camera,
                          const IblSelection &ibl) noexcept {
  if (g_faceCount < static_cast<int>(g_faces.size())) {
    g_faces[static_cast<std::size_t>(g_faceCount)] = {camera, ibl};
  }
  ++g_faceCount;
}
void draw_skybox(const BackendState &, const RenderDevice *, const math::Mat4 &,
                 const math::Mat4 &, DeviceTextureHandle,
                 RendererFrameStats &) noexcept {
  ++g_skyDraws;
}
void draw_preetham_sky(const BackendState &, const RenderDevice *,
                       const math::Mat4 &, const math::Mat4 &,
                       const SceneLightData &, RendererFrameStats &) noexcept {
  ++g_skyDraws;
}
void draw_hosek_sky(const BackendState &, const RenderDevice *,
                    const math::Mat4 &, const math::Mat4 &,
                    const SceneLightData &, RendererFrameStats &) noexcept {
  ++g_skyDraws;
}
void destroy_shader_program(ShaderProgramHandle) noexcept {}
SkyModel selected_sky_model() noexcept { return g_skyModel; }
bool initialize_backend() noexcept { return true; }
DeviceTextureHandle
active_skybox_device_texture(const BackendState &) noexcept {
  return kInvalidDeviceTexture;
}
DeviceTextureHandle texture_device_handle(TextureHandle) noexcept {
  return kInvalidDeviceTexture;
}

} // namespace engine::renderer

namespace {

using namespace engine::renderer;
namespace math = engine::math;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// A fresh renderer: the fake device, empty probe state, and a backend
/// whose environment bakes report available.
void reset_renderer() noexcept {
  engine::tests::reset_fake_device();
  RenderDevice &device = engine::tests::fake_device();
  device.create_texture = &engine::tests::fake::create_texture;
  device.destroy_texture = &engine::tests::fake::destroy_texture;
  device.create_render_target = &record_render_target;
  device.destroy_render_target = &engine::tests::fake::destroy_render_target;
  device.bind_render_target = &engine::tests::fake::bind_render_target;
  device.apply_render_state = &ignore_render_state;
  device.bind_texture_slot = &engine::tests::fake::bind_texture_slot;
  device.bind_program = &engine::tests::fake::bind_program;
  device.set_param_i32 = &engine::tests::fake::set_param_i32;
  device.set_param_f32 = &engine::tests::fake::set_param_f32;
  device.set_param_mat4 = &engine::tests::fake::set_param_mat4;
  device.set_viewport = &engine::tests::fake::set_viewport;
  device.clear = &ignore_clear;
  device.draw = &engine::tests::fake::draw;

  RendererContext &context = renderer_context();
  context.reflectionProbeRequests = {};
  context.reflectionProbeRequestCount = 0U;
  context.reflectionProbeBakeGeneration = 0U;
  context.activeReflectionProbe.fill(-1);
  context.activeSkyboxTexture = TextureHandle{21U};
  context.backend = BackendState{};
  BackendState &backend = context.backend;
  backend.environmentPrefilterAvailable = true;
  backend.environmentPrefilterProgram = DeviceProgramHandle{7U};
  backend.environmentIrradianceAvailable = true;
  backend.environmentIrradianceProgram = DeviceProgramHandle{8U};
  backend.skyboxGeometry = DeviceGeometryHandle{3U};
  backend.brdfLutTexture = DeviceTextureHandle{900U};
  g_faceCount = 0;
  g_skyDraws = 0;
  g_targetCount = 0;
  g_skyModel = SkyModel::Cubemap;
}

ReflectionProbeRequest make_probe(std::uint64_t id, const math::Vec3 &at,
                                  float halfSize) noexcept {
  ReflectionProbeRequest request{};
  request.id = id;
  request.position = at;
  request.boxMin = math::sub(at, math::Vec3(halfSize, halfSize, halfSize));
  request.boxMax = math::add(at, math::Vec3(halfSize, halfSize, halfSize));
  request.captureDistance = 20.0F;
  request.intensity = 0.5F;
  request.boxProjection = true;
  request.faceSize = 64U;
  request.mipLevels = 3U;
  request.irradianceFaceSize = 16U;
  return request;
}

SceneLightData g_lights{};
RendererFrameStats g_stats{};

/// One frame's probe pass over an empty scene.
void bake_frame() noexcept {
  OffscreenSceneInputs inputs{};
  inputs.backend = &backend_state();
  inputs.dev = render_device();
  inputs.lights = &g_lights;
  inputs.frameStats = &g_stats;
  IblSelection sky{};
  sky.prefiltered = DeviceTextureHandle{800U};
  sky.irradiance = DeviceTextureHandle{801U};
  sky.available = true;
  bake_pending_reflection_probe(inputs, sky, DeviceTextureHandle{802U});
}

IblSelection sky_selection() noexcept {
  IblSelection sky{};
  sky.prefiltered = DeviceTextureHandle{800U};
  sky.irradiance = DeviceTextureHandle{801U};
  sky.prefilteredMipLevels = 5;
  sky.available = true;
  return sky;
}

bool same_matrix(const math::Mat4 &a, const math::Mat4 &b) noexcept {
  return std::memcmp(&a, &b, sizeof(math::Mat4)) == 0;
}

/// A probe is captured into its own cube's six faces, in cubemap order,
/// at its position and capture distance, and baked with its sizes.
void test_capture_and_bake() noexcept {
  reset_renderer();
  const ReflectionProbeRequest probe =
      make_probe(7U, math::Vec3(1.0F, 2.0F, 3.0F), 4.0F);
  set_reflection_probe_requests(&probe, 1U);
  bake_frame();

  CHECK(g_faceCount == 6, "six faces captured");
  CHECK(g_skyDraws == 6, "the sky drawn behind every face");
  CHECK(g_targetCount >= 6, "a target per face");
  CameraState camera{};
  camera.position = probe.position;
  camera.fovRadians = 1.57079632679F;
  camera.nearPlane = 0.05F;
  camera.farPlane = probe.captureDistance;
  const math::Mat4 projection = camera_projection_matrix(camera, 1.0F);
  const math::Vec3 forwards[6] = {
      math::Vec3(1.0F, 0.0F, 0.0F), math::Vec3(-1.0F, 0.0F, 0.0F),
      math::Vec3(0.0F, 1.0F, 0.0F), math::Vec3(0.0F, -1.0F, 0.0F),
      math::Vec3(0.0F, 0.0F, 1.0F), math::Vec3(0.0F, 0.0F, -1.0F)};
  // The cubemap convention's up vectors, shared with the environment bake.
  const math::Vec3 ups[6] = {
      math::Vec3(0.0F, -1.0F, 0.0F), math::Vec3(0.0F, -1.0F, 0.0F),
      math::Vec3(0.0F, 0.0F, 1.0F),  math::Vec3(0.0F, 0.0F, -1.0F),
      math::Vec3(0.0F, -1.0F, 0.0F), math::Vec3(0.0F, -1.0F, 0.0F)};
  const DeviceTextureHandle cube = g_targets[0].colors[0].texture;
  for (int face = 0; (face < 6) && (face < g_faceCount); ++face) {
    const RenderTargetDesc &target = g_targets[static_cast<std::size_t>(face)];
    const RecordedFace &drawn = g_faces[static_cast<std::size_t>(face)];
    CHECK(target.colors[0].face == static_cast<CubeFace>(face),
          "faces render in cubemap order");
    CHECK(target.colors[0].texture == cube, "every face lands in one cube");
    CHECK(target.depth.texture != kInvalidDeviceTexture,
          "every face is depth-tested");
    CHECK(drawn.camera.renderTarget == cube,
          "no draw may sample the cube being rendered");
    CHECK(drawn.camera.auxiliaryMask == kPassReflectionProbe,
          "the capture takes the draws kept for probes");
    CHECK((drawn.camera.position.x == 1.0F) &&
              (drawn.camera.position.y == 2.0F) &&
              (drawn.camera.position.z == 3.0F),
          "the capture is taken at the probe");
    CHECK(same_matrix(drawn.camera.projection, projection),
          "a quarter-turn view out to the capture distance");
    const math::Vec3 lookAt = math::add(probe.position, forwards[face]);
    CHECK(same_matrix(drawn.camera.view,
                      math::look_at(probe.position, lookAt, ups[face])),
          "each face looks along its cubemap axis, the right way up");
    CHECK(drawn.ibl.prefiltered == DeviceTextureHandle{800U},
          "the capture is lit by the sky's environment, never its own");
  }

  ReflectionProbeStatus status{};
  CHECK(get_reflection_probe_status(0U, &status), "status of request 0");
  CHECK(status.baked && (status.bakeCount == 1U), "the probe is baked once");
  CHECK((status.faceSize == 64U) && (status.mipLevels == 3U) &&
            (status.irradianceFaceSize == 16U),
        "baked at the probe's sizes");
  CHECK(!get_reflection_probe_status(1U, &status), "no request 1");
}

bool g_resetFrame = false;
bool fake_frame_applies_reset() noexcept { return g_resetFrame; }

/// EXPECTATION (#1212): no probe is captured on a frame a swapchain reset
/// applies to, whose off-screen output has been seen not to land on
/// Direct3D 12; the probe stays pending and is captured on the next frame.
/// On base the capture was taken, recorded as done and kept.
void test_no_capture_on_a_reset_frame() noexcept {
  reset_renderer();
  engine::tests::fake_device().frame_applies_reset = &fake_frame_applies_reset;
  const ReflectionProbeRequest probe =
      make_probe(9U, math::Vec3(0.0F, 1.0F, 0.0F), 4.0F);
  set_reflection_probe_requests(&probe, 1U);
  ReflectionProbeStatus status{};

  g_resetFrame = true;
  bake_frame();
  CHECK((g_faceCount == 0) && get_reflection_probe_status(0U, &status) &&
            !status.baked,
        "a reset frame captures nothing and leaves the probe pending");
  g_resetFrame = false;
  bake_frame();
  CHECK((g_faceCount == 6) && get_reflection_probe_status(0U, &status) &&
            status.baked && (status.bakeCount == 1U),
        "the next frame captures and bakes it");
}

/// Probes bake one per call, and again only when something they were
/// captured for changes.
void test_bakes_follow_what_changed() noexcept {
  reset_renderer();
  std::array<ReflectionProbeRequest, 2> probes = {
      make_probe(7U, math::Vec3(0.0F, 0.0F, 0.0F), 4.0F),
      make_probe(8U, math::Vec3(30.0F, 0.0F, 0.0F), 4.0F)};
  set_reflection_probe_requests(probes.data(), probes.size());
  bake_frame();
  CHECK(g_faceCount == 6, "one probe per frame");
  bake_frame();
  CHECK(g_faceCount == 12, "the second probe the next frame");
  bake_frame();
  set_reflection_probe_requests(probes.data(), probes.size());
  bake_frame();
  CHECK(g_faceCount == 12, "unchanged probes are not captured again");

  probes[1].position = math::Vec3(31.0F, 0.0F, 0.0F);
  set_reflection_probe_requests(probes.data(), probes.size());
  bake_frame();
  CHECK(g_faceCount == 18, "a moved probe is captured again");
  ReflectionProbeStatus status{};
  CHECK(get_reflection_probe_status(1U, &status) && (status.bakeCount == 2U) &&
            (status.capturePosition.x == 31.0F),
        "from where it now stands");

  renderer_context().activeSkyboxTexture = TextureHandle{22U};
  bake_frame();
  bake_frame();
  CHECK(g_faceCount == 30, "a new sky captures every probe again");

  request_reflection_probe_bake();
  bake_frame();
  bake_frame();
  bake_frame();
  CHECK(g_faceCount == 42, "a requested bake captures every probe once");
}

/// A view is lit by the baked probe whose box holds its camera, the smaller
/// box winning, and by the sky otherwise.
void test_view_selection() noexcept {
  reset_renderer();
  std::array<ReflectionProbeRequest, 3> probes = {
      make_probe(7U, math::Vec3(0.0F, 0.0F, 0.0F), 10.0F),
      make_probe(8U, math::Vec3(1.0F, 0.0F, 0.0F), 2.0F),
      make_probe(9U, math::Vec3(50.0F, 0.0F, 0.0F), 5.0F)};
  set_reflection_probe_requests(probes.data(), probes.size());
  const IblSelection sky = sky_selection();

  // Nothing is baked yet: the sky lights every view.
  IblSelection chosen =
      select_view_environment(backend_state(), 0U, math::Vec3(), sky);
  CHECK((chosen.prefiltered == sky.prefiltered) &&
            (active_reflection_probe(RenderViewId::Game) == -1),
        "an unbaked probe does not light a view");

  bake_frame();
  bake_frame();
  chosen = select_view_environment(backend_state(), 0U,
                                   math::Vec3(1.0F, 0.5F, 0.0F), sky);
  CHECK(active_reflection_probe(RenderViewId::Game) == 1,
        "inside both boxes, the smaller one lights the view");
  CHECK(chosen.available && (chosen.prefiltered != sky.prefiltered) &&
            (chosen.prefilteredMipLevels == 3),
        "the probe's own environment is bound");
  CHECK((chosen.probeBoxMin.x == -1.0F) && (chosen.probeBoxMin.w == 1.0F) &&
            (chosen.probeBoxMax.x == 3.0F) && (chosen.probeBoxMax.w == 0.5F) &&
            (chosen.probeCenter.x == 1.0F),
        "its box, projection and intensity reach the shaders");

  chosen = select_view_environment(backend_state(), 0U,
                                   math::Vec3(-8.0F, 0.0F, 0.0F), sky);
  CHECK(active_reflection_probe(RenderViewId::Game) == 0,
        "inside only the larger box, the larger one lights it");

  chosen = select_view_environment(backend_state(), 1U,
                                   math::Vec3(50.0F, 0.0F, 0.0F), sky);
  CHECK((chosen.prefiltered == sky.prefiltered) &&
            (active_reflection_probe(RenderViewId::Scene) == -1),
        "a probe not baked yet leaves its box to the sky");
  CHECK(active_reflection_probe(RenderViewId::Game) == 0,
        "views choose independently");

  chosen = select_view_environment(backend_state(), 0U,
                                   math::Vec3(0.0F, 30.0F, 0.0F), sky);
  CHECK((chosen.prefiltered == sky.prefiltered) &&
            (chosen.probeBoxMin.w == 0.0F) && (chosen.probeBoxMax.w == 1.0F) &&
            (active_reflection_probe(RenderViewId::Game) == -1),
        "outside every box the sky lights the view, unprojected");
}

/// A capture the device refuses is not retried every frame, and a probe
/// no longer requested gives its textures back.
void test_failure_and_release() noexcept {
  reset_renderer();
  const int texturesBefore =
      engine::tests::fake_alive(engine::tests::FakeKind::Texture);
  const ReflectionProbeRequest probe =
      make_probe(7U, math::Vec3(0.0F, 0.0F, 0.0F), 4.0F);
  set_reflection_probe_requests(&probe, 1U);
  engine::tests::fake_log().failKinds =
      engine::tests::fake_kind_bit(engine::tests::FakeKind::RenderTarget);
  bake_frame();
  ReflectionProbeStatus status{};
  CHECK(get_reflection_probe_status(0U, &status) && !status.baked,
        "a refused capture leaves the probe unbaked");
  const int targetsAfterFailure = g_targetCount;
  bake_frame();
  CHECK(g_targetCount == targetsAfterFailure, "and is not retried every frame");

  engine::tests::fake_log().failKinds = 0U;
  request_reflection_probe_bake();
  bake_frame();
  CHECK(get_reflection_probe_status(0U, &status) && status.baked,
        "a requested bake tries again");
  CHECK(engine::tests::fake_alive(engine::tests::FakeKind::RenderTarget) == 0,
        "every face target is destroyed after the capture");

  set_reflection_probe_requests(nullptr, 0U);
  CHECK(engine::tests::fake_alive(engine::tests::FakeKind::Texture) ==
            texturesBefore,
        "a removed probe's textures are destroyed");
  CHECK(reflection_probe_request_count() == 0U, "no requests remain");
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Reflection Probe Bake Unit Tests ===\n");

  engine::core::initialize_cvars();
  test_capture_and_bake();
  test_bakes_follow_what_changed();
  test_view_selection();
  test_failure_and_release();
  test_no_capture_on_a_reset_frame();
  engine::core::shutdown_cvars();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
