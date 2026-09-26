// Verifies the production spot and point shadow passes (flush_shadow_passes)
// against a fake render device: a light flagged castShadow gets a depth
// pass (its slot's target bound, cleared, and the opaque draw list drawn
// into it; six faces for a point light), an unflagged light gets none, and
// the flush reports the passes as active for the lighting binds. Until
// #522 no producer set the flag, so these passes never ran in any scene.
// Auxiliary (camera-culled) casters are drawn into the same passes (#524).
// A mask-mode caster with an opacity mask is drawn through the MASKED
// program with its mask, cutoff and UV transform bound, in the spot and the
// point passes alike, and every other caster through the pass's own.
// The directional cascades are cached, and reused only while the casters'
// resolved geometry and masks are unchanged.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/core/cvar.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shadow_map.h"

#include "../fake_render_device.h"

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
  /// Per depth draw, in order: the program and the texture on unit 0 bound
  /// when it was issued, and the last cutoff uploaded before it.
  static constexpr std::size_t kMaxDraws = 64U;
  std::uint32_t drawPrograms[kMaxDraws] = {};
  std::uint32_t drawMasks[kMaxDraws] = {};
  float drawCutoffs[kMaxDraws] = {};
  std::uint32_t currentProgram = 0U;
  std::uint32_t unit0Texture = 0U;
  float cutoff = -1.0F;
};

/// The fake param slot the MASKED programs' cutoff resolves to.
constexpr engine::renderer::ShaderParam kCutoffParam{40};

FakeDeviceLog g_log{};
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
    if (g_log.drawsOnTargets < FakeDeviceLog::kMaxDraws) {
      g_log.drawPrograms[g_log.drawsOnTargets] = g_log.currentProgram;
      g_log.drawMasks[g_log.drawsOnTargets] = g_log.unit0Texture;
      g_log.drawCutoffs[g_log.drawsOnTargets] = g_log.cutoff;
    }
    ++g_log.drawsOnTargets;
  } else {
    ++g_log.drawsOnBackBuffer;
  }
}
void fake_draw_indexed(DeviceGeometryHandle, std::int32_t) noexcept {
  fake_draw(DeviceGeometryHandle{}, PrimitiveTopology::Triangles, 0, 0);
}
void fake_bind_program(DeviceProgramHandle program) noexcept {
  g_log.currentProgram = program.value;
}
void fake_bind_texture_slot(std::uint32_t slot,
                            DeviceTextureHandle texture) noexcept {
  if (slot == 0U) {
    g_log.unit0Texture = texture.value;
  }
}
void fake_set_param_f32(ShaderParam param, float value) noexcept {
  if (param == kCutoffParam) {
    g_log.cutoff = value;
  }
}
/// Installs the fake device table and clears its log.
void reset_fake_device() noexcept {
  g_log = FakeDeviceLog{};
  tests::reset_fake_device();
  RenderDevice &device = tests::fake_device();
  device.bind_render_target = &fake_bind_render_target;
  device.clear = &fake_clear;
  device.draw = &fake_draw;
  device.draw_indexed = &fake_draw_indexed;
  device.bind_program = &fake_bind_program;
  device.bind_texture_slot = &fake_bind_texture_slot;
  device.set_param_f32 = &fake_set_param_f32;
  device.set_param_i32 = &tests::fake::set_param_i32;
  device.set_param_vec2 = &tests::fake::set_param_vec2;
  device.set_param_mat4 = &tests::fake::set_param_mat4;
  device.set_param_vec3 = &tests::fake::set_param_vec3;
  device.set_viewport = &tests::fake::set_viewport;
  device.apply_render_state = &tests::fake::apply_render_state;
}

} // namespace

