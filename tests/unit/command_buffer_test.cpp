// Verifies command buffer test behavior for the Engine test suite.

#include "command_buffer_flush_internal.h"
#include "command_buffer_math.h"
#include "engine/core/cvar.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"

#include <cstddef>
#include <cstdint>

namespace {

engine::renderer::DrawCommand make_command(std::uint64_t sortKey,
                                           std::uint32_t entity) noexcept {
  engine::renderer::DrawCommand command{};
  command.sortKey.value = sortKey;
  command.entity = entity;
  command.mesh.id = entity + 100U;
  return command;
}

int check_submit_sort_and_reset() {
  static engine::renderer::CommandBufferBuilder builder;
  builder.reset();

  if (!builder.submit(make_command(30U, 3U)) ||
      !builder.submit(make_command(10U, 1U)) ||
      !builder.submit(make_command(20U, 2U))) {
    return 11;
  }
  if (builder.command_count() != 3U) {
    return 12;
  }

  builder.sort_by_key();
  const engine::renderer::CommandBufferView view = builder.view();
  if ((view.count != 3U) || (view.data == nullptr)) {
    return 13;
  }
  if ((view.data[0].entity != 1U) || (view.data[1].entity != 2U) ||
      (view.data[2].entity != 3U)) {
    return 14;
  }

  builder.reset();
  if (builder.command_count() != 0U) {
    return 15;
  }
  if (builder.view().count != 0U) {
    return 16;
  }
  return 0;
}

// Transparent commands sort by depth before any other key bits.
int check_transparent_sorts_depth_first() {
  static engine::renderer::CommandBufferBuilder builder;
  builder.reset();

  constexpr std::uint64_t kTransparent = 1ULL << 63U;
  // Far surface (small inverted depth) with HIGH texture/mesh key bits.
  const std::uint64_t farKey = kTransparent | (0xFFFFFULL << 16U) | 5ULL;
  // Near surface (large inverted depth) with LOW texture/mesh key bits.
  const std::uint64_t nearKey = kTransparent | (0x00001ULL << 16U) | 500ULL;
  // One opaque command to confirm opaque still draws before transparent.
  const std::uint64_t opaqueKey = (0x00002ULL << 16U) | 7ULL;

  if (!builder.submit(make_command(nearKey, 1U)) ||
      !builder.submit(make_command(farKey, 2U)) ||
      !builder.submit(make_command(opaqueKey, 3U))) {
    return 21;
  }

  builder.sort_by_key();
  const engine::renderer::CommandBufferView view = builder.view();
  if (view.count != 3U) {
    return 22;
  }
  if (view.data[0].entity != 3U) {
    return 23; // opaque first
  }
  if ((view.data[1].entity != 2U) || (view.data[2].entity != 1U)) {
    return 24; // far transparent before near transparent, despite key bits
  }
  return 0;
}

int check_append_and_capacity() {
  static engine::renderer::CommandBufferBuilder left;
  static engine::renderer::CommandBufferBuilder right;
  left.reset();
  right.reset();

  if (!left.append_from(right)) {
    return 21;
  }
  if (!left.submit(make_command(3U, 3U)) ||
      !right.submit(make_command(1U, 1U)) ||
      !right.submit(make_command(2U, 2U))) {
    return 22;
  }
  if (!left.append_from(right)) {
    return 23;
  }
  if (left.command_count() != 3U) {
    return 24;
  }
  const engine::renderer::CommandBufferView appended = left.view();
  if ((appended.data[0].entity != 3U) || (appended.data[1].entity != 1U) ||
      (appended.data[2].entity != 2U)) {
    return 25;
  }

  left.reset();
  for (std::size_t i = 0U;
       i < engine::renderer::CommandBufferBuilder::kMaxDrawCommands; ++i) {
    if (!left.submit(make_command(static_cast<std::uint64_t>(i),
                                  static_cast<std::uint32_t>(i)))) {
      return 26;
    }
  }
  if (left.submit(make_command(0U, 0U))) {
    return 27;
  }

  right.reset();
  if (!right.submit(make_command(1U, 1U))) {
    return 28;
  }
  if (left.append_from(right)) {
    return 29;
  }
  return 0;
}

int check_static_mesh_batches() {
  static engine::renderer::CommandBufferBuilder builder;
  builder.reset();

  constexpr std::uint32_t kBenchmarkCount = 10000U;
  for (std::uint32_t i = 0U; i < kBenchmarkCount; ++i) {
    engine::renderer::DrawCommand command = make_command(kBenchmarkCount - i, i);
    command.mesh.id = 7U;
    command.material.albedo = engine::math::Vec3(0.25F, 0.5F, 0.75F);
    command.material.roughness = 0.4F;
    if (!builder.submit(command)) {
      return 91;
    }
  }

  builder.sort_by_key();
  engine::renderer::StaticMeshBatch singleBatch{};
  const std::size_t singleBatchCount =
      engine::renderer::build_static_mesh_batches(builder.view(), 0U,
                                                  builder.command_count(),
                                                  &singleBatch, 1U);
  if ((singleBatchCount != 1U) || (singleBatch.first != 0U) ||
      (singleBatch.count != kBenchmarkCount)) {
    return 92;
  }

  builder.reset();
  engine::renderer::DrawCommand first = make_command(3U, 1U);
  first.mesh.id = 12U;
  first.material.roughness = 0.5F;
  engine::renderer::DrawCommand second = make_command(1U, 2U);
  second.mesh.id = 12U;
  second.material.roughness = 0.9F;
  engine::renderer::DrawCommand third = make_command(2U, 3U);
  third.mesh.id = 12U;
  third.material.roughness = 0.5F;
  if (!builder.submit(first) || !builder.submit(second) ||
      !builder.submit(third)) {
    return 93;
  }

  builder.sort_by_key();
  engine::renderer::StaticMeshBatch batches[4]{};
  const std::size_t batchCount = engine::renderer::build_static_mesh_batches(
      builder.view(), 0U, builder.command_count(), batches, 4U);
  if (batchCount != 2U) {
    return 94;
  }
  if ((batches[0].count != 2U) || (batches[1].count != 1U)) {
    return 95;
  }

  builder.reset();
  engine::renderer::DrawCommand foliageA = make_command(1U, 4U);
  foliageA.mesh.id = 33U;
  foliageA.material.albedo = engine::math::Vec3(0.1F, 0.6F, 0.2F);
  foliageA.foliageWindStrength = 0.25F;
  foliageA.foliageWindFrequency = 1.5F;
  foliageA.foliageWindPhase = 0.0F;
  foliageA.foliageLodIndex = 0U;
  engine::renderer::DrawCommand foliageB = foliageA;
  foliageB.entity = 5U;
  foliageB.foliageWindPhase = 1.0F;
  foliageB.foliageLodIndex = 1U;
  engine::renderer::DrawCommand foliageC = foliageA;
  foliageC.entity = 6U;
  foliageC.foliageWindStrength = 0.5F;
  if (!builder.submit(foliageA) || !builder.submit(foliageB) ||
      !builder.submit(foliageC)) {
    return 96;
  }

  builder.sort_by_key();
  const std::size_t foliageBatchCount =
      engine::renderer::build_static_mesh_batches(builder.view(), 0U,
                                                  builder.command_count(),
                                                  batches, 4U);
  if (foliageBatchCount != 2U) {
    return 97;
  }
  if ((batches[0].count != 2U) || (batches[1].count != 1U)) {
    return 98;
  }

  return 0;
}

int check_camera_state() {
  engine::renderer::CameraState camera{};
  camera.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  camera.target = engine::math::Vec3(4.0F, 5.0F, 6.0F);
  camera.up = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  camera.fovRadians = 0.75F;
  camera.nearPlane = 0.25F;
  camera.farPlane = 250.0F;
  camera.projection = engine::renderer::CameraState::kProjectionOrthographic;
  camera.orthographicSize = 7.5F;
  engine::renderer::set_active_camera(camera);

  const engine::renderer::CameraState readback =
      engine::renderer::get_active_camera();
  if ((readback.position.x != 1.0F) || (readback.position.y != 2.0F) ||
      (readback.position.z != 3.0F)) {
    return 31;
  }
  if ((readback.target.x != 4.0F) || (readback.target.y != 5.0F) ||
      (readback.target.z != 6.0F)) {
    return 32;
  }
  if ((readback.fovRadians != 0.75F) || (readback.nearPlane != 0.25F) ||
      (readback.farPlane != 250.0F)) {
    return 33;
  }
  if ((readback.projection !=
       engine::renderer::CameraState::kProjectionOrthographic) ||
      (readback.orthographicSize != 7.5F)) {
    return 34;
  }

  // The shared projection builder must produce the exact math::ortho
  // matrix for an orthographic camera (half-height scaled by aspect) and
  // the exact math::perspective matrix otherwise (#221).
  const float aspect = 2.0F;
  const engine::math::Mat4 built =
      engine::renderer::camera_projection_matrix(camera, aspect);
  const engine::math::Mat4 expected = engine::math::ortho(
      -7.5F * aspect, 7.5F * aspect, -7.5F, 7.5F, 0.25F, 250.0F);
  for (int c = 0; c < 4; ++c) {
    if ((built.columns[c].x != expected.columns[c].x) ||
        (built.columns[c].y != expected.columns[c].y) ||
        (built.columns[c].z != expected.columns[c].z) ||
        (built.columns[c].w != expected.columns[c].w)) {
      return 35;
    }
  }
  camera.projection = engine::renderer::CameraState::kProjectionPerspective;
  const engine::math::Mat4 builtPersp =
      engine::renderer::camera_projection_matrix(camera, aspect);
  const engine::math::Mat4 expectedPersp =
      engine::math::perspective(0.75F, aspect, 0.25F, 250.0F);
  for (int c = 0; c < 4; ++c) {
    if ((builtPersp.columns[c].x != expectedPersp.columns[c].x) ||
        (builtPersp.columns[c].y != expectedPersp.columns[c].y) ||
        (builtPersp.columns[c].z != expectedPersp.columns[c].z) ||
        (builtPersp.columns[c].w != expectedPersp.columns[c].w)) {
      return 36;
    }
  }
  return 0;
}

/// Verifies renderer shutdown clears public singleton state even when cold.
int check_shutdown_resets_public_renderer_state() {
  engine::renderer::CameraState camera{};
  camera.position = engine::math::Vec3(8.0F, 9.0F, 10.0F);
  camera.target = engine::math::Vec3(11.0F, 12.0F, 13.0F);
  camera.fovRadians = 0.5F;
  engine::renderer::set_active_camera(camera);
  engine::renderer::set_skybox_texture(engine::renderer::TextureHandle{77U});

  engine::renderer::SceneCaptureRequest pendingCapture{};
  engine::renderer::set_scene_capture_requests(&pendingCapture, 1U);

  engine::renderer::shutdown_renderer();

  if (engine::renderer::scene_capture_request_count() != 0U) {
    return 38;
  }

  const engine::renderer::CameraState resetCamera =
      engine::renderer::get_active_camera();
  if ((resetCamera.position.x != 0.0F) || (resetCamera.position.y != 2.0F) ||
      (resetCamera.position.z != 5.0F)) {
    return 34;
  }
  if ((resetCamera.target.x != 0.0F) || (resetCamera.target.y != 0.0F) ||
      (resetCamera.target.z != 0.0F)) {
    return 35;
  }
  if (resetCamera.fovRadians != 1.0471975512F) {
    return 36;
  }
  if (engine::renderer::get_skybox_texture() !=
      engine::renderer::kInvalidTextureHandle) {
    return 37;
  }

  return 0;
}

int check_environment_texture_getters() {
  engine::core::shutdown_cvars();
  if (!engine::core::initialize_cvars()) {
    return 41;
  }
  if (!engine::core::cvar_register_string("r_sky_model", "cubemap", "sky")) {
    engine::core::shutdown_cvars();
    return 42;
  }
  if (!engine::core::cvar_register_bool("r_env_prefilter", true, "prefilter")) {
    engine::core::shutdown_cvars();
    return 43;
  }
  if (!engine::core::cvar_register_bool("r_env_irradiance", true,
                                        "irradiance")) {
    engine::core::shutdown_cvars();
    return 44;
  }
  if (!engine::core::cvar_register_bool("r_env_brdf_lut", true, "brdf")) {
    engine::core::shutdown_cvars();
    return 45;
  }

  if (engine::renderer::get_prefiltered_environment_texture() !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::core::shutdown_cvars();
    return 46;
  }
  if (engine::renderer::get_irradiance_environment_texture() !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::core::shutdown_cvars();
    return 47;
  }
  if (engine::renderer::get_brdf_lut_texture() !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::core::shutdown_cvars();
    return 48;
  }

  if (!engine::core::cvar_set_string("r_sky_model", "none")) {
    engine::core::shutdown_cvars();
    return 49;
  }
  if (engine::renderer::get_prefiltered_environment_texture() !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::core::shutdown_cvars();
    return 50;
  }
  if (engine::renderer::get_irradiance_environment_texture() !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::core::shutdown_cvars();
    return 51;
  }

  if (!engine::core::cvar_set_bool("r_env_brdf_lut", false)) {
    engine::core::shutdown_cvars();
    return 52;
  }
  if (engine::renderer::get_brdf_lut_texture() !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::core::shutdown_cvars();
    return 53;
  }

  engine::core::shutdown_cvars();
  return 0;
}

int check_reflection_probe_bake_settings() {
  engine::renderer::ReflectionProbeBakeSettings settings{};
  settings.prefilteredFaceSize = 130U;
  settings.prefilteredMipLevels = 99U;
  settings.irradianceFaceSize = 7U;
  settings.brdfLutSize = 1000U;

  const engine::renderer::ReflectionProbeBakeSettings normalized =
      engine::renderer::normalize_reflection_probe_bake_settings(settings);
  if (normalized.prefilteredFaceSize != 128U) {
    return 61;
  }
  if (normalized.prefilteredMipLevels != 8U) {
    return 62;
  }
  if (normalized.irradianceFaceSize != 8U) {
    return 63;
  }
  if (normalized.brdfLutSize != 512U) {
    return 64;
  }

  const engine::renderer::ReflectionProbeBakeResult inactive =
      engine::renderer::bake_reflection_probe(
          engine::renderer::ReflectionProbeBakeRequest{});
  constexpr engine::renderer::DeviceTextureHandle kNoTexture{};
  if (inactive.baked || (inactive.sourceCubemapTexture != kNoTexture) ||
      (inactive.prefilteredEnvironmentTexture != kNoTexture) ||
      (inactive.irradianceEnvironmentTexture != kNoTexture) ||
      (inactive.brdfLutTexture != kNoTexture)) {
    return 65;
  }

  return 0;
}

int check_distance_fog_settings() {
  using engine::renderer::DistanceFogMode;

  const engine::renderer::DistanceFogSettings defaultDistanceFog{};
  if (defaultDistanceFog.mode != DistanceFogMode::Exp2) {
    return 70;
  }
  const engine::renderer::HeightFogSettings defaultHeightFog{};
  if (!defaultHeightFog.enabled) {
    return 84;
  }

  if (engine::renderer::parse_distance_fog_mode("linear") !=
      DistanceFogMode::Linear) {
    return 71;
  }
  if (engine::renderer::parse_distance_fog_mode("exp") !=
      DistanceFogMode::Exp) {
    return 72;
  }
  if (engine::renderer::parse_distance_fog_mode("exp2") !=
      DistanceFogMode::Exp2) {
    return 73;
  }
  if (engine::renderer::parse_distance_fog_mode("bad") !=
      DistanceFogMode::Off) {
    return 74;
  }

  engine::math::Vec3 color{};
  if (!engine::renderer::parse_distance_fog_color("0.25, 0.5, 2.0",
                                                  &color)) {
    return 75;
  }
  if ((color.x != 0.25F) || (color.y != 0.5F) || (color.z != 1.0F)) {
    return 76;
  }
  if (engine::renderer::parse_distance_fog_color("0.1 0.2", &color)) {
    return 77;
  }

  engine::renderer::DistanceFogSettings settings{};
  settings.mode = DistanceFogMode::Exp2;
  settings.start = -5.0F;
  settings.end = -1.0F;
  settings.density = -0.5F;
  settings.color = engine::math::Vec3(-1.0F, 0.5F, 2.0F);

  const engine::renderer::DistanceFogSettings normalized =
      engine::renderer::normalize_distance_fog_settings(settings);
  if (normalized.mode != DistanceFogMode::Exp2) {
    return 78;
  }
  if ((normalized.start != 0.0F) || (normalized.end <= normalized.start) ||
      (normalized.density != 0.0F)) {
    return 79;
  }
  if ((normalized.color.x != 0.0F) || (normalized.color.y != 0.5F) ||
      (normalized.color.z != 1.0F)) {
    return 80;
  }

  engine::renderer::HeightFogSettings height{};
  height.enabled = true;
  height.baseHeight = 12.0F;
  height.density = 2.0F;
  height.falloff = -1.0F;
  height.stepCount = 256;

  const engine::renderer::HeightFogSettings normalizedHeight =
      engine::renderer::normalize_height_fog_settings(height);
  if (!normalizedHeight.enabled || (normalizedHeight.baseHeight != 12.0F)) {
    return 81;
  }
  if ((normalizedHeight.density != 1.0F) ||
      (normalizedHeight.falloff != 0.001F) ||
      (normalizedHeight.stepCount != 64)) {
    return 82;
  }

  height.density = 0.0F;
  const engine::renderer::HeightFogSettings disabledHeight =
      engine::renderer::normalize_height_fog_settings(height);
  if (disabledHeight.enabled) {
    return 83;
  }

  return 0;
}

int check_scene_capture_requests() {
  // Normalization clamps resolution and repairs degenerate camera planes.
  engine::renderer::SceneCaptureRequest request{};
  request.width = 8U;
  request.height = 5000U;
  request.camera.fovRadians = 0.0F;
  request.camera.nearPlane = -1.0F;
  request.camera.farPlane = -2.0F;

  const engine::renderer::SceneCaptureRequest normalized =
      engine::renderer::normalize_scene_capture_request(request);
  if ((normalized.width != engine::renderer::kMinSceneCaptureSize) ||
      (normalized.height != engine::renderer::kMaxSceneCaptureSize)) {
    return 90;
  }
  if (normalized.camera.fovRadians != 1.0471975512F) {
    return 91;
  }
  if (normalized.camera.nearPlane != 0.1F) {
    return 92;
  }
  if (normalized.camera.farPlane != normalized.camera.nearPlane + 100.0F) {
    return 93;
  }

  // In-range requests pass through unchanged.
  engine::renderer::SceneCaptureRequest valid{};
  valid.width = 320U;
  valid.height = 240U;
  valid.camera.fovRadians = 0.9F;
  valid.camera.nearPlane = 0.5F;
  valid.camera.farPlane = 50.0F;
  const engine::renderer::SceneCaptureRequest untouched =
      engine::renderer::normalize_scene_capture_request(valid);
  if ((untouched.width != 320U) || (untouched.height != 240U) ||
      (untouched.camera.fovRadians != 0.9F) ||
      (untouched.camera.nearPlane != 0.5F) ||
      (untouched.camera.farPlane != 50.0F)) {
    return 94;
  }

  // Stored requests report their count; overflow drops to the slot count.
  engine::renderer::SceneCaptureRequest
      many[engine::renderer::kMaxSceneCaptures + 2U]{};
  engine::renderer::set_scene_capture_requests(
      many, engine::renderer::kMaxSceneCaptures + 2U);
  if (engine::renderer::scene_capture_request_count() !=
      engine::renderer::kMaxSceneCaptures) {
    return 95;
  }

  // A null array with nonzero count clears the stored requests.
  engine::renderer::set_scene_capture_requests(nullptr, 3U);
  if (engine::renderer::scene_capture_request_count() != 0U) {
    return 96;
  }

  engine::renderer::set_scene_capture_requests(many, 2U);
  if (engine::renderer::scene_capture_request_count() != 2U) {
    return 97;
  }
  engine::renderer::set_scene_capture_requests(nullptr, 0U);
  if (engine::renderer::scene_capture_request_count() != 0U) {
    return 98;
  }

  // Without a rendered flush, capture textures stay unavailable; slot
  // indices past the fixed capacity always return 0.
  if (engine::renderer::get_scene_capture_texture(0U) !=
      engine::renderer::kInvalidDeviceTexture) {
    return 99;
  }
  if (engine::renderer::get_scene_capture_texture(
          engine::renderer::kMaxSceneCaptures) !=
      engine::renderer::kInvalidDeviceTexture) {
    return 100;
  }

  // Capture texture handles: out-of-range slots are always invalid; a
  // requested slot gets a stable handle that resolves to "no texture"
  // until a flush creates the target.
  if (engine::renderer::scene_capture_texture_handle(
          engine::renderer::kMaxSceneCaptures) !=
      engine::renderer::kInvalidTextureHandle) {
    return 101;
  }
  if (!engine::renderer::initialize_texture_system()) {
    return 102;
  }
  engine::renderer::shutdown_renderer();
  engine::renderer::SceneCaptureRequest handleRequest{};
  engine::renderer::set_scene_capture_requests(&handleRequest, 1U);
  const engine::renderer::TextureHandle slotHandle =
      engine::renderer::scene_capture_texture_handle(0U);
  if (slotHandle == engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 103;
  }
  if (engine::renderer::texture_device_handle(slotHandle) !=
      engine::renderer::kInvalidDeviceTexture) {
    engine::renderer::shutdown_texture_system();
    return 104;
  }
  // Re-submitting requests keeps the handle stable.
  engine::renderer::set_scene_capture_requests(&handleRequest, 1U);
  if (engine::renderer::scene_capture_texture_handle(0U) != slotHandle) {
    engine::renderer::shutdown_texture_system();
    return 105;
  }
  // Renderer shutdown releases the slot handles even before GL init, and
  // (audit R-5) actually unloads them from the texture system: the old
  // handle must be stale afterwards, not a leaked live registration.
  engine::renderer::shutdown_renderer();
  if (engine::renderer::scene_capture_texture_handle(0U) !=
      engine::renderer::kInvalidTextureHandle) {
    engine::renderer::shutdown_texture_system();
    return 106;
  }
  if (engine::renderer::update_external_texture(
          slotHandle, engine::renderer::DeviceTextureHandle{7U})) {
    engine::renderer::shutdown_texture_system();
    return 107;
  }
  engine::renderer::shutdown_texture_system();

  return 0;
}

/// EXPECTATION: skin palettes mirror the capture-request store contract —
/// counts clamp to kMaxSkinPalettes, a null array with nonzero count
/// clears the store, and a fresh DrawCommand carries no palette.
int check_skin_palette_store() {
  if (engine::renderer::DrawCommand{}.skinPalette !=
      engine::renderer::kInvalidSkinPalette) {
    return 110;
  }

  static engine::renderer::SkinPalette
      palettes[engine::renderer::kMaxSkinPalettes + 2U]{};
  palettes[0].jointCount =
      static_cast<std::uint32_t>(engine::renderer::kMaxSkinPaletteJoints) +
      5U;
  engine::renderer::set_skin_palettes(
      palettes, engine::renderer::kMaxSkinPalettes + 2U);
  if (engine::renderer::skin_palette_count() !=
      engine::renderer::kMaxSkinPalettes) {
    return 111;
  }

  engine::renderer::set_skin_palettes(nullptr, 3U);
  if (engine::renderer::skin_palette_count() != 0U) {
    return 112;
  }

  engine::renderer::set_skin_palettes(palettes, 2U);
  if (engine::renderer::skin_palette_count() != 2U) {
    return 113;
  }
  engine::renderer::set_skin_palettes(nullptr, 0U);
  if (engine::renderer::skin_palette_count() != 0U) {
    return 114;
  }
  return 0;
}

/// Audit H-10: sanitize_scene_light_counts must pass valid counts through
/// by reference identity (no copy), clamp oversized public counts to the
/// fixed array capacities, preserve the light payloads when clamping, and
/// handle each count overflowing independently.
int check_scene_light_count_sanitizer() {
  static engine::renderer::SceneLightData lights{};
  static engine::renderer::SceneLightData storage{};

  lights.pointLightCount = engine::renderer::kMaxPointLights;
  lights.spotLightCount = engine::renderer::kMaxSpotLights;
  if (&engine::renderer::sanitize_scene_light_counts(lights, storage) !=
      &lights) {
    return 120;
  }

  lights.pointLightCount = 0U;
  lights.spotLightCount = 0U;
  if (&engine::renderer::sanitize_scene_light_counts(lights, storage) !=
      &lights) {
    return 121;
  }

  lights.pointLightCount = engine::renderer::kMaxPointLights + 1U;
  lights.spotLightCount = 3U;
  lights.pointLights[engine::renderer::kMaxPointLights - 1U].intensity = 7.5F;
  lights.spotLights[2].intensity = 2.5F;
  const engine::renderer::SceneLightData &pointClamped =
      engine::renderer::sanitize_scene_light_counts(lights, storage);
  if (&pointClamped != &storage) {
    return 122;
  }
  if (pointClamped.pointLightCount != engine::renderer::kMaxPointLights) {
    return 123;
  }
  if (pointClamped.spotLightCount != 3U) {
    return 124;
  }
  if (pointClamped.pointLights[engine::renderer::kMaxPointLights - 1U]
          .intensity != 7.5F) {
    return 125;
  }
  if (pointClamped.spotLights[2].intensity != 2.5F) {
    return 126;
  }

  lights.pointLightCount = 5U;
  lights.spotLightCount = engine::renderer::kMaxSpotLights + 900U;
  const engine::renderer::SceneLightData &spotClamped =
      engine::renderer::sanitize_scene_light_counts(lights, storage);
  if (&spotClamped != &storage) {
    return 127;
  }
  if (spotClamped.pointLightCount != 5U) {
    return 128;
  }
  if (spotClamped.spotLightCount != engine::renderer::kMaxSpotLights) {
    return 129;
  }
  return 0;
}

/// Audit R-3: point_shadow_slot_light_position (the lighting passes' slot
/// read) must return the referenced light's position only while the slot
/// index is live, and a zero vector for empty (-1) slots, indices at or
/// past the live count, and the emptied-family case (count back to zero)
/// where the old code read pointLights[0] regardless.
int check_point_shadow_slot_liveness() {
  static engine::renderer::SceneLightData lights{};
  lights.pointLightCount = 2U;
  lights.pointLights[0].position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  lights.pointLights[1].position = engine::math::Vec3(4.0F, 5.0F, 6.0F);

  const engine::math::Vec3 empty =
      engine::renderer::point_shadow_slot_light_position(-1, lights);
  if ((empty.x != 0.0F) || (empty.y != 0.0F) || (empty.z != 0.0F)) {
    return 130;
  }

  const engine::math::Vec3 first =
      engine::renderer::point_shadow_slot_light_position(0, lights);
  if ((first.x != 1.0F) || (first.y != 2.0F) || (first.z != 3.0F)) {
    return 131;
  }
  const engine::math::Vec3 second =
      engine::renderer::point_shadow_slot_light_position(1, lights);
  if ((second.x != 4.0F) || (second.y != 5.0F) || (second.z != 6.0F)) {
    return 132;
  }

  const engine::math::Vec3 pastCount =
      engine::renderer::point_shadow_slot_light_position(2, lights);
  if ((pastCount.x != 0.0F) || (pastCount.y != 0.0F) ||
      (pastCount.z != 0.0F)) {
    return 133;
  }

  lights.pointLightCount = 0U;
  const engine::math::Vec3 stale =
      engine::renderer::point_shadow_slot_light_position(0, lights);
  if ((stale.x != 0.0F) || (stale.y != 0.0F) || (stale.z != 0.0F)) {
    return 134;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_submit_sort_and_reset();
  if (result != 0) {
    return result;
  }
  result = check_transparent_sorts_depth_first();
  if (result != 0) {
    return result;
  }
  result = check_append_and_capacity();
  if (result != 0) {
    return result;
  }
  result = check_static_mesh_batches();
  if (result != 0) {
    return result;
  }
  result = check_camera_state();
  if (result != 0) {
    return result;
  }
  result = check_shutdown_resets_public_renderer_state();
  if (result != 0) {
    return result;
  }
  result = check_environment_texture_getters();
  if (result != 0) {
    return result;
  }
  result = check_reflection_probe_bake_settings();
  if (result != 0) {
    return result;
  }
  result = check_distance_fog_settings();
  if (result != 0) {
    return result;
  }
  result = check_scene_capture_requests();
  if (result != 0) {
    return result;
  }
  result = check_skin_palette_store();
  if (result != 0) {
    return result;
  }
  result = check_scene_light_count_sanitizer();
  if (result != 0) {
    return result;
  }
  return check_point_shadow_slot_liveness();
}
