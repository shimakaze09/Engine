// Declares private command buffer math helpers.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/shadow_map.h"

namespace engine::renderer {

/// Returns the model matrix carried by a draw command.
math::Mat4 compute_model_matrix(const DrawCommand &command) noexcept;

/// Computes a model-view-projection matrix from a model and view-projection.
math::Mat4 compute_mvp(const math::Mat4 &model,
                       const math::Mat4 &viewProjection) noexcept;

/// Extracts a 3x3 normal matrix from a model matrix into column-major storage.
void extract_normal_matrix(const math::Mat4 &model,
                           float *normalMatrixOut) noexcept;

/// Position of the point light a shadow slot references, or a zero vector
/// when the slot is empty (-1) or its index is outside the live light
/// count — a stale slot must never sample another light's data.
math::Vec3 point_shadow_slot_light_position(
    int slotLightIndex, const SceneLightData &lights) noexcept;

} // namespace engine::renderer
