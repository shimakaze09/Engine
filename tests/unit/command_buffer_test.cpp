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
  result = check_camera_state();
  if (result != 0) {
    return result;
  }
  result = check_environment_texture_getters();
  if (result != 0) {
    return result;
  }
  return 0;
}
