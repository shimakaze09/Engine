#include "engine/runtime/camera_manager.h"

#include <cmath>

#include "engine/core/logging.h"

namespace engine::runtime {

namespace {
constexpr const char *kLogChannel = "camera";

float lerp(float a, float b, float t) noexcept {
  return a + (b - a) * t;
}

math::Vec3 lerp_vec3(const math::Vec3 &a, const math::Vec3 &b,
                     float t) noexcept {
  return math::Vec3(lerp(a.x, b.x, t), lerp(a.y, b.y, t),
                    lerp(a.z, b.z, t));
}

float clamp01(float v) noexcept {
  if (v < 0.0F) {
    return 0.0F;
  }
  if (v > 1.0F) {
    return 1.0F;
  }
  return v;
}

} // namespace

bool CameraManager::push_camera(std::uint32_t ownerEntityIndex,
                                const CameraEntry &entry,
                                float priority) noexcept {
  // Check if this owner already has a camera, update it.
  for (auto &cam : m_cameras) {
    if (cam.active && (cam.ownerEntityIndex == ownerEntityIndex)) {
      cam.position = entry.position;
      cam.target = entry.target;
      cam.up = entry.up;
      cam.fovRadians = entry.fovRadians;
      cam.nearPlane = entry.nearPlane;
      cam.farPlane = entry.farPlane;
      cam.priority = priority;
      cam.blendSpeed = entry.blendSpeed;
      return true;
    }
  }

  // Find a free slot.
  for (auto &cam : m_cameras) {
    if (!cam.active) {
      cam = entry;
      cam.ownerEntityIndex = ownerEntityIndex;
      cam.priority = priority;
      cam.blendWeight = 0.0F;
      cam.active = true;
      return true;
    }
  }

  core::log_message(core::LogLevel::Warning, kLogChannel,
                    "camera manager full, cannot push another camera");
  return false;
}

bool CameraManager::pop_camera(std::uint32_t ownerEntityIndex) noexcept {
  for (auto &cam : m_cameras) {
    if (cam.active && (cam.ownerEntityIndex == ownerEntityIndex)) {
      cam.active = false;
      return true;
    }
  }
  return false;
}

const CameraEntry *CameraManager::active_camera() const noexcept {
  const CameraEntry *best = nullptr;
  for (const auto &cam : m_cameras) {
    if (!cam.active) {
      continue;
    }
    if ((best == nullptr) || (cam.priority > best->priority)) {
      best = &cam;
    }
  }
  return best;
}

bool CameraManager::add_shake(float amplitude, float frequency, float duration,
                              float decay) noexcept {
  for (auto &shake : m_shakes) {
    if (!shake.active) {
      shake.amplitude = amplitude;
      shake.frequency = frequency;
      shake.duration = duration;
      shake.decay = decay;
      shake.elapsed = 0.0F;
      shake.active = true;
      return true;
    }
  }
  core::log_message(core::LogLevel::Warning, kLogChannel,
                    "camera shake slots full");
  return false;
}

float CameraManager::noise1d(float t) noexcept {
  // Simple value-noise approximation via integer lattice + smoothstep.
  const float fl = std::floor(t);
  const int i0 = static_cast<int>(fl);
  const float frac = t - fl;
  // Hash integers to pseudo-random floats in [-1, 1].
  auto hash = [](int n) noexcept -> float {
    n = (n << 13) ^ n;
    const int t2 =
        (n * (n * n * 15731 + 789221) + 1376312589) & 0x7FFFFFFF;
    return 1.0F - static_cast<float>(t2) / 1073741824.0F;
  };
  const float v0 = hash(i0);
  const float v1 = hash(i0 + 1);
  // Smoothstep interpolation.
  const float s = frac * frac * (3.0F - 2.0F * frac);
  return v0 + s * (v1 - v0);
}

void CameraManager::evaluate(float dt, math::Vec3 *outPosition,
                              math::Vec3 *outTarget, math::Vec3 *outUp,
                              float *outFov, float *outNear,
                              float *outFar) noexcept {
  if ((outPosition == nullptr) || (outTarget == nullptr) ||
      (outUp == nullptr) || (outFov == nullptr) || (outNear == nullptr) ||
      (outFar == nullptr)) {
    return;
  }

  // Find highest-priority camera.
  const CameraEntry *best = active_camera();
  if (best == nullptr) {
    // No cameras — return current state unchanged.
    *outPosition = m_currentPosition;
    *outTarget = m_currentTarget;
    *outUp = m_currentUp;
    *outFov = m_currentFov;
    *outNear = m_currentNear;
    *outFar = m_currentFar;
    return;
  }

  // Advance blend weights.
  for (auto &cam : m_cameras) {
    if (!cam.active) {
      continue;
    }
    if (&cam == best) {
      cam.blendWeight =
          clamp01(cam.blendWeight + cam.blendSpeed * dt);
    } else {
      cam.blendWeight =
          clamp01(cam.blendWeight - cam.blendSpeed * dt);
    }
  }

  // Blend active camera toward target.
  const float t = clamp01(best->blendWeight);

  if (!m_hasEvaluated) {
    // First evaluate: snap directly.
    m_currentPosition = best->position;
    m_currentTarget = best->target;
    m_currentUp = best->up;
    m_currentFov = best->fovRadians;
    m_currentNear = best->nearPlane;
    m_currentFar = best->farPlane;
    m_hasEvaluated = true;
  } else {
    m_currentPosition = lerp_vec3(m_currentPosition, best->position, t);
    m_currentTarget = lerp_vec3(m_currentTarget, best->target, t);
    m_currentUp = lerp_vec3(m_currentUp, best->up, t);
    m_currentFov = lerp(m_currentFov, best->fovRadians, t);
    m_currentNear = lerp(m_currentNear, best->nearPlane, t);
    m_currentFar = lerp(m_currentFar, best->farPlane, t);
  }

  // Apply camera shakes additively.
  math::Vec3 shakeOffset(0.0F, 0.0F, 0.0F);
  for (auto &shake : m_shakes) {
    if (!shake.active) {
      continue;
    }
    shake.elapsed += dt;
    if (shake.elapsed >= shake.duration) {
      shake.active = false;
      continue;
    }
    const float progress = shake.elapsed / shake.duration;
    const float envelope =
        shake.amplitude * std::exp(-shake.decay * progress);
    const float phase = shake.elapsed * shake.frequency;
    shakeOffset.x += envelope * noise1d(phase);
    shakeOffset.y += envelope * noise1d(phase + 100.0F);
    shakeOffset.z += envelope * noise1d(phase + 200.0F);
  }

  *outPosition =
      math::Vec3(m_currentPosition.x + shakeOffset.x,
                  m_currentPosition.y + shakeOffset.y,
                  m_currentPosition.z + shakeOffset.z);
  *outTarget =
      math::Vec3(m_currentTarget.x + shakeOffset.x,
                  m_currentTarget.y + shakeOffset.y,
                  m_currentTarget.z + shakeOffset.z);
  *outUp = m_currentUp;
  *outFov = m_currentFov;
  *outNear = m_currentNear;
  *outFar = m_currentFar;
}

void CameraManager::clear() noexcept {
  for (auto &cam : m_cameras) {
    cam = CameraEntry{};
  }
  for (auto &shake : m_shakes) {
    shake = CameraShakeEntry{};
  }
  m_hasEvaluated = false;
}

std::size_t CameraManager::camera_count() const noexcept {
  std::size_t count = 0U;
  for (const auto &cam : m_cameras) {
    if (cam.active) {
      ++count;
    }
  }
  return count;
}

std::size_t CameraManager::shake_count() const noexcept {
  std::size_t count = 0U;
  for (const auto &shake : m_shakes) {
    if (shake.active) {
      ++count;
    }
  }
  return count;
}

} // namespace engine::runtime
