// Verifies the IBL bake helpers against a fake render device (audit
// M-05): every ensure_* bake restores the device to the ambient scene
// state (back buffer bound, opaque-scene render state), a failed
// per-face render-target creation aborts the bake, destroys the staged
// cubemap, and leaves no cached success state, and successful bakes
// destroy every transient face target they created.

#include "command_buffer_context.h"
#include "command_buffer_ibl.h"
#include "command_buffer_sky.h"
#include "engine/core/cvar.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"

#include <cstdint>
#include <cstdio>

namespace engine::renderer {

// Link stubs for the sibling symbols the IBL TU references; the bake
// helpers under test never reach them.
void destroy_shader_program(ShaderProgramHandle) noexcept {}
SkyModel selected_sky_model() noexcept { return SkyModel::Cubemap; }
bool initialize_backend() noexcept { return true; }
DeviceTextureHandle
active_skybox_device_texture(const BackendState &) noexcept {
  return kInvalidDeviceTexture;
}
DeviceTextureHandle texture_device_handle(TextureHandle) noexcept {
  return kInvalidDeviceTexture;
}

namespace {

// Fake device state observed by the tests.
struct FakeDeviceState final {
  std::uint32_t boundTarget = 0U;
  RenderState renderState{};
  std::uint32_t nextResource = 100U;
  std::uint32_t destroyedTextures[8] = {};
  std::size_t destroyedTextureCount = 0U;
  int aliveRenderTargets = 0;
  bool failRenderTargetCreate = false;
  std::size_t drawCalls = 0U;
};

FakeDeviceState g_fake{};

DeviceTextureHandle fake_create_texture(const TextureDesc &) noexcept {
  return DeviceTextureHandle{g_fake.nextResource++};
}
void fake_destroy_texture(DeviceTextureHandle tex) noexcept {
  if (g_fake.destroyedTextureCount < 8U) {
    g_fake.destroyedTextures[g_fake.destroyedTextureCount] = tex.value;
  }
  ++g_fake.destroyedTextureCount;
}
RenderTargetHandle fake_create_render_target(
    const RenderTargetDesc &) noexcept {
  if (g_fake.failRenderTargetCreate) {
    return RenderTargetHandle{};
  }
  ++g_fake.aliveRenderTargets;
  return RenderTargetHandle{g_fake.nextResource++};
}
void fake_destroy_render_target(RenderTargetHandle target) noexcept {
  if (target.value != 0U) {
    --g_fake.aliveRenderTargets;
  }
}
void fake_bind_render_target(RenderTargetHandle target) noexcept {
  g_fake.boundTarget = target.value;
}
void fake_apply_render_state(const RenderState &state) noexcept {
  g_fake.renderState = state;
}
void fake_bind_texture_slot(std::uint32_t, DeviceTextureHandle) noexcept {}
void fake_bind_program(DeviceProgramHandle) noexcept {}
void fake_set_param_i32(ShaderParam, std::int32_t) noexcept {}
void fake_set_param_f32(ShaderParam, float) noexcept {}
void fake_set_param_mat4(ShaderParam, const float *) noexcept {}
void fake_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {}
void fake_draw(DeviceGeometryHandle, PrimitiveTopology, std::int32_t,
               std::int32_t) noexcept {
  ++g_fake.drawCalls;
}

RenderDevice g_device{};

} // namespace

/// Link seam: the IBL TU resolves its device through this override.
const RenderDevice *render_device() noexcept {
  g_device.create_texture = &fake_create_texture;
  g_device.destroy_texture = &fake_destroy_texture;
  g_device.create_render_target = &fake_create_render_target;
  g_device.destroy_render_target = &fake_destroy_render_target;
  g_device.bind_render_target = &fake_bind_render_target;
  g_device.apply_render_state = &fake_apply_render_state;
  g_device.bind_texture_slot = &fake_bind_texture_slot;
  g_device.bind_program = &fake_bind_program;
  g_device.set_param_i32 = &fake_set_param_i32;
  g_device.set_param_f32 = &fake_set_param_f32;
  g_device.set_param_mat4 = &fake_set_param_mat4;
  g_device.set_viewport = &fake_set_viewport;
  g_device.draw = &fake_draw;
  return &g_device;
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

/// Resets the fake device and returns a backend whose prefilter,
/// irradiance, and BRDF-LUT pipelines report available.
BackendState make_bake_backend() noexcept {
  g_fake = FakeDeviceState{};
  BackendState backend{};
  backend.environmentPrefilterAvailable = true;
  backend.environmentPrefilterProgram = DeviceProgramHandle{7U};
  backend.environmentIrradianceAvailable = true;
  backend.environmentIrradianceProgram = DeviceProgramHandle{8U};
  backend.environmentBrdfLutAvailable = true;
  backend.environmentBrdfLutProgram = DeviceProgramHandle{9U};
  backend.skyboxGeometry = DeviceGeometryHandle{3U};
  backend.emptyGeometry = DeviceGeometryHandle{4U};
  return backend;
}

/// Asserts the fake device is back in the ambient scene state.
void check_state_restored(const char *what) noexcept {
  CHECK(g_fake.boundTarget == 0U, what);
  CHECK(g_fake.renderState.depthTest == DepthTest::Less,
        "depth test restored");
  CHECK(g_fake.renderState.depthWrite, "depth write restored");
  CHECK(g_fake.renderState.blend == BlendMode::Disabled, "blend restored");
  CHECK(g_fake.renderState.cull == CullMode::Back, "face culling restored");
  CHECK(g_fake.aliveRenderTargets == 0,
        "every transient face target destroyed");
}

/// EXPECTATION (audit M-05): a successful prefilter bake draws every
/// face/mip and leaves no bake state on the device.
void test_prefilter_restores_state() noexcept {
  BackendState backend = make_bake_backend();
  const DeviceTextureHandle tex = ensure_prefiltered_environment(
      backend, render_device(), DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(tex != kInvalidDeviceTexture, "prefilter bake succeeds");
  CHECK(backend.prefilteredEnvironmentTexture == tex,
        "prefilter result cached");
  CHECK(g_fake.drawCalls > 0U, "prefilter bake drew");
  check_state_restored("prefilter leaves the back buffer bound");
}

/// EXPECTATION (audit M-05): a failed face render-target creation aborts
/// the bake, destroys the staged cubemap, caches nothing, and still
/// restores device state.
void test_prefilter_target_failure_fails_clean() noexcept {
  BackendState backend = make_bake_backend();
  g_fake.failRenderTargetCreate = true;
  const DeviceTextureHandle tex = ensure_prefiltered_environment(
      backend, render_device(), DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(tex == kInvalidDeviceTexture,
        "failed face target fails the prefilter bake");
  CHECK(backend.prefilteredEnvironmentTexture == kInvalidDeviceTexture,
        "no prefilter texture cached on failure");
  CHECK(g_fake.destroyedTextureCount == 1U, "staged cubemap destroyed");
  CHECK(g_fake.drawCalls == 0U, "no draws without a face target");
  check_state_restored("failed prefilter leaves the back buffer bound");
}

/// EXPECTATION (audit M-05): the irradiance bake has the same success and
/// failure contracts as the prefilter bake.
void test_irradiance_contracts() noexcept {
  BackendState backend = make_bake_backend();
  const DeviceTextureHandle tex = ensure_irradiance_environment(
      backend, render_device(), DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(tex != kInvalidDeviceTexture, "irradiance bake succeeds");
  check_state_restored("irradiance leaves the back buffer bound");

  BackendState failing = make_bake_backend();
  g_fake.failRenderTargetCreate = true;
  const DeviceTextureHandle failed = ensure_irradiance_environment(
      failing, render_device(), DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(failed == kInvalidDeviceTexture,
        "failed face target fails the irradiance bake");
  CHECK(failing.irradianceEnvironmentTexture == kInvalidDeviceTexture,
        "no irradiance texture cached on failure");
  CHECK(g_fake.destroyedTextureCount == 1U, "staged irradiance destroyed");
  check_state_restored("failed irradiance leaves the back buffer bound");
}

/// EXPECTATION (audit M-05): the BRDF LUT bake restores the ambient state
/// after rendering and keeps only the LUT texture.
void test_brdf_lut_restores_state() noexcept {
  BackendState backend = make_bake_backend();
  const DeviceTextureHandle tex =
      ensure_brdf_lut(backend, render_device(), ReflectionProbeBakeSettings{});
  CHECK(tex != kInvalidDeviceTexture, "brdf lut bake succeeds");
  CHECK(backend.brdfLutTexture == tex, "brdf lut cached");
  check_state_restored("brdf lut leaves the back buffer bound");
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer IBL Bake Unit Tests ===\n");

  engine::core::initialize_cvars();
  test_prefilter_restores_state();
  test_prefilter_target_failure_fails_clean();
  test_irradiance_contracts();
  test_brdf_lut_restores_state();
  engine::core::shutdown_cvars();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
