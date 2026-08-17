// Verifies the required-parameter reflection contract (issue #56) at the
// renderer/device seam with a fake RenderDevice whose shader_param can
// report chosen names as missing (invalid): a program that links but
// lacks a required parameter leaves its feature family unavailable at
// init, a hot reload that drops a required parameter downgrades the
// family (available → unavailable) through the production
// check_shader_reload + refresh_backend_program_state path, and a
// corrected reload restores it (unavailable → available). Covers a sky
// family (skybox), a shadow family (cascades), a post family (SSAO),
// and the program-id-gated FXAA pass. This closes the reload-regression
// gap acknowledged in PR #52's closure table. Also pins the deferred
// resolver's optional-uniform contract (issue #95): uniforms the lighting
// shader declares but never reads (uTileCountY, uScreenSize) may be
// optimized out by a conforming compiler and must not disable the
// deferred path, while a genuinely required uniform still does.

#include "command_buffer_capture.h"
#include "command_buffer_context.h"
#include "command_buffer_init_internal.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

constexpr const char *kShaderDir = "cb_reload_contract_shaders";
constexpr const char *kShaderMount = "cbshdr";

// Every shader file initialize_backend loads, so no family soft-fails
// for a missing file; the fake device never inspects the source text.
constexpr const char *kShaderFiles[] = {
    "default.vert",         "default.frag",
    "pbr.vert",             "pbr.frag",
    "fullscreen.vert",      "tonemap.frag",
    "skybox.vert",          "skybox.frag",
    "preetham_sky.frag",    "procedural_sky.frag",
    "prefilter_environment.frag", "irradiance_convolution.frag",
    "brdf_lut.frag",        "gbuffer.vert",
    "gbuffer.frag",         "deferred_lighting.frag",
    "gbuffer_debug.frag",   "shadow_depth.vert",
    "shadow_depth.frag",    "shadow_depth_point.vert",
    "shadow_depth_point.frag", "fxaa.frag",
    "bloom_threshold.frag", "bloom_downsample.frag",
    "bloom_upsample.frag",  "ssao.frag",
    "ssao_blur.frag",       "debug_line.vert",
    "debug_line.frag",      "luminance.frag",
};

engine::renderer::RenderDevice g_fakeDevice{};
std::uint32_t g_nextProgram = 100U;
std::uint32_t g_nextResource = 1U;

// Uniform names the fake reports as missing (-1), each scoped to
// programs linked after its marker so a reload's re-linked program loses
// the uniform while untouched programs keep theirs.
struct MissingUniform final {
  const char *name = nullptr;
  std::uint32_t afterProgram = 0U;
};
constexpr std::size_t kMaxMissing = 8U;
MissingUniform g_missing[kMaxMissing] = {};
std::size_t g_missingCount = 0U;

void clear_missing_uniforms() noexcept { g_missingCount = 0U; }

void add_missing_uniform(const char *name,
                         std::uint32_t afterProgram) noexcept {
  if (g_missingCount < kMaxMissing) {
    g_missing[g_missingCount] = MissingUniform{name, afterProgram};
    ++g_missingCount;
  }
}

engine::renderer::DeviceProgramHandle
fake_create_program(const char *vs, const char *fs) noexcept {
  if ((vs == nullptr) || (fs == nullptr)) {
    return engine::renderer::kInvalidDeviceProgram;
  }
  return engine::renderer::DeviceProgramHandle{++g_nextProgram};
}

void fake_destroy_program(engine::renderer::DeviceProgramHandle) noexcept {}

engine::renderer::ShaderParam
fake_shader_param(engine::renderer::DeviceProgramHandle program,
                  const char *name) noexcept {
  for (std::size_t i = 0U; i < g_missingCount; ++i) {
    if ((std::strcmp(name, g_missing[i].name) == 0) &&
        (program.value > g_missing[i].afterProgram)) {
      return engine::renderer::kInvalidShaderParam;
    }
  }
  return engine::renderer::ShaderParam{3};
}

