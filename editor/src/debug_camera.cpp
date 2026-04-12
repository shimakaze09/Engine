#include "engine/editor/debug_camera.h"

#include <cmath>

#include "engine/core/debug_draw.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

namespace engine::editor {

namespace {

constexpr float kSpeedBoost = 3.0F;

math::Vec3 camera_forward(float yaw, float pitch) noexcept {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);
  return math::Vec3(cp * sy, sp, cp * cy);
}

math::Vec3 camera_right(float yaw) noexcept {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  return math::Vec3(cy, 0.0F, -sy);
}

} // namespace

void update_debug_camera(DebugCamera &camera, float dt, bool forward, bool back,
                         bool left, bool right, bool up, bool down, bool shift,
                         int mouseDeltaX, int mouseDeltaY) noexcept {
  // Mouse look
  camera.yaw -= static_cast<float>(mouseDeltaX) * camera.lookSensitivity;
  camera.pitch -= static_cast<float>(mouseDeltaY) * camera.lookSensitivity;
  if (camera.pitch < DebugCamera::kMinPitch) {
    camera.pitch = DebugCamera::kMinPitch;
  }
  if (camera.pitch > DebugCamera::kMaxPitch) {
    camera.pitch = DebugCamera::kMaxPitch;
  }

  // Movement
  float speed = camera.moveSpeed * dt;
  if (shift) {
    speed *= kSpeedBoost;
  }

  const math::Vec3 fwd = camera_forward(camera.yaw, camera.pitch);
  const math::Vec3 rgt = camera_right(camera.yaw);
  constexpr math::Vec3 worldUp(0.0F, 1.0F, 0.0F);

  if (forward) {
    camera.position = math::add(camera.position, math::mul(fwd, speed));
  }
  if (back) {
    camera.position = math::add(camera.position, math::mul(fwd, -speed));
  }
  if (right) {
    camera.position = math::add(camera.position, math::mul(rgt, speed));
  }
  if (left) {
    camera.position = math::add(camera.position, math::mul(rgt, -speed));
  }
  if (up) {
    camera.position = math::add(camera.position, math::mul(worldUp, speed));
  }
  if (down) {
    camera.position = math::add(camera.position, math::mul(worldUp, -speed));
  }
}

renderer::CameraState debug_camera_state(const DebugCamera &camera) noexcept {
  const math::Vec3 fwd = camera_forward(camera.yaw, camera.pitch);

  renderer::CameraState state{};
  state.position = camera.position;
  state.target = math::add(camera.position, fwd);
  state.up = math::Vec3(0.0F, 1.0F, 0.0F);
  return state;
}

void draw_camera_frustum_wireframe(const renderer::CameraState &frozenCamera,
                                   float aspectRatio) noexcept {
  // Compute view and projection matrices, then extract frustum corners.
  const math::Mat4 view = math::look_at(frozenCamera.position,
                                        frozenCamera.target, frozenCamera.up);
  const math::Mat4 proj =
      math::perspective(frozenCamera.fovRadians, aspectRatio,
                        frozenCamera.nearPlane, frozenCamera.farPlane);
  const math::Mat4 vp = math::mul(proj, view);

  // Invert VP to unproject NDC corners.
  math::Mat4 invVP{};
  if (!math::inverse(vp, &invVP)) {
    return; // Degenerate matrix
  }

  // NDC corners: 8 corners of the unit cube [-1,1]^3
  // near z = -1, far z = 1 in OpenGL NDC
  constexpr float kNDC[8][4] = {
      {-1.0F, -1.0F, -1.0F, 1.0F}, // near-bottom-left
      {1.0F, -1.0F, -1.0F, 1.0F},  // near-bottom-right
      {1.0F, 1.0F, -1.0F, 1.0F},   // near-top-right
      {-1.0F, 1.0F, -1.0F, 1.0F},  // near-top-left
      {-1.0F, -1.0F, 1.0F, 1.0F},  // far-bottom-left
      {1.0F, -1.0F, 1.0F, 1.0F},   // far-bottom-right
      {1.0F, 1.0F, 1.0F, 1.0F},    // far-top-right
      {-1.0F, 1.0F, 1.0F, 1.0F},   // far-top-left
  };

  math::Vec3 corners[8]{};
  for (int i = 0; i < 8; ++i) {
    const math::Vec4 ndc(kNDC[i][0], kNDC[i][1], kNDC[i][2], kNDC[i][3]);
    const math::Vec4 world = math::mul(invVP, ndc);
    if (std::fabs(world.w) < 0.0001F) {
      return;
    }
    const float invW = 1.0F / world.w;
    corners[i] = math::Vec3(world.x * invW, world.y * invW, world.z * invW);
  }

  const core::DebugColor color{1.0F, 1.0F, 0.0F, 1.0F}; // yellow

  auto toDbg = [](const math::Vec3 &v) -> core::DebugVec3 {
    return {v.x, v.y, v.z};
  };

  // Near quad
  core::debug_draw_line(toDbg(corners[0]), toDbg(corners[1]), color);
  core::debug_draw_line(toDbg(corners[1]), toDbg(corners[2]), color);
  core::debug_draw_line(toDbg(corners[2]), toDbg(corners[3]), color);
  core::debug_draw_line(toDbg(corners[3]), toDbg(corners[0]), color);

  // Far quad
  core::debug_draw_line(toDbg(corners[4]), toDbg(corners[5]), color);
  core::debug_draw_line(toDbg(corners[5]), toDbg(corners[6]), color);
  core::debug_draw_line(toDbg(corners[6]), toDbg(corners[7]), color);
  core::debug_draw_line(toDbg(corners[7]), toDbg(corners[4]), color);

  // Connecting edges
  core::debug_draw_line(toDbg(corners[0]), toDbg(corners[4]), color);
  core::debug_draw_line(toDbg(corners[1]), toDbg(corners[5]), color);
  core::debug_draw_line(toDbg(corners[2]), toDbg(corners[6]), color);
  core::debug_draw_line(toDbg(corners[3]), toDbg(corners[7]), color);
}

} // namespace engine::editor
