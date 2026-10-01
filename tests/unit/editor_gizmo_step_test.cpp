// Verifies that a gizmo drag writes only the channel it changed, through
// gizmo_step_to_local, the conversion the Scene view's gizmo calls. Under
// a parent scaled (2,1,1), eight 45-degree world-space rotations of a
// child leave its local scale exactly (1,1,1) and its rotation unit length
// and a full turn; the old whole-matrix decomposition rewrote the scale
// on the first drag. Moving the child sets only its position, through the
// parent's inverse; scaling it stretches only the dragged axis.

#include <cmath>
#include <cstdio>

#include "../test_harness.h"
#include "editor_transform_util.h"
#include "engine/math/transform.h"

namespace {

using engine::editor::gizmo_step_to_local;
using engine::editor::GizmoChannel;
namespace math = engine::math;

engine::tests::TestContext g_tests;

/// The world pose the World composes for a child of `parent` with local
/// transform `local`, as World::get_physics_transform builds it.
engine::physics::PhysicsTransform
child_pose(const engine::physics::PhysicsTransform &parent,
           const engine::runtime::Transform &local) noexcept {
  engine::physics::PhysicsTransform pose{};
  pose.matrix =
      math::mul(parent.matrix,
                math::compose_trs(local.position, local.rotation, local.scale));
  pose.position = math::Vec3(pose.matrix.columns[3].x, pose.matrix.columns[3].y,
                             pose.matrix.columns[3].z);
  pose.rotation = math::normalize(math::mul(parent.rotation, local.rotation));
  pose.scale =
      math::Vec3(parent.scale.x * local.scale.x, parent.scale.y * local.scale.y,
                 parent.scale.z * local.scale.z);
  return pose;
}

engine::physics::PhysicsTransform stretched_parent() noexcept {
  engine::physics::PhysicsTransform parent{};
  parent.scale = math::Vec3(2.0F, 1.0F, 1.0F);
  parent.matrix = math::compose_trs(math::Vec3(), math::Quat(), parent.scale);
  return parent;
}

void check_rotate_keeps_scale() noexcept {
  const engine::physics::PhysicsTransform parent = stretched_parent();
  const math::Quat step =
      math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F), 0.785398163F);
  engine::runtime::Transform local{};
  bool converted = true;
  float worstLength = 0.0F;
  for (int drag = 0; drag < 8; ++drag) {
    const engine::physics::PhysicsTransform pose = child_pose(parent, local);
    // ImGuizmo's world-space rotate turns the matrix it was given.
    const math::Mat4 after = math::mul(math::to_mat4(step), pose.matrix);
    engine::runtime::Transform next{};
    converted =
        converted && gizmo_step_to_local(GizmoChannel::Rotate, pose.matrix,
                                         after, pose, &parent, local, &next);
    local = next;
    worstLength = std::fmax(
        worstLength,
        std::fabs(std::sqrt(math::dot(local.rotation, local.rotation)) - 1.0F));
  }
  g_tests.check(converted, "every rotate step converts");
  g_tests.check((local.scale.x == 1.0F) && (local.scale.y == 1.0F) &&
                    (local.scale.z == 1.0F),
                "rotating never rewrites the child's scale");
  g_tests.check((local.position.x == 0.0F) && (local.position.y == 0.0F) &&
                    (local.position.z == 0.0F),
                "rotating never moves the child");
  // Normalized each step: within one float ulp of 1, about 1.2e-7.
  g_tests.check(worstLength <= 1.2e-7F, "the rotation stays unit length");
  // Eight 45-degree steps are a full turn: +-identity, to the round-off of
  // eight quaternion products (about 1e-6 in each component).
  g_tests.check((std::fabs(std::fabs(local.rotation.w) - 1.0F) <= 2.0e-6F) &&
                    (std::fabs(local.rotation.z) <= 2.0e-6F),
                "eight 45-degree steps make a full turn");

  engine::runtime::Transform once{};
  const engine::physics::PhysicsTransform start =
      child_pose(parent, engine::runtime::Transform{});
  static_cast<void>(
      gizmo_step_to_local(GizmoChannel::Rotate, start.matrix,
                          math::mul(math::to_mat4(step), start.matrix), start,
                          &parent, engine::runtime::Transform{}, &once));
  // The child turns 45 degrees about Z, as the gizmo drew it.
  g_tests.check(std::fabs(once.rotation.z - std::sin(0.392699082F)) <= 1.0e-6F,
                "one step turns the child 45 degrees");
}

void check_translate_and_scale() noexcept {
  const engine::physics::PhysicsTransform parent = stretched_parent();
  engine::runtime::Transform local{};
  local.rotation = math::from_axis_angle(math::Vec3(0.0F, 1.0F, 0.0F), 0.5F);
  local.scale = math::Vec3(1.5F, 1.0F, 0.5F);
  const engine::physics::PhysicsTransform pose = child_pose(parent, local);

  math::Mat4 moved = pose.matrix;
  moved.columns[3].x += 4.0F;
  engine::runtime::Transform after{};
  g_tests.check(gizmo_step_to_local(GizmoChannel::Translate, pose.matrix, moved,
                                    pose, &parent, local, &after) &&
                    (std::fabs(after.position.x - 2.0F) <= 1.0e-6F) &&
                    (after.rotation.y == local.rotation.y) &&
                    (after.scale.x == 1.5F),
                "moving sets only the position, through the parent");

  math::Mat4 stretched = pose.matrix;
  stretched.columns[0] = math::mul(stretched.columns[0], 2.0F);
  g_tests.check(gizmo_step_to_local(GizmoChannel::Scale, pose.matrix, stretched,
                                    pose, &parent, local, &after) &&
                    (std::fabs(after.scale.x - 3.0F) <= 1.0e-6F) &&
                    (after.scale.y == 1.0F) && (after.scale.z == 0.5F) &&
                    (after.rotation.y == local.rotation.y),
                "scaling stretches only the dragged axis");

  math::Mat4 collapsed = pose.matrix;
  collapsed.columns[0] = math::Vec4(0.0F, 0.0F, 0.0F, 0.0F);
  g_tests.check(!gizmo_step_to_local(GizmoChannel::Rotate, collapsed,
                                     pose.matrix, pose, &parent, local, &after),
                "a singular basis is refused");
}

} // namespace

int main() {
  check_rotate_keeps_scale();
  check_translate_and_scale();
  return g_tests.finish("editor_gizmo_step");
}