engine::renderer::DeviceBufferHandle
fake_create_buffer(const engine::renderer::BufferDesc &) noexcept {
  return engine::renderer::DeviceBufferHandle{g_nextResource++};
}

void fake_destroy_buffer(engine::renderer::DeviceBufferHandle) noexcept {}

engine::renderer::DeviceGeometryHandle
fake_create_geometry(const engine::renderer::GeometryDesc &) noexcept {
  return engine::renderer::DeviceGeometryHandle{g_nextResource++};
}

void fake_destroy_geometry(engine::renderer::DeviceGeometryHandle) noexcept {}

engine::renderer::DeviceTextureHandle
fake_create_texture(const engine::renderer::TextureDesc &) noexcept {
  return engine::renderer::DeviceTextureHandle{g_nextResource++};
}

void fake_destroy_texture(engine::renderer::DeviceTextureHandle) noexcept {}

engine::renderer::RenderTargetHandle
fake_create_render_target(const engine::renderer::RenderTargetDesc &) noexcept {
  return engine::renderer::RenderTargetHandle{g_nextResource++};
}

void fake_destroy_render_target(engine::renderer::RenderTargetHandle) noexcept {
}

void fake_update_buffer(engine::renderer::DeviceBufferHandle, const void *,
                        std::ptrdiff_t) noexcept {}

/// Installs the fake function table: the program/parameter seam plus the
/// resource creators the init paths need (skybox geometry, debug line
/// buffers, SSAO noise, shadow targets). Uniform-block entries stay null
/// (and caps.uniformBlocks false) so the skinning family is skipped,
/// keeping the harness scoped to issue #56.
void configure_fake_device() noexcept {
  g_fakeDevice = engine::renderer::RenderDevice{};
  g_fakeDevice.create_program = &fake_create_program;
  g_fakeDevice.destroy_program = &fake_destroy_program;
  g_fakeDevice.shader_param = &fake_shader_param;
  g_fakeDevice.create_buffer = &fake_create_buffer;
  g_fakeDevice.destroy_buffer = &fake_destroy_buffer;
  g_fakeDevice.update_buffer = &fake_update_buffer;
  g_fakeDevice.create_geometry = &fake_create_geometry;
  g_fakeDevice.destroy_geometry = &fake_destroy_geometry;
  g_fakeDevice.create_texture = &fake_create_texture;
  g_fakeDevice.destroy_texture = &fake_destroy_texture;
  g_fakeDevice.create_render_target = &fake_create_render_target;
  g_fakeDevice.destroy_render_target = &fake_destroy_render_target;
}

bool write_shader_file(const char *fileName, const char *text) noexcept {
  char path[256] = {};
  std::snprintf(path, sizeof(path), "%s/%s", kShaderDir, fileName);
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, len, file) == len;
  std::fclose(file);
  return ok;
}

/// Advances the file's recorded mtime by a strictly growing number of
/// whole seconds so every edit is observably newer than the previous one
/// (a plain rewrite lands in the same second as the prior bumped stamp)
/// without any wall-clock sleep.
bool bump_shader_mtime(const char *fileName, int seconds) noexcept {
  char path[256] = {};
  std::snprintf(path, sizeof(path), "%s/%s", kShaderDir, fileName);
  std::error_code ec{};
  const auto current = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  std::filesystem::last_write_time(
      path, current + std::chrono::seconds(seconds), ec);
  return !ec;
}

/// Rewrites the file with fresh content and bumps its mtime so the next
/// check_shader_reload re-links its program through the fake device.
bool touch_shader_file(const char *fileName) noexcept {
  static int edit = 0;
  char text[64] = {};
  std::snprintf(text, sizeof(text), "// edit %d\n", ++edit);
  return write_shader_file(fileName, text) &&
         bump_shader_mtime(fileName, edit + 1);
}

/// Resets the backend singleton and shader system so initialize_backend
/// can run again from a clean slate within one process.
void reset_backend_harness() noexcept {
  engine::renderer::backend_state() = engine::renderer::BackendState{};
  engine::renderer::shutdown_shader_system();
}

