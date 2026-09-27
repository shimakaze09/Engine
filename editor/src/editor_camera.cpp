// Implements editor camera behavior for the Engine editor tool.

#include "engine/editor/editor_camera.h"

#include <algorithm>
#include <cmath>

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

renderer::CameraState editor_camera_state(const EditorCamera &camera) noexcept {
  const float cosPitch = std::cos(camera.pitch);
  const float sinPitch = std::sin(camera.pitch);
  const float cosYaw = std::cos(camera.yaw);
  const float sinYaw = std::sin(camera.yaw);

  const math::Vec3 offset(cosPitch * sinYaw * camera.distance,
                          sinPitch * camera.distance,
                          cosPitch * cosYaw * camera.distance);

  renderer::CameraState state{};
  state.position = math::add(camera.target, offset);
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

/// The clip-space depth of the near plane under the projection's
/// convention; the far plane is 1 under both.
constexpr float near_clip_depth(bool depthZeroToOne) noexcept {
  return depthZeroToOne ? 0.0F : -1.0F;
}

bool unproject(const math::Mat4 &inverseViewProjection, float x, float y,
               float z, math::Vec3 *out) noexcept {
  const math::Vec4 world =
      math::mul(inverseViewProjection, math::Vec4(x, y, z, 1.0F));
  if (std::fabs(world.w) < 1.0e-12F) {
    return false;
  }
  const float invW = 1.0F / world.w;
  *out = math::Vec3(world.x * invW, world.y * invW, world.z * invW);
  return true;
}

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
      !unproject(inverseVP, ndcX, ndcY, near_clip_depth(depthZeroToOne),
                 &nearPoint) ||
      !unproject(inverseVP, ndcX, ndcY, 1.0F, &farPoint)) {
    return false;
  }
  out->origin = nearPoint;
  out->direction = math::sub(farPoint, nearPoint);
  return true;
}

bool frustum_corners(const math::Mat4 &view, const math::Mat4 &projection,
                     bool depthZeroToOne, math::Vec3 (&out)[8]) noexcept {
  math::Mat4 inverseVP{};
  if (!inverse_view_projection(view, projection, &inverseVP)) {
    return false;
  }
  constexpr float kQuad[4][2] = {
      {-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}};
  const float depths[2] = {near_clip_depth(depthZeroToOne), 1.0F};
  for (int plane = 0; plane < 2; ++plane) {
    for (int corner = 0; corner < 4; ++corner) {
      if (!unproject(inverseVP, kQuad[corner][0], kQuad[corner][1],
                     depths[plane], &out[(plane * 4) + corner])) {
        return false;
      }
    }
  }
  return true;
}

} // namespace engine::editor
