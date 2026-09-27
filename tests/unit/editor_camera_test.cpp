// Verifies the Scene view's projection helpers under both clip depth
// conventions: GL's [-1, 1] and the [0, 1] of D3D, Metal and Vulkan. A
// world point's viewport ray passes back through the point and starts on
// the near plane, and the frustum's corners sit on the near and far
// planes. The frustum overlay had unprojected the GL cube on every
// device, which puts its near face at half the near distance on a [0, 1]
// device.

#include "engine/editor/editor_camera.h"

#include "engine/math/mat4.h"
#include "engine/math/ray.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

#include "../test_harness.h"

#include <cmath>

namespace {

using engine::editor::editor_camera_state;
using engine::editor::EditorCamera;
using engine::editor::frustum_corners;
using engine::editor::viewport_ray;
using engine::math::Mat4;
using engine::math::Vec3;
using engine::math::Vec4;

constexpr float kFov = 1.0471975512F;
constexpr float kAspect = 16.0F / 9.0F;
constexpr float kNear = 0.1F;
constexpr float kFar = 100.0F;

// Float inverses of a 1000:1 perspective: the near plane unprojects to a
// few ulps of relative error, while depth resolution near the far plane is
// coarse (dz/dd ~ n/d^2), so the far plane is held to 1e-3 relative.
constexpr float kNearRelTol = 1.0e-4F;
constexpr float kFarRelTol = 1.0e-3F;

Mat4 projection(bool zeroOne) noexcept {
  return zeroOne
             ? engine::math::perspective_zero_one(kFov, kAspect, kNear, kFar)
             : engine::math::perspective(kFov, kAspect, kNear, kFar);
}

/// Depth of `point` in front of the camera, along its view direction.
float view_depth(const Vec3 &eye, const Vec3 &forward,
                 const Vec3 &point) noexcept {
  return engine::math::dot(engine::math::sub(point, eye), forward);
}

bool near_rel(float actual, float expected, float relTol) noexcept {
  return std::fabs(actual - expected) <= relTol * std::fabs(expected);
}

void check_convention(engine::tests::TestContext &t, bool zeroOne,
                      const char *name) noexcept {
  EditorCamera editorCamera{};
  editorCamera.yaw = 0.7F;
  editorCamera.pitch = 0.4F;
  editorCamera.distance = 12.0F;
  const engine::renderer::CameraState cam = editor_camera_state(editorCamera);
  const Mat4 view = engine::math::look_at(cam.position, cam.target, cam.up);
  const Mat4 proj = projection(zeroOne);
  const Vec3 forward =
      engine::math::normalize(engine::math::sub(cam.target, cam.position));

  // A point off-centre in view: project it, then cast the ray back.
  const Vec3 point(1.5F, 0.75F, -2.0F);
  const Vec4 clip = engine::math::mul(engine::math::mul(proj, view),
                                      Vec4(point.x, point.y, point.z, 1.0F));
  const float ndcX = clip.x / clip.w;
  const float ndcY = clip.y / clip.w;
  engine::math::Ray ray{};
  t.check(viewport_ray(view, proj, zeroOne, ndcX, ndcY, &ray), name);

  // Distance from the point to the ray's line, relative to its depth.
  const Vec3 toPoint = engine::math::sub(point, ray.origin);
  const Vec3 dir = engine::math::normalize(ray.direction);
  const Vec3 offLine = engine::math::sub(
      toPoint, engine::math::mul(dir, engine::math::dot(toPoint, dir)));
  const float depth = view_depth(cam.position, forward, point);
  t.check(engine::math::length(offLine) <= kNearRelTol * depth,
          zeroOne ? "[0,1]: the ray passes back through the point"
                  : "[-1,1]: the ray passes back through the point");
  t.check(near_rel(view_depth(cam.position, forward, ray.origin), kNear,
                   kNearRelTol),
          zeroOne ? "[0,1]: the ray starts on the near plane"
                  : "[-1,1]: the ray starts on the near plane");
  const Vec3 rayEnd = engine::math::add(ray.origin, ray.direction);
  t.check(near_rel(view_depth(cam.position, forward, rayEnd), kFar, kFarRelTol),
          zeroOne ? "[0,1]: the ray reaches the far plane"
                  : "[-1,1]: the ray reaches the far plane");

  Vec3 corners[8]{};
  t.check(frustum_corners(view, proj, zeroOne, corners), name);
  bool nearOk = true;
  bool farOk = true;
  for (int i = 0; i < 4; ++i) {
    nearOk = nearOk && near_rel(view_depth(cam.position, forward, corners[i]),
                                kNear, kNearRelTol);
    farOk = farOk && near_rel(view_depth(cam.position, forward, corners[i + 4]),
                              kFar, kFarRelTol);
  }
  t.check(nearOk, zeroOne ? "[0,1]: the near corners sit on the near plane"
                          : "[-1,1]: the near corners sit on the near plane");
  t.check(farOk, zeroOne ? "[0,1]: the far corners sit on the far plane"
                         : "[-1,1]: the far corners sit on the far plane");

  // The far quad's width at depth `far` matches the field of view.
  const float halfHeight = kFar * std::tan(kFov * 0.5F);
  const float quadHeight =
      engine::math::length(engine::math::sub(corners[7], corners[4]));
  t.check(near_rel(quadHeight, 2.0F * halfHeight, kFarRelTol),
          zeroOne ? "[0,1]: the far quad spans the field of view"
                  : "[-1,1]: the far quad spans the field of view");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  check_convention(t, false, "[-1,1]: helpers succeed");
  check_convention(t, true, "[0,1]: helpers succeed");

  // A singular projection is refused, not unprojected into garbage.
  Vec3 corners[8]{};
  engine::math::Ray ray{};
  Mat4 zero{};
  for (Vec4 &column : zero.columns) {
    column = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
  }
  t.check(!frustum_corners(zero, zero, false, corners) &&
              !viewport_ray(zero, zero, false, 0.0F, 0.0F, &ray),
          "a singular matrix is refused");
  return t.finish("editor_camera");
}