/// EXPECTATION: a program that links while missing a required uniform
/// leaves its family unavailable at init and cleans up its soft-fail
/// state, while unaffected families initialize normally.
int check_init_with_missing_required_uniform() {
  using namespace engine::renderer;

  clear_missing_uniforms();
  add_missing_uniform("u_skybox", 0U);
  add_missing_uniform("u_lightMVP", 0U);
  add_missing_uniform("u_noiseScale", 0U);

  if (!initialize_backend()) {
    return 301;
  }
  const BackendState &backend = backend_state();

  if (backend.skyboxAvailable) {
    return 302;
  }
  if (backend.shadowAvailable || backend.spotShadowAvailable) {
    return 303;
  }
  if (backend.shadowDepthShaderHandle != kInvalidShaderProgram) {
    return 304;
  }
  if (backend.pointShadowAvailable) {
    return 305;
  }
  if (backend.ssaoAvailable) {
    return 306;
  }
  if (backend.ssaoNoiseTexture != kInvalidDeviceTexture) {
    return 307;
  }

  if (!backend.preethamSkyAvailable || !backend.hosekSkyAvailable) {
    return 308;
  }
  if (!backend.environmentPrefilterAvailable ||
      !backend.environmentIrradianceAvailable ||
      !backend.environmentBrdfLutAvailable) {
    return 309;
  }
  if (!backend.deferredAvailable) {
    return 310;
  }
  if (!backend.debugLineAvailable || !backend.autoExposureAvailable) {
    return 311;
  }
  if ((backend.fxaaProgram == kInvalidDeviceProgram) ||
      (backend.bloomThresholdProgram == kInvalidDeviceProgram) ||
      (backend.bloomDownsampleProgram == kInvalidDeviceProgram) ||
      (backend.bloomUpsampleProgram == kInvalidDeviceProgram)) {
    return 312;
  }
  return 0;
}

/// EXPECTATION: after a healthy init, a reload that drops a required
/// uniform makes exactly the affected families unavailable on refresh,
/// and a corrected reload makes them available again — both directions
/// through check_shader_reload (epoch advances via the production swap)
/// and refresh_backend_program_state.
int check_reload_transitions() {
  using namespace engine::renderer;

  reset_backend_harness();
  clear_missing_uniforms();
  if (!initialize_backend()) {
    return 320;
  }

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();
  if (!backend.skyboxAvailable || !backend.shadowAvailable ||
      !backend.spotShadowAvailable || !backend.pointShadowAvailable ||
      !backend.ssaoAvailable ||
      (backend.fxaaProgram == kInvalidDeviceProgram)) {
    return 321;
  }

  const std::uint32_t reloadMarker = g_nextProgram;
  add_missing_uniform("u_skybox", reloadMarker);
  add_missing_uniform("u_lightMVP", reloadMarker);
  add_missing_uniform("u_noiseScale", reloadMarker);
  add_missing_uniform("u_texelSize", reloadMarker);

  if (!touch_shader_file("skybox.frag") ||
      !touch_shader_file("shadow_depth.vert") ||
      !touch_shader_file("ssao.frag") || !touch_shader_file("fxaa.frag")) {
    return 322;
  }
  const std::uint64_t epochBeforeBreak = shader_reload_epoch();
  check_shader_reload();
  if (shader_reload_epoch() == epochBeforeBreak) {
    return 323;
  }
  refresh_backend_program_state(backend, dev);

  if (backend.skyboxAvailable) {
    return 324;
  }
  if (backend.shadowAvailable || backend.spotShadowAvailable) {
    return 325;
  }
  if (backend.ssaoAvailable) {
    return 326;
  }
  if (backend.fxaaProgram != kInvalidDeviceProgram) {
    return 327;
  }
  if (!backend.pointShadowAvailable) {
    return 328;
  }
  if (!backend.preethamSkyAvailable || !backend.hosekSkyAvailable ||
      !backend.deferredAvailable || !backend.debugLineAvailable ||
      !backend.autoExposureAvailable) {
    return 329;
  }
  if ((backend.skyboxShaderHandle == kInvalidShaderProgram) ||
      (backend.shadowDepthShaderHandle == kInvalidShaderProgram) ||
      (backend.ssaoShaderHandle == kInvalidShaderProgram) ||
      (backend.fxaaShaderHandle == kInvalidShaderProgram)) {
    return 330;
  }

  clear_missing_uniforms();
  if (!touch_shader_file("skybox.frag") ||
      !touch_shader_file("shadow_depth.vert") ||
      !touch_shader_file("ssao.frag") || !touch_shader_file("fxaa.frag")) {
    return 331;
  }
  const std::uint64_t epochBeforeFix = shader_reload_epoch();
  check_shader_reload();
  if (shader_reload_epoch() == epochBeforeFix) {
    return 332;
  }
  refresh_backend_program_state(backend, dev);

  if (!backend.skyboxAvailable) {
    return 333;
  }
  if (!backend.shadowAvailable || !backend.spotShadowAvailable) {
    return 334;
  }
  if (!backend.ssaoAvailable) {
    return 335;
  }
  if (backend.fxaaProgram == kInvalidDeviceProgram) {
    return 336;
  }
  if (!backend.pointShadowAvailable || !backend.deferredAvailable) {
    return 337;
  }
  return 0;
}

