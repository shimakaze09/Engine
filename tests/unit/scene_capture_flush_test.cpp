// Verifies the production scene-capture pass (flush_scene_captures) against
// a fake render device: a mesh whose albedo is the capture being rendered
// (a monitor that its own capture camera can see) is drawn with the
// fallback texture instead of sampling that capture's colour target while
// the pass renders into it, and a mesh with an ordinary texture, or showing
// another capture, still samples it.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"

#include "../fake_render_device.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

/// The texture handles the capture targets register as, one per slot, and
/// the ordinary texture a material names.
constexpr std::uint32_t kCaptureHandleBase = 500U;
constexpr TextureHandle kOrdinaryTexture{7U};
constexpr DeviceTextureHandle kOrdinaryDeviceTexture{7007U};

GpuMesh g_mesh{};

/// What slot 0 held at each draw, and the target the draw landed on.
struct DrawRecord final {
  std::uint32_t albedo = 0U;
  std::uint32_t target = 0U;
};
constexpr std::size_t kMaxDraws = 16U;
DrawRecord g_drawLog[kMaxDraws] = {};
std::size_t g_drawCount = 0U;
std::uint32_t g_slot0 = 0U;
std::uint32_t g_boundTarget = 0U;

void record_bind_texture_slot(std::uint32_t slot,
                              DeviceTextureHandle texture) noexcept {
  if (slot == 0U) {
    g_slot0 = texture.value;
  }
}

void record_bind_render_target(RenderTargetHandle target) noexcept {
  g_boundTarget = target.value;
}

void record_draw_indexed(DeviceGeometryHandle, std::int32_t) noexcept {
  if (g_drawCount < kMaxDraws) {
    g_drawLog[g_drawCount] = DrawRecord{g_slot0, g_boundTarget};
  }
  ++g_drawCount;
}

void reset_fake_device() noexcept {
  tests::reset_fake_device();
  RenderDevice &device = tests::fake_device();
  device.create_texture = &tests::fake::create_texture;
  device.destroy_texture = &tests::fake::destroy_texture;
  device.create_render_target = &tests::fake::create_render_target;
  device.destroy_render_target = &tests::fake::destroy_render_target;
  device.bind_render_target = &record_bind_render_target;
  device.bind_texture_slot = &record_bind_texture_slot;
  device.bind_program = &tests::fake::bind_program;
  device.draw_indexed = &record_draw_indexed;
  device.set_param_f32 = &tests::fake::set_param_f32;
  device.set_param_i32 = &tests::fake::set_param_i32;
  device.set_param_vec2 = &tests::fake::set_param_vec2;
  device.set_param_vec3 = &tests::fake::set_param_vec3;
  device.set_param_vec4 = &tests::fake::set_param_vec4;
  device.set_param_mat3 = &tests::fake::set_param_mat3;
  device.set_param_mat4 = &tests::fake::set_param_mat4;
  device.set_param_vec4_array = &tests::fake::set_param_vec4_array;
  device.set_param_mat4_array = &tests::fake::set_param_mat4_array;
  device.set_viewport = &tests::fake::set_viewport;
  device.apply_render_state = &tests::fake::apply_render_state;
  device.clear = &tests::fake::clear;
  g_drawCount = 0U;
  g_slot0 = 0U;
  g_boundTarget = 0U;
}

} // namespace

