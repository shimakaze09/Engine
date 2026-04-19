#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/vec3.h"

namespace engine::runtime {

/// A single camera on the priority stack.
struct CameraEntry final {
  math::Vec3 position = math::Vec3(0.0F, 2.0F, 5.0F);
  math::Vec3 target = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 up = math::Vec3(0.0F, 1.0F, 0.0F);
  float fovRadians = 1.0471975512F; // 60 degrees
  float nearPlane = 0.1F;
  float farPlane = 100.0F;
  float priority = 0.0F;
  float blendSpeed = 5.0F; ///< How fast blend weight ramps from 0→1.
  float blendWeight = 0.0F;
  std::uint32_t ownerEntityIndex = 0U;
  bool active = false;
};

/// A single active camera shake instance.
struct CameraShakeEntry final {
  float amplitude = 0.1F;
  float frequency = 15.0F;
  float duration = 0.5F;
  float decay = 2.0F;
  float elapsed = 0.0F;
  bool active = false;
};

/// Per-World camera manager: priority stack + camera shake.
///
/// Usage: game logic pushes cameras with priorities. The manager blends toward
/// the highest-priority camera. Camera shakes are applied additively on top.
/// Call evaluate() once per frame to produce the final camera state.
class CameraManager final {
public:
  static constexpr std::size_t kMaxCameras = 16U;
  static constexpr std::size_t kMaxShakes = 8U;

  /// Push a new camera onto the priority stack (or update if owner exists).
  bool push_camera(std::uint32_t ownerEntityIndex, const CameraEntry &entry,
                   float priority) noexcept;

  /// Pop (remove) the camera owned by the given entity.
  bool pop_camera(std::uint32_t ownerEntityIndex) noexcept;

  /// Get the raw entry for the highest-priority camera (before shake).
  const CameraEntry *active_camera() const noexcept;

  /// Add a camera shake.
  bool add_shake(float amplitude, float frequency, float duration,
                 float decay) noexcept;

  /// Evaluate the final camera state for this frame. Advances blend weights
  /// and shake timers by @p dt. Writes the resulting camera values into the
  /// output parameters.
  void evaluate(float dt, math::Vec3 *outPosition, math::Vec3 *outTarget,
                math::Vec3 *outUp, float *outFov, float *outNear,
                float *outFar) noexcept;

  /// Clear all cameras and shakes.
  void clear() noexcept;

  std::size_t camera_count() const noexcept;
  std::size_t shake_count() const noexcept;

private:
  /// Simple Perlin-like noise hash for shake offsets.
  static float noise1d(float t) noexcept;

  CameraEntry m_cameras[kMaxCameras]{};
  CameraShakeEntry m_shakes[kMaxShakes]{};
  /// Interpolated output from the previous evaluate call, used for blending.
  math::Vec3 m_currentPosition = math::Vec3(0.0F, 2.0F, 5.0F);
  math::Vec3 m_currentTarget = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 m_currentUp = math::Vec3(0.0F, 1.0F, 0.0F);
  float m_currentFov = 1.0471975512F;
  float m_currentNear = 0.1F;
  float m_currentFar = 100.0F;
  bool m_hasEvaluated = false;
};

} // namespace engine::runtime
