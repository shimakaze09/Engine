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
// deferred path, while a genuinely required uniform still does. And it
// pins the dx11 profile's sidecar contract (#301): DXBC uniform tables
// lose Load-only samplers to fxc stripping, so dx11 programs must link
// through the spirv-introspected entry, never the plain one.

#include "command_buffer_capture.h"
#include "command_buffer_context.h"
#include "command_buffer_init_internal.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"

#include "../fake_render_device.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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

std::uint32_t g_nextProgram = 100U;

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

const char *fake_cooked_profile() noexcept { return "spirv"; }
const char *fake_cooked_profile_dx11() noexcept { return "dx11"; }

// Link-entry counters for the dx11 sidecar scenario: plain links, links
// through the introspected entry, and introspected links whose spirv
// sidecar bytes were missing.
std::size_t g_plainLinks = 0U;
std::size_t g_introspectedLinks = 0U;
std::size_t g_introspectedMissingMeta = 0U;

void reset_link_counters() noexcept {
  g_plainLinks = 0U;
  g_introspectedLinks = 0U;
  g_introspectedMissingMeta = 0U;
}

engine::renderer::DeviceProgramHandle
fake_create_program_binary(const void *vs, std::ptrdiff_t,
                           const void *fs, std::ptrdiff_t) noexcept {
  if ((vs == nullptr) || (fs == nullptr)) {
    return engine::renderer::kInvalidDeviceProgram;
  }
  ++g_plainLinks;
  return engine::renderer::DeviceProgramHandle{++g_nextProgram};
}

