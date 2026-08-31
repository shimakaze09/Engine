// Implements camera manager behavior for the Engine runtime world.

#include "engine/runtime/camera_manager.h"

#include <cmath>
#include <cstdint>

#include "engine/core/logging.h"

namespace engine::runtime {

namespace {
constexpr const char *kLogChannel = "camera";

float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

math::Vec3 lerp_vec3(const math::Vec3 &a, const math::Vec3 &b,
                     float t) noexcept {
  return math::Vec3(lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t));
}

/// True when a Vec3 carries only finite components.
bool finite_vec3(const math::Vec3 &v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Validates a camera entry and its priority at the manager boundary.
/// Every field evaluate() folds into frame state must be finite — one NaN
/// contaminates blend state, the view/culling camera, and the audio
/// listener until a clear resets it — and the documented range
/// relationships hold: a positive near plane, far beyond near, a
/// nonnegative blend speed, and a positive orthographic size when the
/// entry selects the orthographic projection.
bool camera_entry_valid(const CameraEntry &entry, float priority) noexcept {
  if (!finite_vec3(entry.position) || !finite_vec3(entry.target) ||
      !finite_vec3(entry.up)) {
    return false;
  }
  if (!std::isfinite(entry.fovRadians) || !std::isfinite(entry.nearPlane) ||
      !std::isfinite(entry.farPlane) ||
      !std::isfinite(entry.orthographicSize) ||
      !std::isfinite(entry.blendSpeed) || !std::isfinite(priority)) {
    return false;
  }
  if (!(entry.nearPlane > 0.0F) || !(entry.farPlane > entry.nearPlane)) {
    return false;
  }
  if (entry.blendSpeed < 0.0F) {
    return false;
  }
  if ((entry.projection != 0U) && !(entry.orthographicSize > 0.0F)) {
    return false;
  }
  return true;
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

bool CameraManager::push_camera(core::Entity ownerEntity,
                                const CameraEntry &entry,
                                float priority) noexcept {
  if (ownerEntity == core::kInvalidEntity) {
    return false;
  }

  // Validated before any slot is touched, so a refused push — a fresh
  // entry or an update to a live one — leaves the stack and the current
  // evaluated state exactly as they were.
  if (!camera_entry_valid(entry, priority)) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "push_camera rejected non-finite or out-of-range "
                      "camera parameters");
    return false;
  }

  for (auto &cam : m_cameras) {
    if (cam.active && (cam.ownerEntity == ownerEntity)) {
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

  for (auto &cam : m_cameras) {
    if (!cam.active) {
      cam = entry;
      cam.ownerEntity = ownerEntity;
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

bool CameraManager::pop_camera(core::Entity ownerEntity) noexcept {
  if (ownerEntity == core::kInvalidEntity) {
    return false;
  }

  for (auto &cam : m_cameras) {
    if (cam.active && (cam.ownerEntity == ownerEntity)) {
      cam.active = false;
      return true;
    }
  }
  return false;
}

void CameraManager::on_entity_destroyed(core::Entity entity) noexcept {
  static_cast<void>(pop_camera(entity));
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
  // Same boundary rule as push_camera: shake parameters feed the evaluated
  // camera additively every frame, so non-finite values, a non-positive
  // duration, or a negative amplitude/frequency/decay are refused before
  // any slot activates.
  if (!std::isfinite(amplitude) || !std::isfinite(frequency) ||
      !std::isfinite(duration) || !std::isfinite(decay) ||
      !(duration > 0.0F) || (amplitude < 0.0F) || (frequency < 0.0F) ||
      (decay < 0.0F)) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "add_shake rejected non-finite or out-of-range shake "
                      "parameters");
    return false;
  }

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

/// Simple value noise: integer-lattice hash to [-1, 1] with smoothstep
/// interpolation between lattice points.
float CameraManager::noise1d(float t) noexcept {
  if (!std::isfinite(t)) {
    return 0.0F;
  }

  const double lattice = std::floor(static_cast<double>(t));
  constexpr double kUint32Period = 4294967296.0;
  double wrappedLattice = std::fmod(lattice, kUint32Period);
  if (wrappedLattice < 0.0) {
    wrappedLattice += kUint32Period;
  }
  const std::uint32_t i0 = static_cast<std::uint32_t>(wrappedLattice);
  const float frac = static_cast<float>(static_cast<double>(t) - lattice);

  const auto hash = [](std::uint32_t value) noexcept -> float {
    const std::uint32_t mixed = (value << 13U) ^ value;
    const std::uint32_t hashed =
        (mixed * ((mixed * mixed * 15731U) + 789221U) + 1376312589U) &
        0x7FFFFFFFU;
    return 1.0F - static_cast<float>(hashed) / 1073741824.0F;
  };
  const float v0 = hash(i0);
  const float v1 = hash(i0 + 1U);
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

  CameraEntry evaluated{};
  evaluate(dt, &evaluated);
  *outPosition = evaluated.position;
  *outTarget = evaluated.target;
  *outUp = evaluated.up;
  *outFov = evaluated.fovRadians;
  *outNear = evaluated.nearPlane;
  *outFar = evaluated.farPlane;
}

/// Struct-filling primary (#221): also carries the projection kind and
/// orthographic size. The kind never interpolates — the winning camera's
/// projection applies instantly (there is no meaningful matrix blend
/// between perspective and orthographic) while orthographicSize lerps
/// exactly like fovRadians, its perspective analogue.
void CameraManager::evaluate(float dt, CameraEntry *outCamera) noexcept {
  if (outCamera == nullptr) {
    return;
  }

  const CameraEntry *best = active_camera();
  if (best == nullptr) {
    outCamera->position = m_currentPosition;
    outCamera->target = m_currentTarget;
    outCamera->up = m_currentUp;
    outCamera->fovRadians = m_currentFov;
    outCamera->nearPlane = m_currentNear;
    outCamera->farPlane = m_currentFar;
    outCamera->projection = m_currentProjection;
    outCamera->orthographicSize = m_currentOrthoSize;
    return;
  }

  for (auto &cam : m_cameras) {
    if (!cam.active) {
      continue;
    }
    if (&cam == best) {
      cam.blendWeight = clamp01(cam.blendWeight + cam.blendSpeed * dt);
    } else {
      cam.blendWeight = clamp01(cam.blendWeight - cam.blendSpeed * dt);
    }
  }

  const float t = clamp01(best->blendWeight);

  if (!m_hasEvaluated) {
    m_currentPosition = best->position;
    m_currentTarget = best->target;
    m_currentUp = best->up;
    m_currentFov = best->fovRadians;
    m_currentNear = best->nearPlane;
    m_currentFar = best->farPlane;
    m_currentOrthoSize = best->orthographicSize;
    m_hasEvaluated = true;
  } else {
    m_currentPosition = lerp_vec3(m_currentPosition, best->position, t);
    m_currentTarget = lerp_vec3(m_currentTarget, best->target, t);
    m_currentUp = lerp_vec3(m_currentUp, best->up, t);
    m_currentFov = lerp(m_currentFov, best->fovRadians, t);
    m_currentNear = lerp(m_currentNear, best->nearPlane, t);
    m_currentFar = lerp(m_currentFar, best->farPlane, t);
    m_currentOrthoSize = lerp(m_currentOrthoSize, best->orthographicSize, t);
  }
  m_currentProjection = best->projection;

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
    const float envelope = shake.amplitude * std::exp(-shake.decay * progress);
    const float phase = shake.elapsed * shake.frequency;
    shakeOffset.x += envelope * noise1d(phase);
    shakeOffset.y += envelope * noise1d(phase + 100.0F);
    shakeOffset.z += envelope * noise1d(phase + 200.0F);
  }

  outCamera->position = math::Vec3(m_currentPosition.x + shakeOffset.x,
                                   m_currentPosition.y + shakeOffset.y,
                                   m_currentPosition.z + shakeOffset.z);
  outCamera->target = math::Vec3(m_currentTarget.x + shakeOffset.x,
                                 m_currentTarget.y + shakeOffset.y,
                                 m_currentTarget.z + shakeOffset.z);
  outCamera->up = m_currentUp;
  outCamera->fovRadians = m_currentFov;
  outCamera->nearPlane = m_currentNear;
  outCamera->farPlane = m_currentFar;
  outCamera->projection = m_currentProjection;
  outCamera->orthographicSize = m_currentOrthoSize;
}

void CameraManager::clear() noexcept {
  for (auto &cam : m_cameras) {
    cam = CameraEntry{};
  }
  for (auto &shake : m_shakes) {
    shake = CameraShakeEntry{};
  }
  m_hasEvaluated = false;
  m_currentProjection = 0U;
  m_currentOrthoSize = 5.0F;
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
