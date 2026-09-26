// Implements private command buffer math helpers.

#include "command_buffer_math.h"

namespace engine::renderer {

math::Mat4 compute_model_matrix(const DrawCommand &command) noexcept {
  return command.modelMatrix;
}

math::Mat4 compute_mvp(const math::Mat4 &model,
                       const math::Mat4 &viewProjection) noexcept {
  return math::mul(viewProjection, model);
}

void extract_normal_matrix(const math::Mat4 &model,
                           float *normalMatrixOut) noexcept {
  if (normalMatrixOut == nullptr) {
    return;
  }

  math::Mat4 invModel{};
  const math::Mat4 normalSource =
      math::inverse(model, &invModel) ? math::transpose(invModel) : model;

  normalMatrixOut[0] = normalSource.columns[0].x;
  normalMatrixOut[1] = normalSource.columns[0].y;
  normalMatrixOut[2] = normalSource.columns[0].z;

  normalMatrixOut[3] = normalSource.columns[1].x;
  normalMatrixOut[4] = normalSource.columns[1].y;
  normalMatrixOut[5] = normalSource.columns[1].z;

  normalMatrixOut[6] = normalSource.columns[2].x;
  normalMatrixOut[7] = normalSource.columns[2].y;
  normalMatrixOut[8] = normalSource.columns[2].z;
}

math::Vec3 point_shadow_slot_light_position(
    int slotLightIndex, const SceneLightData &lights) noexcept {
  if ((slotLightIndex < 0) ||
      (static_cast<std::size_t>(slotLightIndex) >= lights.pointLightCount)) {
    return math::Vec3{};
  }
  return lights.pointLights[static_cast<std::size_t>(slotLightIndex)].position;
}

} // namespace engine::renderer