engine::renderer::DeviceProgramHandle
fake_create_program_binary_introspected(
    const void *vs, std::ptrdiff_t, const void *fs, std::ptrdiff_t,
    const void *vsMeta, std::ptrdiff_t vsMetaSize, const void *fsMeta,
    std::ptrdiff_t fsMetaSize) noexcept {
  if ((vs == nullptr) || (fs == nullptr)) {
    return engine::renderer::kInvalidDeviceProgram;
  }
  ++g_introspectedLinks;
  if ((vsMeta == nullptr) || (vsMetaSize <= 0) || (fsMeta == nullptr) ||
      (fsMetaSize <= 0)) {
    ++g_introspectedMissingMeta;
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

/// Installs the fake function table: the program/parameter seam plus the
/// resource creators the init paths need (skybox geometry, debug line
/// buffers, SSAO noise, shadow targets). Uniform-block entries stay null
/// (and caps.uniformBlocks false) so the skinning family is skipped,
/// keeping the harness scoped to issue #56.
void configure_fake_device() noexcept {
  engine::tests::reset_fake_device();
  engine::renderer::RenderDevice &device = engine::tests::fake_device();
  // Enough sampler units that the deferred capability gate stays open —
  // this harness exercises the reload contract, not device limits.
  device.caps.maxTextureSamplers = 32U;
  device.caps.cookedPrograms = true;
  device.cooked_program_profile = &fake_cooked_profile;
  device.create_program_binary = &fake_create_program_binary;
  device.destroy_program = &fake_destroy_program;
  device.shader_param = &fake_shader_param;
  device.create_buffer = &engine::tests::fake::create_buffer;
  device.destroy_buffer = &engine::tests::fake::destroy_buffer;
  device.update_buffer = &engine::tests::fake::update_buffer;
  device.create_geometry = &engine::tests::fake::create_geometry;
  device.destroy_geometry = &engine::tests::fake::destroy_geometry;
  device.create_texture = &engine::tests::fake::create_texture;
  device.destroy_texture = &engine::tests::fake::destroy_texture;
  device.create_render_target = &engine::tests::fake::create_render_target;
  device.destroy_render_target = &engine::tests::fake::destroy_render_target;
}

bool write_profile_shader_file(const char *fileName, const char *profile,
                               const char *text) noexcept {
  // Cooked layout (#296): the loader reads the bgfx cook's binaries.
  char path[256] = {};
  std::snprintf(path, sizeof(path), "%s/bgfx/cooked/%s.default.%s.bin",
                kShaderDir, fileName, profile);
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

bool write_shader_file(const char *fileName, const char *text) noexcept {
  return write_profile_shader_file(fileName, "spirv", text);
}

/// Advances the file's recorded mtime by a strictly growing number of
/// whole seconds so every edit is observably newer than the previous one
/// (a plain rewrite lands in the same second as the prior bumped stamp)
/// without any wall-clock sleep.
bool bump_shader_mtime(const char *fileName, int seconds) noexcept {
  char path[256] = {};
  std::snprintf(path, sizeof(path), "%s/bgfx/cooked/%s.default.spirv.bin",
                kShaderDir, fileName);
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

/// EXPECTATION (#647): the per-program table the passes bind through
/// follows a reload. Every other cached device program is re-read on
/// refresh; this table was not, so a recook left the flush binding
/// programs the shader system had already destroyed. All three shipped
/// variants cook from one source, so one edit to the fragment stage
/// relinks every one of them and staleness covers every forward draw in
/// the frame rather than one model's — including the physically-based
/// program, which the table holds a second reference to.
int check_shading_programs_follow_a_reload() {
  using namespace engine::renderer;

  reset_backend_harness();
  clear_missing_uniforms();
  if (!initialize_backend()) {
    return 370;
  }

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();
  const std::uint8_t ids[3] = {shading_program_id(ShadingModel::Pbr),
                               shading_program_id(ShadingModel::Toon),
                               shading_program_id(ShadingModel::Unlit)};

  // The positive control. A build where the toon and unlit variants did
  // not register would pass every assertion below while proving nothing,
  // so an unregistered program is a failure of this test rather than a
  // case it skips.
  DeviceProgramHandle before[3] = {};
  for (std::size_t i = 0U; i < 3U; ++i) {
    before[i] = backend.shadingPrograms[ids[i]];
    if (before[i] == kInvalidDeviceProgram) {
      return 371;
    }
    if (backend.shadingProgramShaderHandles[ids[i]] == kInvalidShaderProgram) {
      return 372;
    }
  }
  // Three separate links, so three distinct device programs: if two ids
  // shared one program, a stale entry could read as refreshed.
  if ((before[0] == before[1]) || (before[1] == before[2]) ||
      (before[0] == before[2])) {
    return 373;
  }

  if (!touch_shader_file("pbr.frag")) {
    return 374;
  }
  const std::uint64_t epochBeforeReload = shader_reload_epoch();
  check_shader_reload();
  if (shader_reload_epoch() == epochBeforeReload) {
    return 375; // nothing relinked, so the refresh below proves nothing
  }
  refresh_backend_program_state(backend, dev);

  for (std::size_t i = 0U; i < 3U; ++i) {
    const DeviceProgramHandle now = backend.shadingPrograms[ids[i]];
    if (now == kInvalidDeviceProgram) {
      return 376;
    }
    // The failure this case exists for: the entry still names the
    // program the reload destroyed.
    if (now == before[i]) {
      return 377;
    }
    if (now != shader_device_program(backend.shadingProgramShaderHandles[
            ids[i]])) {
      return 378;
    }
  }
  // The physically-based entry and the program the rest of the backend
  // caches are one fact held twice; a refresh that moved only one of
  // them would shade the same material differently depending on which
  // the pass read.
  if (backend.shadingPrograms[shading_program_id(ShadingModel::Pbr)] !=
      backend.pbrProgram) {
    return 379;
  }
  return 0;
}

/// EXPECTATION: an id past what a draw key can name is refused at
/// registration rather than stored somewhere a draw could reach. The
/// key's field is seven bits, so nothing a real key carries is
/// unaddressable; this pins the refusal for the authored programs #642
/// adds, where the id comes from a document rather than an enumerator.
int check_registration_refuses_what_cannot_be_drawn() {
  using namespace engine::renderer;

  reset_backend_harness();
  clear_missing_uniforms();
  if (!initialize_backend()) {
    return 380;
  }
  BackendState &backend = backend_state();
  const ShaderProgramHandle loaded =
      backend.shadingProgramShaderHandles[shading_program_id(
          ShadingModel::Toon)];
  if (loaded == kInvalidShaderProgram) {
    return 381;
  }

  if (register_shading_program(
          backend, static_cast<std::uint8_t>(kMaxShadingPrograms), loaded) !=
      ShadingProgramRegistration::NotAddressable) {
    return 382;
  }
  if (register_shading_program(backend, 255U, loaded) !=
      ShadingProgramRegistration::NotAddressable) {
    return 383;
  }
  // The last id a key can carry, which is inside the table by one.
  if (register_shading_program(
          backend, static_cast<std::uint8_t>(kMaxShadingPrograms - 1U),
          loaded) != ShadingProgramRegistration::Registered) {
    return 384;
  }
  // A program that did not load leaves its id unregistered: an entry
  // holding nothing is how the flush knows to fall back, so claiming the
  // id with one would be indistinguishable from never registering.
  if (register_shading_program(backend, 5U, kInvalidShaderProgram) !=
      ShadingProgramRegistration::ProgramUnavailable) {
    return 385;
  }
  if (backend.shadingProgramShaderHandles[5] != kInvalidShaderProgram) {
    return 386;
  }
  // A second registration replaces, which is how a reauthored program
  // takes over its own id rather than needing a separate release.
  if (register_shading_program(backend, 5U, loaded) !=
      ShadingProgramRegistration::Registered) {
    return 387;
  }
  if (backend.shadingPrograms[5] != shader_device_program(loaded)) {
    return 388;
  }
  return 0;
}

/// EXPECTATION: the dx11 cooked profile links every program through the
/// introspected entry with both spirv sidecars present, never the plain
/// entry. DXBC uniform tables are incomplete — fxc strips the
/// SamplerState of any texture read only via Load/texelFetch, which
/// silently disabled the deferred path on D3D (#301 hardware run) —
/// so the spirv table must stay the source of the parameter set.
int check_dx11_profile_links_with_spirv_sidecars() {
  using namespace engine::renderer;

  reset_backend_harness();
  clear_missing_uniforms();
  for (const char *fileName : kShaderFiles) {
    if (!write_profile_shader_file(fileName, "dx11", "// dxbc stub\n")) {
      return 360;
    }
  }
  engine::tests::fake_device().cooked_program_profile =
      &fake_cooked_profile_dx11;
  engine::tests::fake_device().create_program_binary_introspected =
      &fake_create_program_binary_introspected;
  reset_link_counters();

  int result = 0;
  if (!initialize_backend()) {
    result = 361;
  } else if (!backend_state().deferredAvailable) {
    result = 362;
  } else if (g_introspectedLinks == 0U) {
    result = 363; // dx11 fell back to the plain, table-stripped entry
  } else if (g_introspectedMissingMeta != 0U) {
    result = 364; // a link ran without its spirv sidecar bytes
  } else if (g_plainLinks != 0U) {
    result = 365;
  }

  engine::tests::fake_device().cooked_program_profile = &fake_cooked_profile;
  engine::tests::fake_device().create_program_binary_introspected = nullptr;
  return result;
}

} // namespace

namespace engine::renderer {

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
  std::filesystem::create_directories(std::string(kShaderDir) +
                                          "/bgfx/cooked",
                                      ec);
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
  if (result == 0) {
    result = check_shading_programs_follow_a_reload();
  }
  if (result == 0) {
    result = check_registration_refuses_what_cannot_be_drawn();
  }
  if (result == 0) {
    result = check_dx11_profile_links_with_spirv_sidecars();
  }

  std::filesystem::remove_all(kShaderDir, ec);
  engine::core::shutdown_vfs();
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  // Every code in this suite is above 255 and a process exit status
  // carries only the low byte, so the status a runner reports is not the
  // code written here. Printing both keeps a failure greppable.
  if (result != 0) {
    std::fprintf(stderr,
                 "command_buffer_reload_contract_test: failed with code %d "
                 "(the exit status shows %d)\n",
                 result, result & 0xFF);
  }
  return result;
}
