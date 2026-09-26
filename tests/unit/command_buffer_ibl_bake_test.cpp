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

#include "../fake_render_device.h"

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

// The render state the bake last applied; the shared fake records the rest.
RenderState g_renderState{};

void fake_apply_render_state(const RenderState &state) noexcept {
  g_renderState = state;
}

} // namespace

/// Installs the device the IBL TU resolves through the shared seam.
void reset_fake_device() noexcept {
  g_renderState = RenderState{};
  tests::reset_fake_device();
  RenderDevice &device = tests::fake_device();
  device.create_texture = &tests::fake::create_texture;
  device.destroy_texture = &tests::fake::destroy_texture;
  device.create_render_target = &tests::fake::create_render_target;
  device.destroy_render_target = &tests::fake::destroy_render_target;
  device.bind_render_target = &tests::fake::bind_render_target;
  device.apply_render_state = &fake_apply_render_state;
  device.bind_texture_slot = &tests::fake::bind_texture_slot;
  device.bind_program = &tests::fake::bind_program;
  device.set_param_i32 = &tests::fake::set_param_i32;
  device.set_param_f32 = &tests::fake::set_param_f32;
  device.set_param_mat4 = &tests::fake::set_param_mat4;
  device.set_viewport = &tests::fake::set_viewport;
  device.draw = &tests::fake::draw;
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
  reset_fake_device();
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
  CHECK(engine::tests::fake_log().boundRenderTarget == 0U, what);
  CHECK(g_renderState.depthTest == DepthTest::Less, "depth test restored");
  CHECK(g_renderState.depthWrite, "depth write restored");
  CHECK(g_renderState.blend == BlendMode::Disabled, "blend restored");
  CHECK(g_renderState.cull == CullMode::Back, "face culling restored");
  CHECK(engine::tests::fake_alive(engine::tests::FakeKind::RenderTarget) == 0,
        "every transient face target destroyed");
}

/// EXPECTATION (audit M-05): a successful prefilter bake draws every
/// face/mip and leaves no bake state on the device.
void test_prefilter_restores_state() noexcept {
  BackendState backend = make_bake_backend();
  const DeviceTextureHandle tex = ensure_prefiltered_environment(
      backend, render_device(), TextureHandle{3U}, DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(tex != kInvalidDeviceTexture, "prefilter bake succeeds");
  CHECK(backend.prefilteredEnvironmentTexture == tex,
        "prefilter result cached");
  CHECK(engine::tests::fake_log().draws > 0, "prefilter bake drew");
  check_state_restored("prefilter leaves the back buffer bound");
}

/// EXPECTATION (audit M-05): a failed face render-target creation aborts
/// the bake, destroys the staged cubemap, caches nothing, and still
/// restores device state.
void test_prefilter_target_failure_fails_clean() noexcept {
  BackendState backend = make_bake_backend();
  engine::tests::fake_log().failKinds =
      engine::tests::fake_kind_bit(engine::tests::FakeKind::RenderTarget);
  const DeviceTextureHandle tex = ensure_prefiltered_environment(
      backend, render_device(), TextureHandle{3U}, DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(tex == kInvalidDeviceTexture,
        "failed face target fails the prefilter bake");
  CHECK(backend.prefilteredEnvironmentTexture == kInvalidDeviceTexture,
        "no prefilter texture cached on failure");
  CHECK(engine::tests::fake_destroys(engine::tests::FakeKind::Texture) == 1,
        "staged cubemap destroyed");
  CHECK(engine::tests::fake_log().draws == 0, "no draws without a face target");
  check_state_restored("failed prefilter leaves the back buffer bound");
}

/// EXPECTATION (audit M-05): the irradiance bake has the same success and
/// failure contracts as the prefilter bake.
void test_irradiance_contracts() noexcept {
  BackendState backend = make_bake_backend();
  const DeviceTextureHandle tex = ensure_irradiance_environment(
      backend, render_device(), TextureHandle{3U}, DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(tex != kInvalidDeviceTexture, "irradiance bake succeeds");
  check_state_restored("irradiance leaves the back buffer bound");

  BackendState failing = make_bake_backend();
  engine::tests::fake_log().failKinds =
      engine::tests::fake_kind_bit(engine::tests::FakeKind::RenderTarget);
  const DeviceTextureHandle failed = ensure_irradiance_environment(
      failing, render_device(), TextureHandle{3U}, DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  CHECK(failed == kInvalidDeviceTexture,
        "failed face target fails the irradiance bake");
  CHECK(failing.irradianceEnvironmentTexture == kInvalidDeviceTexture,
        "no irradiance texture cached on failure");
  CHECK(engine::tests::fake_destroys(engine::tests::FakeKind::Texture) == 1,
        "staged irradiance destroyed");
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

/// A new environment whose device texture reuses the handle of the one it
/// replaced (bgfx recycles destroyed handles) is baked again rather than
/// served the old bake; the same environment again is served from the
/// cache.
void test_bake_cache_follows_the_environment() noexcept {
  BackendState backend = make_bake_backend();
  const DeviceTextureHandle first = ensure_prefiltered_environment(
      backend, render_device(), TextureHandle{3U}, DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  const DeviceTextureHandle firstIrradiance = ensure_irradiance_environment(
      backend, render_device(), TextureHandle{3U}, DeviceTextureHandle{5U},
      ReflectionProbeBakeSettings{});
  const int drawsAfterFirst = engine::tests::fake_log().draws;
  CHECK(ensure_prefiltered_environment(
            backend, render_device(), TextureHandle{3U},
            DeviceTextureHandle{5U}, ReflectionProbeBakeSettings{}) == first,
        "the same environment is served from the cache");
  CHECK(engine::tests::fake_log().draws == drawsAfterFirst,
        "a cached bake draws nothing");

  // The texture slot's next generation: same device handle, new texture.
  const DeviceTextureHandle second = ensure_prefiltered_environment(
      backend, render_device(), TextureHandle{3U + 8192U},
      DeviceTextureHandle{5U}, ReflectionProbeBakeSettings{});
  const DeviceTextureHandle secondIrradiance = ensure_irradiance_environment(
      backend, render_device(), TextureHandle{3U + 8192U},
      DeviceTextureHandle{5U}, ReflectionProbeBakeSettings{});
  CHECK((second != kInvalidDeviceTexture) && (second != first),
        "a new environment on a reused device handle is prefiltered again");
  CHECK((secondIrradiance != kInvalidDeviceTexture) &&
            (secondIrradiance != firstIrradiance),
        "and its irradiance is convolved again");
  CHECK(engine::tests::fake_log().draws > drawsAfterFirst,
        "the new bakes drew");
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
  test_bake_cache_follows_the_environment();
  engine::core::shutdown_cvars();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
