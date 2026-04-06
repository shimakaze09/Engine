#include "engine/editor/editor_camera.h"

#include <cmath>

#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"

namespace engine::editor {

namespace {

constexpr float kOrbitSensitivity = 0.005F;
constexpr float kPanSensitivity = 0.01F;
constexpr float kZoomFactor = 0.1F;

} // namespace

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

    // Right vector (horizontal plane).
    const math::Vec3 right(cosYaw, 0.0F, -sinYaw);
    // Up vector (world up).
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
  return state;
}

} // namespace engine::editor
