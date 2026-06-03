#include "engine/core/cvar.h"
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

  if (engine::renderer::get_prefiltered_environment_texture() != 0U) {
    engine::core::shutdown_cvars();
    return 46;
  }
  if (engine::renderer::get_irradiance_environment_texture() != 0U) {
    engine::core::shutdown_cvars();
    return 47;
  }
  if (engine::renderer::get_brdf_lut_texture() != 0U) {
    engine::core::shutdown_cvars();
    return 48;
  }

  if (!engine::core::cvar_set_string("r_sky_model", "none")) {
    engine::core::shutdown_cvars();
    return 49;
  }
  if (engine::renderer::get_prefiltered_environment_texture() != 0U) {
    engine::core::shutdown_cvars();
    return 50;
  }
  if (engine::renderer::get_irradiance_environment_texture() != 0U) {
    engine::core::shutdown_cvars();
    return 51;
  }

  if (!engine::core::cvar_set_bool("r_env_brdf_lut", false)) {
    engine::core::shutdown_cvars();
    return 52;
  }
  if (engine::renderer::get_brdf_lut_texture() != 0U) {
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
  if (inactive.baked || (inactive.sourceCubemapTexture != 0U) ||
      (inactive.prefilteredEnvironmentTexture != 0U) ||
      (inactive.irradianceEnvironmentTexture != 0U) ||
      (inactive.brdfLutTexture != 0U)) {
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

} // namespace

int main() {
  int result = check_submit_sort_and_reset();
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
  return 0;
}
