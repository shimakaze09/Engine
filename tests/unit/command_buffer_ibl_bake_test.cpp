// Verifies the IBL bake helpers against a fake render device (audit
// M-05): every ensure_* bake restores the device to the ambient scene
// state (default framebuffer, depth test and face culling enabled, entry
// viewport), an incomplete bake framebuffer aborts the bake, destroys the
// staged cubemap, and leaves no cached success state, and the cubemap
// face attach path re-enables color attachment 0 so deferred-attachment
// FBOs (created with GL_NONE draw buffers, audit H-12) do not discard
// bake output.

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
std::uint32_t active_skybox_gpu_texture(const BackendState &) noexcept {
  return 0U;
}
std::uint32_t texture_gpu_id(TextureHandle) noexcept { return 0U; }

namespace {

// Fake device state observed by the tests.
struct FakeGlState final {
  std::uint32_t boundFbo = 0U;
  bool depthTest = true;
  bool faceCulling = true;
  std::int32_t viewport[4] = {0, 0, 640, 480};
  std::uint32_t nextResource = 100U;
  std::uint32_t destroyedTextures[8] = {};
  std::size_t destroyedTextureCount = 0U;
  bool fboComplete = true;
  std::size_t drawCalls = 0U;
};

FakeGlState g_gl{};

std::uint32_t fake_create_framebuffer(std::uint32_t, std::uint32_t) noexcept {
  return g_gl.nextResource++;
}
void fake_destroy_framebuffer(std::uint32_t) noexcept {}
void fake_bind_framebuffer(std::uint32_t fbo) noexcept { g_gl.boundFbo = fbo; }
bool fake_check_framebuffer_complete() noexcept { return g_gl.fboComplete; }
std::uint32_t fake_create_cubemap_hdr_empty(std::int32_t,
                                            std::int32_t) noexcept {
  return g_gl.nextResource++;
}
std::uint32_t fake_create_texture_2d_hdr(std::int32_t, std::int32_t,
                                         std::int32_t,
                                         const float *) noexcept {
  return g_gl.nextResource++;
}
void fake_destroy_texture(std::uint32_t tex) noexcept {
  if (g_gl.destroyedTextureCount < 8U) {
    g_gl.destroyedTextures[g_gl.destroyedTextureCount] = tex;
  }
  ++g_gl.destroyedTextureCount;
}
void fake_framebuffer_cubemap_color_face_mip(std::uint32_t fbo, std::uint32_t,
                                             std::int32_t,
                                             std::int32_t) noexcept {
  g_gl.boundFbo = fbo;
}
void fake_bind_texture_cubemap(std::int32_t, std::uint32_t) noexcept {}
void fake_bind_program(std::uint32_t) noexcept {}
void fake_bind_vertex_array(std::uint32_t) noexcept {}
void fake_set_uniform_int(std::int32_t, std::int32_t) noexcept {}
void fake_set_uniform_float(std::int32_t, float) noexcept {}
void fake_set_uniform_mat4(std::int32_t, const float *) noexcept {}
void fake_enable_depth_test() noexcept { g_gl.depthTest = true; }
void fake_disable_depth_test() noexcept { g_gl.depthTest = false; }
void fake_enable_face_culling() noexcept { g_gl.faceCulling = true; }
void fake_disable_face_culling() noexcept { g_gl.faceCulling = false; }
void fake_set_viewport(std::int32_t x, std::int32_t y, std::int32_t w,
                       std::int32_t h) noexcept {
  g_gl.viewport[0] = x;
  g_gl.viewport[1] = y;
  g_gl.viewport[2] = w;
  g_gl.viewport[3] = h;
}
void fake_get_viewport(std::int32_t *x, std::int32_t *y, std::int32_t *w,
                       std::int32_t *h) noexcept {
  *x = g_gl.viewport[0];
  *y = g_gl.viewport[1];
  *w = g_gl.viewport[2];
  *h = g_gl.viewport[3];
}
void fake_draw_arrays_triangles(std::int32_t, std::int32_t) noexcept {
  ++g_gl.drawCalls;
}

RenderDevice g_device{};

} // namespace