// Link seams: the texture system (each capture slot registers as its own
// handle, which resolves to that slot's current colour target; one other
// handle is an ordinary texture) and the mesh registry (every handle
// resolves to one indexed mesh).
TextureHandle register_external_texture(DeviceTextureHandle) noexcept {
  static std::uint32_t next = kCaptureHandleBase;
  return TextureHandle{next++};
}
bool update_external_texture(TextureHandle, DeviceTextureHandle) noexcept {
  return true;
}
void unload_texture(TextureHandle) noexcept {}
DeviceTextureHandle texture_device_handle(TextureHandle handle) noexcept {
  if (handle == kOrdinaryTexture) {
    return kOrdinaryDeviceTexture;
  }
  for (std::size_t slot = 0U; slot < kMaxSceneCaptures; ++slot) {
    const SceneCaptureTarget &target =
        backend_state().sceneCaptureTargets[slot];
    if ((handle != kInvalidTextureHandle) && (target.textureHandle == handle)) {
      return target.colorTexture;
    }
  }
  return kInvalidDeviceTexture;
}
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *, MeshHandle) noexcept {
  return &g_mesh;
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

constexpr std::size_t kDrawCount = 3U;
DrawCommand g_draws[kDrawCount] = {};

FrameFlushContext make_context(const SceneLightData &lights) noexcept {
  return FrameFlushContext{.backend = backend_state(),
                           .dev = render_device(),
                           .commandBufferView = {g_draws, kDrawCount},
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
                           .opaqueCount = kDrawCount,
                           .totalCount = kDrawCount,
                           .opaqueBatchCount = 0U,
                           .gbufferDebugMode = 0};
}

/// EXPECTATION: with two captures, capture 0's pass draws the mesh showing
/// capture 0 with the fallback texture, and the meshes showing capture 1
/// and an ordinary texture with theirs; capture 1's pass likewise refuses
/// only capture 1. No draw ever samples the target it lands on.
void test_capture_never_samples_its_own_target() noexcept {
  reset_fake_device();
  BackendState &backend = backend_state();
  backend.pbrProgram = DeviceProgramHandle{1U};
  backend.fallbackTexture2D = DeviceTextureHandle{9000U};
  g_mesh = GpuMesh{};
  g_mesh.geometry = DeviceGeometryHandle{1U};
  g_mesh.vertexCount = 3U;
  g_mesh.indexCount = 3U;

  SceneCaptureRequest requests[2] = {};
  for (SceneCaptureRequest &request : requests) {
    request.width = 64U;
    request.height = 64U;
    request.camera.position = engine::math::Vec3(0.0F, 0.0F, 5.0F);
    request.camera.target = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  }
  set_scene_capture_requests(requests, 2U);

  for (DrawCommand &draw : g_draws) {
    draw = DrawCommand{};
    draw.mesh = MeshHandle{1U};
    draw.material.opacity = 1.0F;
  }
  g_draws[0].material.albedoTexture = scene_capture_texture_handle(0U);
  g_draws[1].material.albedoTexture = scene_capture_texture_handle(1U);
  g_draws[2].material.albedoTexture = kOrdinaryTexture;

  // Targets are created as each capture first renders, so the first frame
  // creates them and the second is the steady state asserted on.
  SceneLightData lights{};
  FrameFlushContext ctx = make_context(lights);
  flush_scene_captures(ctx);
  g_drawCount = 0U;
  flush_scene_captures(ctx);

  const SceneCaptureTarget &first = backend.sceneCaptureTargets[0];
  const SceneCaptureTarget &second = backend.sceneCaptureTargets[1];
  CHECK((first.colorTexture != kInvalidDeviceTexture) &&
            (second.colorTexture != kInvalidDeviceTexture) &&
            (first.colorTexture != second.colorTexture),
        "both captures created their own colour target");
  CHECK(g_drawCount == 2U * kDrawCount,
        "each capture draws every mesh, the self-referencing one included");
  if (g_drawCount != 2U * kDrawCount) {
    set_scene_capture_requests(nullptr, 0U);
    return;
  }

  const DrawRecord *firstPass = &g_drawLog[0];
  const DrawRecord *secondPass = &g_drawLog[kDrawCount];
  for (std::size_t i = 0U; i < kDrawCount; ++i) {
    CHECK(firstPass[i].target == first.target.value,
          "capture 0's draws land on capture 0's target");
    CHECK(secondPass[i].target == second.target.value,
          "capture 1's draws land on capture 1's target");
  }
  CHECK(firstPass[0].albedo == backend.fallbackTexture2D.value,
        "capture 0 draws the mesh showing capture 0 with the fallback");
  CHECK(firstPass[1].albedo == second.colorTexture.value,
        "capture 0 still samples capture 1");
  CHECK(firstPass[2].albedo == kOrdinaryDeviceTexture.value,
        "capture 0 samples an ordinary texture");
  CHECK(secondPass[0].albedo == first.colorTexture.value,
        "capture 1 still samples capture 0");
  CHECK(secondPass[1].albedo == backend.fallbackTexture2D.value,
        "capture 1 draws the mesh showing capture 1 with the fallback");
  CHECK(secondPass[2].albedo == kOrdinaryDeviceTexture.value,
        "capture 1 samples an ordinary texture");

  set_scene_capture_requests(nullptr, 0U);
}

} // namespace

int main() {
  test_capture_never_samples_its_own_target();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("scene capture flush checks passed\n");
  return 0;
}