// Link seams for the shadow flush TU: the profiler, the mesh registry
// lookup (every handle resolves to one unskinned mesh) and the skinning
// helpers the unskinned draws never reach.
void gpu_profiler_begin_pass(GpuPassId) noexcept {}
void gpu_profiler_end_pass(GpuPassId) noexcept {}
bool g_meshResolves = true;
const GpuMesh *lookup_gpu_mesh(const GpuMeshRegistry *, MeshHandle) noexcept {
  return g_meshResolves ? &g_mesh : nullptr;
}
bool g_paletteUploads = false;
bool upload_bone_palette(BackendState &, const RenderDevice *, std::uint32_t,
                         ShaderParam, std::uint32_t *) noexcept {
  return g_paletteUploads;
}
std::size_t skin_palette_count() noexcept { return 0U; }
/// Every live texture handle maps to a device texture 1000 above its id,
/// or to none while the textures are still loading.
bool g_texturesResolve = true;
DeviceTextureHandle texture_device_handle(TextureHandle handle) noexcept {
  return ((handle.id == 0U) || !g_texturesResolve)
             ? kInvalidDeviceTexture
             : DeviceTextureHandle{1000U + handle.id};
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

/// EXPECTATION (#565 row 3): the four shadow slots go to the four casters
/// nearest the camera, in order of distance, whatever order the lights were
/// created in. Here distance falls as the index rises, so the slots must
/// hold the highest indices, nearest first.
void test_slots_go_to_the_nearest_casters() noexcept {
  reset_backend();
  reset_fake_device();
  const engine::math::Vec3 camera = renderer_context().activeCamera.position;
  SceneLightData lights{};
  lights.spotLightCount = 10U;
  lights.pointLightCount = 10U;
  for (std::size_t i = 0U; i < 10U; ++i) {
    const float distance = 40.0F - 3.0F * static_cast<float>(i);
    lights.spotLights[i].castShadow = true;
    lights.spotLights[i].radius = 12.0F;
    lights.spotLights[i].position =
        engine::math::Vec3(camera.x + distance, camera.y, camera.z);
    lights.pointLights[i].castShadow = true;
    lights.pointLights[i].radius = 12.0F;
    lights.pointLights[i].position =
        engine::math::Vec3(camera.x, camera.y + distance, camera.z);
  }
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  for (int s = 0; s < 4; ++s) {
    CHECK(g_backend.spotShadowState.slots[s].lightIndex == (9 - s),
          "spot slots hold the nearest casters, nearest first");
    CHECK(g_backend.pointShadowState.slots[s].lightIndex == (9 - s),
          "point slots hold the nearest casters, nearest first");
  }
}

/// EXPECTATION (#565 row 3): casters at exactly the same distance take the
/// slots in index order, so which lights cast is a function of the scene
/// and not of how a particular std::sort happens to order equal keys. The
/// even-indexed lights all sit at one point, so their distances are the
/// same float, and they are the nearest; the odd-indexed ones are scattered
/// further out. Interleaving them is what makes the sort move things: a
/// range of nothing but equal keys is left untouched by the libraries
/// tried, and would pass with or without a tie-break. On base the
/// comparator looked at distance alone.
void test_equidistant_casters_take_slots_in_index_order() noexcept {
  reset_backend();
  reset_fake_device();
  const engine::math::Vec3 camera = renderer_context().activeCamera.position;
  SceneLightData lights{};
  lights.spotLightCount = static_cast<std::uint32_t>(kMaxSpotLights);
  lights.pointLightCount = static_cast<std::uint32_t>(kMaxPointLights);
  const auto place = [&camera](std::size_t index) noexcept {
    if ((index % 2U) == 0U) {
      return engine::math::Vec3(camera.x + 3.0F, camera.y, camera.z);
    }
    // A fixed scramble of the odd indices over 10 to 73 metres, all
    // further than the tied group at 3.
    const float distance =
        10.0F + static_cast<float>((index * 37U + 11U) % 64U);
    return engine::math::Vec3(camera.x, camera.y + distance, camera.z);
  };
  for (std::size_t i = 0U; i < kMaxSpotLights; ++i) {
    lights.spotLights[i].castShadow = true;
    lights.spotLights[i].radius = 12.0F;
    lights.spotLights[i].position = place(i);
  }
  for (std::size_t i = 0U; i < kMaxPointLights; ++i) {
    lights.pointLights[i].castShadow = true;
    lights.pointLights[i].radius = 12.0F;
    lights.pointLights[i].position = place(i);
  }
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  for (int s = 0; s < 4; ++s) {
    CHECK(g_backend.spotShadowState.slots[s].lightIndex == (2 * s),
          "tied spot casters take the slots in index order");
    CHECK(g_backend.pointShadowState.slots[s].lightIndex == (2 * s),
          "tied point casters take the slots in index order");
  }
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
/// A MASKED program with every uniform resolved to a fake param.
MaskedShadowProgram fake_masked_program(std::uint32_t program) noexcept {
  MaskedShadowProgram masked{};
  masked.program = DeviceProgramHandle{program};
  masked.lightMvpLoc = ShaderParam{41};
  masked.modelLoc = ShaderParam{42};
  masked.lightPosLoc = ShaderParam{43};
  masked.farPlaneLoc = ShaderParam{44};
  masked.opacityMaskLoc = ShaderParam{45};
  masked.alphaCutoffLoc = kCutoffParam;
  masked.uvTilingLoc = ShaderParam{46};
  masked.uvOffsetLoc = ShaderParam{47};
  return masked;
}

/// EXPECTATION (#693): with the MASKED programs available, a mask-mode
/// caster with an opacity mask draws through them, with its mask on unit 0
/// and its cutoff uploaded, into the spot map and every point face; the
/// opaque caster keeps the pass's own program. On base every caster drew
/// through the empty-fragment program and cast a full silhouette.
void test_masked_casters_draw_through_the_masked_programs() noexcept {
  reset_backend();
  reset_fake_device();
  g_backend.shadowMasked = fake_masked_program(11U);
  g_backend.shadowPointMasked = fake_masked_program(12U);
  g_draws[1].material.alphaMode = AlphaMode::Mask;
  g_draws[1].material.alphaCutoff = 0.25F;
  g_draws[1].material.opacityTexture = TextureHandle{5U};
  SceneLightData lights{};
  lights.spotLightCount = 1U;
  lights.spotLights[0].castShadow = true;
  lights.pointLightCount = 1U;
  lights.pointLights[0].castShadow = true;
  lights.pointLights[0].radius = 8.0F;
  FrameFlushContext ctx = make_context(lights);
  flush_shadow_passes(ctx);
  CHECK(g_log.drawsOnTargets == 14U, "two draws into the spot map and each "
                                     "of the six faces");
  const std::size_t draws = (g_log.drawsOnTargets < FakeDeviceLog::kMaxDraws)
                                ? g_log.drawsOnTargets
                                : 0U;
  for (std::size_t i = 0U; i < draws; ++i) {
    const bool spot = i < 2U;
    const bool maskedDraw = (i % 2U) == 1U;
    const std::uint32_t base = spot ? 1U : 2U;
    const std::uint32_t masked = spot ? 11U : 12U;
    CHECK(g_log.drawPrograms[i] == (maskedDraw ? masked : base),
          "each caster draws through the program its material needs");
    if (maskedDraw) {
      CHECK(g_log.drawMasks[i] == 1005U, "the caster's mask is on unit 0");
      CHECK(g_log.drawCutoffs[i] == 0.25F, "the caster's cutoff is uploaded");
    }
  }
  g_draws[1].material = Material{};
}

/// EXPECTATION (#693): a caster casts a full silhouette through the pass's
/// own program when the lit passes would not cut it either (mask mode with
/// no mask texture, blend mode) and when no MASKED program is available.
void test_unmasked_casters_keep_the_pass_program() noexcept {
  const struct {
    AlphaMode mode;
    std::uint32_t mask;
    bool maskedAvailable;
    const char *what;
  } cases[] = {
      {AlphaMode::Mask, 0U, true, "mask mode with no mask texture"},
      {AlphaMode::Blend, 5U, true, "blend mode"},
      {AlphaMode::Mask, 5U, false, "a masked caster with no MASKED program"},
  };
  for (const auto &row : cases) {
    reset_backend();
    reset_fake_device();
    if (row.maskedAvailable) {
      g_backend.shadowMasked = fake_masked_program(11U);
    }
    g_draws[1].material.alphaMode = row.mode;
    g_draws[1].material.opacityTexture = TextureHandle{row.mask};
    SceneLightData lights{};
    lights.spotLightCount = 1U;
    lights.spotLights[0].castShadow = true;
    FrameFlushContext ctx = make_context(lights);
    flush_shadow_passes(ctx);
    CHECK((g_log.drawsOnTargets == 2U) && (g_log.drawPrograms[0] == 1U) &&
              (g_log.drawPrograms[1] == 1U),
          row.what);
  }
  g_draws[1].material = Material{};
}

/// EXPECTATION (#693): a posed skinned caster with a mask draws through the
/// skinned MASKED program; without it, through the skinned program (the
/// pose ahead of the mask); and when its palette cannot upload, through the
/// static MASKED program, never through a program left bound by the failed
/// upload.
void test_skinned_masked_casters_pick_the_fitting_program() noexcept {
  const struct {
    bool skinnedMaskedAvailable;
    bool paletteUploads;
    std::uint32_t expected;
    const char *what;
  } cases[] = {
      {true, true, 13U,
       "a posed masked caster uses the skinned MASKED program"},
      {false, true, 3U, "without it, the skinned program keeps the pose"},
      {true, false, 11U,
       "a palette that cannot upload falls to the static "
       "MASKED program"},
  };
  for (const auto &row : cases) {
    reset_backend();
    reset_fake_device();
    g_backend.shadowDepthSkinnedProgram = DeviceProgramHandle{3U};
    g_backend.shadowMasked = fake_masked_program(11U);
    if (row.skinnedMaskedAvailable) {
      g_backend.shadowSkinnedMasked = fake_masked_program(13U);
    }
    g_paletteUploads = row.paletteUploads;
    g_mesh.hasSkin = true;
    g_draws[1].skinPalette = 0U;
    g_draws[1].material.alphaMode = AlphaMode::Mask;
    g_draws[1].material.opacityTexture = TextureHandle{5U};
    SceneLightData lights{};
    lights.spotLightCount = 1U;
    lights.spotLights[0].castShadow = true;
    FrameFlushContext ctx = make_context(lights);
    flush_shadow_passes(ctx);
    CHECK((g_log.drawsOnTargets == 2U) &&
              (g_log.drawPrograms[1] == row.expected),
          row.what);
    CHECK(g_log.currentProgram == 0U,
          "the pass unbinds its program when it ends");
  }
  g_paletteUploads = false;
  g_mesh.hasSkin = false;
  g_draws[1].skinPalette = kInvalidSkinPalette;
  g_draws[1].material = Material{};
}

/// EXPECTATION (#542): the cached directional cascades are reused only
/// while a redraw would draw the same maps. An unchanged frame reuses
/// them; a caster whose mesh starts resolving to geometry, whose mask
/// texture finishes loading, or whose cutoff changes redraws them. On base
/// the key hashed none of the three, so each reused maps drawn without
/// the change.
void test_directional_cache_follows_what_casters_draw() noexcept {
  reset_backend();
  g_backend.shadowAvailable = true;
  g_backend.shadowMasked = fake_masked_program(11U);
  for (std::size_t c = 0U; c < kShadowCascadeCount; ++c) {
    g_backend.shadowState.depthTargets[c] =
        RenderTargetHandle{300U + static_cast<std::uint32_t>(c)};
  }
  g_draws[1].material.alphaMode = AlphaMode::Mask;
  g_draws[1].material.alphaCutoff = 0.25F;
  g_draws[1].material.opacityTexture = TextureHandle{5U};
  SceneLightData lights{};
  lights.directionalLightCount = 1U;
  lights.directionalLights[0].direction = engine::math::Vec3(0.3F, -1.0F, 0.2F);
  lights.directionalLights[0].intensity = 1.0F;
  // Every cascade is cleared when the maps are redrawn and none is bound
  // when they are reused.
  const auto redrawn = [&]() noexcept {
    reset_fake_device();
    FrameFlushContext ctx = make_context(lights);
    flush_shadow_passes(ctx);
    return g_log.clearsOnTargets == kShadowCascadeCount;
  };
  const auto reused = [&]() noexcept {
    reset_fake_device();
    FrameFlushContext ctx = make_context(lights);
    flush_shadow_passes(ctx);
    return g_log.bindCount == 0U;
  };

  g_meshResolves = false;
  g_texturesResolve = false;
  CHECK(redrawn(), "the first frame draws the cascades");
  CHECK(reused(), "an unchanged frame reuses them");

  g_meshResolves = true;
  CHECK(redrawn(), "a mesh that now resolves to geometry redraws them");
  CHECK(g_log.drawsOnTargets == 2U * kShadowCascadeCount,
        "the redraw draws both casters into every cascade");
  CHECK(reused(), "and the frame after reuses them again");

  g_texturesResolve = true;
  CHECK(redrawn(), "a mask texture that finished loading redraws them");
  CHECK(reused(), "and the frame after reuses them again");

  g_draws[1].material.alphaCutoff = 0.5F;
  CHECK(redrawn(), "a changed cutoff redraws them");
  CHECK(reused(), "and the frame after reuses them again");

  g_draws[1].material = Material{};
}

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
  test_slots_go_to_the_nearest_casters();
  test_equidistant_casters_take_slots_in_index_order();
  test_cvar_gate_still_disables();
  test_masked_casters_draw_through_the_masked_programs();
  test_unmasked_casters_keep_the_pass_program();
  test_skinned_masked_casters_pick_the_fitting_program();
  test_directional_cache_follows_what_casters_draw();

  if (g_failures != 0) {
    std::fprintf(stderr, "shadow_caster_flush_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::puts("shadow_caster_flush_test passed");
  return 0;
}