/// EXPECTATION (issue #95): a driver that strips the declared-but-unread
/// uTileCountY/uScreenSize uniforms (-1 from glGetUniformLocation) leaves
/// the deferred path available, while a missing genuinely required
/// deferred uniform (uTileCountX) still disables it.
int check_deferred_survives_optimized_out_uniforms() {
  using namespace engine::renderer;

  reset_backend_harness();
  clear_missing_uniforms();
  add_missing_uniform("uTileCountY", 0U);
  add_missing_uniform("uScreenSize", 0U);
  if (!initialize_backend()) {
    return 340;
  }
  if (!backend_state().deferredAvailable) {
    return 341;
  }

  reset_backend_harness();
  clear_missing_uniforms();
  add_missing_uniform("uTileCountX", 0U);
  if (!initialize_backend()) {
    return 342;
  }
  if (backend_state().deferredAvailable) {
    return 343;
  }
  return 0;
}

} // namespace

namespace engine::renderer {

bool initialize_render_device() noexcept { return true; }

void shutdown_render_device() noexcept {}

const RenderDevice *render_device() noexcept { return &g_fakeDevice; }

bool initialize_gpu_profiler() noexcept { return true; }

void shutdown_gpu_profiler() noexcept {}

void destroy_scene_capture_targets(BackendState &,
                                   const RenderDevice *) noexcept {}

} // namespace engine::renderer

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging()) {
    return 400;
  }
  if (!engine::core::initialize_cvars()) {
    engine::core::shutdown_logging();
    return 401;
  }
  if (!engine::core::initialize_vfs()) {
    engine::core::shutdown_cvars();
    engine::core::shutdown_logging();
    return 402;
  }

  int result = 0;
  std::error_code ec{};
  std::filesystem::create_directories(kShaderDir, ec);
  if (ec) {
    result = 403;
  }
  if ((result == 0) && !engine::core::mount(kShaderMount, kShaderDir)) {
    result = 404;
  }
  if (result == 0) {
    for (const char *fileName : kShaderFiles) {
      if (!write_shader_file(fileName, "// stub\n")) {
        result = 405;
        break;
      }
    }
  }

  if (result == 0) {
    configure_fake_device();
    engine::renderer::set_shader_root_path(kShaderMount);
    result = check_init_with_missing_required_uniform();
  }
  if (result == 0) {
    result = check_reload_transitions();
  }
  if (result == 0) {
    result = check_deferred_survives_optimized_out_uniforms();
  }

  std::filesystem::remove_all(kShaderDir, ec);
  engine::core::shutdown_vfs();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  return result;
}
