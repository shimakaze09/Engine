// Implements editor camera behavior for the Engine editor tool.

#include "engine/editor/editor_camera.h"

#include <algorithm>
#include <cmath>

#include "engine/math/frustum.h"
#include "engine/math/mat4.h"
#include "engine/math/ray.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"

namespace engine::editor {

namespace {

constexpr float kOrbitSensitivity = 0.005F;
constexpr float kPanSensitivity = 0.01F;
constexpr float kZoomFactor = 0.1F;
/// Clip planes as multiples of the orbit distance (see editor_camera_state).
constexpr float kNearPerDistance = 0.0025F;
constexpr float kFarPerDistance = 2.5F;

} // namespace

/// Advances this system for the current frame or tick for editor camera.
void update_editor_camera(EditorCamera &camera, int deltaX, int deltaY,
                          int scrollDelta, bool orbit, bool pan) noexcept {
  const float dx = static_cast<float>(deltaX);
  const float dy = static_cast<float>(deltaY);

  if (orbit) {
    camera.yaw -= dx * kOrbitSensitivity;
    camera.pitch -= dy * kOrbitSensitivity;

    if (camera.pitch < EditorCamera::kMinPitch) {
      camera.pitch = EditorCamera::kMinPitch;
    }
    if (camera.pitch > EditorCamera::kMaxPitch) {
      camera.pitch = EditorCamera::kMaxPitch;
    }
  }

  if (pan) {
    const float scale = camera.distance * kPanSensitivity;

    const float cosYaw = std::cos(camera.yaw);
    const float sinYaw = std::sin(camera.yaw);

      const math::Vec3 right(cosYaw, 0.0F, -sinYaw);
      const math::Vec3 up(0.0F, 1.0F, 0.0F);

    camera.target =
        math::add(math::add(camera.target, math::mul(right, -dx * scale)),
                  math::mul(up, dy * scale));
  }

  if (scrollDelta != 0) {
    const float factor = 1.0F - static_cast<float>(scrollDelta) * kZoomFactor;
    camera.distance *= factor;

    if (camera.distance < EditorCamera::kMinDistance) {
      camera.distance = EditorCamera::kMinDistance;
    }
    if (camera.distance > EditorCamera::kMaxDistance) {
      camera.distance = EditorCamera::kMaxDistance;
    }
  }
}

namespace {

/// The orbit offset from target to eye.
math::Vec3 orbit_offset(const EditorCamera &camera) noexcept {
  const float cosPitch = std::cos(camera.pitch);
  return math::Vec3(cosPitch * std::sin(camera.yaw) * camera.distance,
                    std::sin(camera.pitch) * camera.distance,
                    cosPitch * std::cos(camera.yaw) * camera.distance);
}

constexpr float kFlyBoost = 4.0F;
constexpr float kFlySpeedStep = 1.2F;

} // namespace

void fly_editor_camera(EditorCamera &camera, const FlyInput &input,
                       float seconds) noexcept {
  for (int notch = 0; notch < input.wheel; ++notch) {
    camera.flySpeed *= kFlySpeedStep;
  }
  for (int notch = 0; notch > input.wheel; --notch) {
    camera.flySpeed /= kFlySpeedStep;
  }
  camera.flySpeed = std::clamp(camera.flySpeed, EditorCamera::kMinFlySpeed,
                               EditorCamera::kMaxFlySpeed);

  // Turning about the eye: hold the eye, turn, and put the target back in
  // front of it at the same distance.
  const math::Vec3 eye = math::add(camera.target, orbit_offset(camera));
  camera.yaw -= input.lookX * kOrbitSensitivity;
  camera.pitch = std::clamp(camera.pitch + (input.lookY * kOrbitSensitivity),
                            EditorCamera::kMinPitch, EditorCamera::kMaxPitch);
  const math::Vec3 offset = orbit_offset(camera);
  camera.target = math::sub(eye, offset);

  if (!(seconds > 0.0F)) {
    return;
  }
  const math::Vec3 forward = math::mul(offset, -1.0F / camera.distance);
  const math::Vec3 right(std::cos(camera.yaw), 0.0F, -std::sin(camera.yaw));
  const math::Vec3 up(0.0F, 1.0F, 0.0F);
  const float step =
      camera.flySpeed * (input.boost ? kFlyBoost : 1.0F) * seconds;
  const math::Vec3 move = math::add(
      math::add(math::mul(forward, static_cast<float>(input.forward) * step),
                math::mul(right, static_cast<float>(input.right) * step)),
      math::mul(up, static_cast<float>(input.up) * step));
  camera.target = math::add(camera.target, move);
}

renderer::CameraState editor_camera_state(const EditorCamera &camera) noexcept {
  renderer::CameraState state{};
  state.position = math::add(camera.target, orbit_offset(camera));
  state.target = camera.target;
  state.up = math::Vec3(0.0F, 1.0F, 0.0F);
  // The clip planes follow the orbit distance, as Unity's scene camera
  // clips dynamically: the far plane keeps the target and one and a half
  // times its distance beyond it in view at any zoom, and the near plane
  // scales with it, so far:near stays at 1000 and the depth buffer keeps
  // the precision it has at the defaults. Both planes start scaling at
  // 40 m, below which they keep the renderer's defaults.
  const renderer::CameraState defaults{};
  state.nearPlane =
      std::max(defaults.nearPlane, kNearPerDistance * camera.distance);
  state.farPlane =
      std::max(defaults.farPlane, kFarPerDistance * camera.distance);
  return state;
}

namespace {

bool inverse_view_projection(const math::Mat4 &view,
                             const math::Mat4 &projection,
                             math::Mat4 *out) noexcept {
  return math::inverse(math::mul(projection, view), out);
}

} // namespace

bool viewport_ray(const math::Mat4 &view, const math::Mat4 &projection,
                  bool depthZeroToOne, float ndcX, float ndcY,
                  math::Ray *out) noexcept {
  math::Mat4 inverseVP{};
  math::Vec3 nearPoint{};
  math::Vec3 farPoint{};
  if ((out == nullptr) ||
      !inverse_view_projection(view, projection, &inverseVP) ||
      !math::unproject_ndc(
          inverseVP,
          math::Vec3(ndcX, ndcY, math::clip_near_depth(depthZeroToOne)),
          &nearPoint) ||
      !math::unproject_ndc(inverseVP, math::Vec3(ndcX, ndcY, 1.0F),
                           &farPoint)) {
    return false;
  }
  out->origin = nearPoint;
  out->direction = math::sub(farPoint, nearPoint);
  return true;
}

bool frustum_corners(const math::Mat4 &view, const math::Mat4 &projection,
                     bool depthZeroToOne, math::Vec3 (&out)[8]) noexcept {
  math::Mat4 inverseVP{};
  return inverse_view_projection(view, projection, &inverseVP) &&
         math::frustum_corners(inverseVP, depthZeroToOne, out);
}

} // namespace engine::editor