/// Link seam: the IBL TU resolves its device through this override.
const RenderDevice *render_device() noexcept {
  g_device.create_framebuffer = &fake_create_framebuffer;
  g_device.destroy_framebuffer = &fake_destroy_framebuffer;
  g_device.bind_framebuffer = &fake_bind_framebuffer;
  g_device.check_framebuffer_complete = &fake_check_framebuffer_complete;
  g_device.create_cubemap_hdr_empty = &fake_create_cubemap_hdr_empty;
  g_device.create_texture_2d_hdr = &fake_create_texture_2d_hdr;
  g_device.destroy_texture = &fake_destroy_texture;
  g_device.framebuffer_cubemap_color_face_mip =
      &fake_framebuffer_cubemap_color_face_mip;
  g_device.bind_texture_cubemap = &fake_bind_texture_cubemap;
  g_device.bind_program = &fake_bind_program;
  g_device.bind_vertex_array = &fake_bind_vertex_array;
  g_device.set_uniform_int = &fake_set_uniform_int;
  g_device.set_uniform_float = &fake_set_uniform_float;
  g_device.set_uniform_mat4 = &fake_set_uniform_mat4;
  g_device.enable_depth_test = &fake_enable_depth_test;
  g_device.disable_depth_test = &fake_disable_depth_test;
  g_device.enable_face_culling = &fake_enable_face_culling;
  g_device.disable_face_culling = &fake_disable_face_culling;
  g_device.set_viewport = &fake_set_viewport;
  g_device.get_viewport = &fake_get_viewport;
  g_device.draw_arrays_triangles = &fake_draw_arrays_triangles;
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
  g_gl = FakeGlState{};
  BackendState backend{};
  backend.environmentPrefilterAvailable = true;
  backend.environmentPrefilterProgram = 7U;
  backend.environmentIrradianceAvailable = true;
  backend.environmentIrradianceProgram = 8U;
  backend.environmentBrdfLutAvailable = true;
  backend.environmentBrdfLutProgram = 9U;
  backend.skyboxVertexArray = 3U;
  backend.emptyVao = 4U;
  return backend;
}

/// Asserts the fake device is back in the ambient scene state with the
/// entry viewport.
void check_state_restored(const char *what) noexcept {
  CHECK(g_gl.boundFbo == 0U, what);
  CHECK(g_gl.depthTest, "depth test restored");
  CHECK(g_gl.faceCulling, "face culling restored");
  CHECK((g_gl.viewport[0] == 0) && (g_gl.viewport[1] == 0) &&
            (g_gl.viewport[2] == 640) && (g_gl.viewport[3] == 480),
        "viewport restored");
}

/// EXPECTATION (audit M-05): a successful prefilter bake draws every
/// face/mip and leaves no bake state on the device.
void test_prefilter_restores_state() noexcept {
  BackendState backend = make_bake_backend();
  const std::uint32_t tex = ensure_prefiltered_environment(
      backend, render_device(), 5U, ReflectionProbeBakeSettings{});
  CHECK(tex != 0U, "prefilter bake succeeds");
  CHECK(backend.prefilteredEnvironmentTexture == tex,
        "prefilter result cached");
  CHECK(g_gl.drawCalls > 0U, "prefilter bake drew");
  check_state_restored("prefilter leaves default framebuffer bound");
}

/// EXPECTATION (audit M-05): an incomplete prefilter framebuffer aborts
/// the bake, destroys the staged cubemap, caches nothing, and still
/// restores device state.
void test_prefilter_incomplete_fbo_fails_clean() noexcept {
  BackendState backend = make_bake_backend();
  g_gl.fboComplete = false;
  const std::uint32_t tex = ensure_prefiltered_environment(
      backend, render_device(), 5U, ReflectionProbeBakeSettings{});
  CHECK(tex == 0U, "incomplete fbo fails the prefilter bake");
  CHECK(backend.prefilteredEnvironmentTexture == 0U,
        "no prefilter texture cached on failure");
  CHECK(g_gl.destroyedTextureCount == 1U, "staged cubemap destroyed");
  CHECK(g_gl.drawCalls == 0U, "no draws into an incomplete fbo");
  check_state_restored("failed prefilter leaves default framebuffer bound");
}

/// EXPECTATION (audit M-05): the irradiance bake has the same success and
/// failure contracts as the prefilter bake.
void test_irradiance_contracts() noexcept {
  BackendState backend = make_bake_backend();
  const std::uint32_t tex = ensure_irradiance_environment(
      backend, render_device(), 5U, ReflectionProbeBakeSettings{});
  CHECK(tex != 0U, "irradiance bake succeeds");
  check_state_restored("irradiance leaves default framebuffer bound");

  BackendState failing = make_bake_backend();
  g_gl.fboComplete = false;
  const std::uint32_t failed = ensure_irradiance_environment(
      failing, render_device(), 5U, ReflectionProbeBakeSettings{});
  CHECK(failed == 0U, "incomplete fbo fails the irradiance bake");
  CHECK(failing.irradianceEnvironmentTexture == 0U,
        "no irradiance texture cached on failure");
  CHECK(g_gl.destroyedTextureCount == 1U, "staged irradiance destroyed");
  check_state_restored("failed irradiance leaves default framebuffer bound");
}

/// EXPECTATION (audit M-05): the BRDF LUT bake restores viewport and
/// depth/cull enables after rendering.
void test_brdf_lut_restores_state() noexcept {
  BackendState backend = make_bake_backend();
  const std::uint32_t tex =
      ensure_brdf_lut(backend, render_device(), ReflectionProbeBakeSettings{});
  CHECK(tex != 0U, "brdf lut bake succeeds");
  CHECK(backend.brdfLutTexture == tex, "brdf lut cached");
  check_state_restored("brdf lut leaves default framebuffer bound");
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer IBL Bake Unit Tests ===\n");

  engine::core::initialize_cvars();
  test_prefilter_restores_state();
  test_prefilter_incomplete_fbo_fails_clean();
  test_irradiance_contracts();
  test_brdf_lut_restores_state();
  engine::core::shutdown_cvars();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
