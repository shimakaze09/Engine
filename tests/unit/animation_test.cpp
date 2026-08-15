// Verifies CPU skeletal pose evaluation: clip sampling (step, linear,
// clamped ends, single-key, unanimated joints), shortest-path rotation
// blending, hierarchy composition order, and skinning-palette identity at
// the bind pose.

#include <cmath>
#include <cstddef>
#include <vector>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/runtime/animation.h"

namespace {

using engine::runtime::AnimationClip;
using engine::runtime::AnimInterp;
using engine::runtime::AnimSkeleton;
using engine::runtime::AnimTarget;
using engine::runtime::AnimTrackDesc;
using engine::runtime::JointPose;
using engine::runtime::kInvalidAnimJoint;
namespace math = engine::math;

/// Absolute tolerance for values that pass through trig/normalization.
constexpr float kEps = 1.0e-6F;

bool near(float a, float b) { return std::fabs(a - b) <= kEps; }

bool near3(const math::Vec3 &v, float x, float y, float z) {
  return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

/// Two-joint skeleton (root + child) with rest child offset (0, 1, 0).
AnimSkeleton make_two_joint_skeleton() {
  AnimSkeleton skeleton{};
  skeleton.jointCount = 2U;
  skeleton.parents[0] = kInvalidAnimJoint;
  skeleton.parents[1] = 0U;
  skeleton.restPose[1].translation = math::Vec3(0.0F, 1.0F, 0.0F);
  return skeleton;
}

/// Appends floats to clip.payload. AnimationClip::payload is a fixed-count
/// NothrowBuffer (audit #174), not a std::vector, so it has no incremental
/// push_back; production loaders always know their total float count
/// upfront and never need to grow one. Tests build a track at a time, so
/// this rebuilds a merged buffer via one allocate + copy per call — a
/// pattern that is fine off the noexcept hot path this buffer type exists
/// to protect.
bool append_payload(AnimationClip &clip, const float *values,
                    std::size_t count) {
  const std::size_t oldSize = clip.payload.size();
  std::vector<float> merged(oldSize + count);
  for (std::size_t i = 0U; i < oldSize; ++i) {
    merged[i] = clip.payload[i];
  }
  for (std::size_t i = 0U; i < count; ++i) {
    merged[oldSize + i] = values[i];
  }
  return clip.payload.assign(merged.data(), merged.size());
}

/// Appends a vec3 track with the given keys; returns the track index.
std::uint32_t add_vec3_track(AnimationClip &clip, std::uint32_t joint,
                             AnimTarget target, AnimInterp interp,
                             const float *times, const math::Vec3 *values,
                             std::uint32_t keyCount) {
  AnimTrackDesc &track = clip.tracks[clip.trackCount];
  track.joint = joint;
  track.target = target;
  track.interpolation = interp;
  track.keyCount = keyCount;
  track.timesOffset = static_cast<std::uint32_t>(clip.payload.size());
  static_cast<void>(append_payload(clip, times, keyCount));
  track.valuesOffset = static_cast<std::uint32_t>(clip.payload.size());
  std::vector<float> flatValues;
  flatValues.reserve(static_cast<std::size_t>(keyCount) * 3U);
  for (std::uint32_t k = 0U; k < keyCount; ++k) {
    flatValues.push_back(values[k].x);
    flatValues.push_back(values[k].y);
    flatValues.push_back(values[k].z);
  }
  static_cast<void>(append_payload(clip, flatValues.data(), flatValues.size()));
  return clip.trackCount++;
}

/// Appends a rotation track with the given keys; returns the track index.
std::uint32_t add_quat_track(AnimationClip &clip, std::uint32_t joint,
                             AnimInterp interp, const float *times,
                             const math::Quat *values,
                             std::uint32_t keyCount) {
  AnimTrackDesc &track = clip.tracks[clip.trackCount];
  track.joint = joint;
  track.target = AnimTarget::Rotation;
  track.interpolation = interp;
  track.keyCount = keyCount;
  track.timesOffset = static_cast<std::uint32_t>(clip.payload.size());
  static_cast<void>(append_payload(clip, times, keyCount));
  track.valuesOffset = static_cast<std::uint32_t>(clip.payload.size());
  std::vector<float> flatValues;
  flatValues.reserve(static_cast<std::size_t>(keyCount) * 4U);
  for (std::uint32_t k = 0U; k < keyCount; ++k) {
    flatValues.push_back(values[k].x);
    flatValues.push_back(values[k].y);
    flatValues.push_back(values[k].z);
    flatValues.push_back(values[k].w);
  }
  static_cast<void>(append_payload(clip, flatValues.data(), flatValues.size()));
  return clip.trackCount++;
}

/// Linear sampling: exact values at keys, exact midpoint, clamped ends,
/// and rest pose preserved on unanimated joints and channels.
int check_linear_sampling() {
  const AnimSkeleton skeleton = make_two_joint_skeleton();
  AnimationClip clip{};
  clip.durationSeconds = 2.0F;
  const float times[2] = {0.0F, 2.0F};
  const math::Vec3 values[2] = {math::Vec3(0.0F, 0.0F, 0.0F),
                                math::Vec3(4.0F, -2.0F, 8.0F)};
  add_vec3_track(clip, 0U, AnimTarget::Translation, AnimInterp::Linear, times,
                 values, 2U);

  JointPose pose[2] = {};
  sample_clip_pose(skeleton, clip, 0.0F, pose);
  if (!near3(pose[0].translation, 0.0F, 0.0F, 0.0F)) {
    return 10;
  }

  sample_clip_pose(skeleton, clip, 1.0F, pose);
  if ((pose[0].translation.x != 2.0F) || (pose[0].translation.y != -1.0F) ||
      (pose[0].translation.z != 4.0F)) {
    return 11;
  }

  sample_clip_pose(skeleton, clip, 2.0F, pose);
  if (!near3(pose[0].translation, 4.0F, -2.0F, 8.0F)) {
    return 12;
  }

  sample_clip_pose(skeleton, clip, 5.0F, pose);
  if (!near3(pose[0].translation, 4.0F, -2.0F, 8.0F)) {
    return 13;
  }
  sample_clip_pose(skeleton, clip, -1.0F, pose);
  if (!near3(pose[0].translation, 0.0F, 0.0F, 0.0F)) {
    return 14;
  }

  if (!near3(pose[1].translation, 0.0F, 1.0F, 0.0F)) {
    return 15;
  }
  if ((pose[0].scale.x != 1.0F) || (pose[0].rotation.w != 1.0F)) {
    return 16;
  }
  return 0;
}

/// Step sampling holds the earlier key until the next key time.
int check_step_sampling() {
  const AnimSkeleton skeleton = make_two_joint_skeleton();
  AnimationClip clip{};
  clip.durationSeconds = 2.0F;
  const float times[3] = {0.0F, 1.0F, 2.0F};
  const math::Vec3 values[3] = {math::Vec3(0.0F, 0.0F, 0.0F),
                                math::Vec3(5.0F, 0.0F, 0.0F),
                                math::Vec3(9.0F, 0.0F, 0.0F)};
  add_vec3_track(clip, 0U, AnimTarget::Translation, AnimInterp::Step, times,
                 values, 3U);

  JointPose pose[2] = {};
  sample_clip_pose(skeleton, clip, 0.999F, pose);
  if (pose[0].translation.x != 0.0F) {
    return 20;
  }
  sample_clip_pose(skeleton, clip, 1.0F, pose);
  if (pose[0].translation.x != 5.0F) {
    return 21;
  }
  sample_clip_pose(skeleton, clip, 1.5F, pose);
  if (pose[0].translation.x != 5.0F) {
    return 22;
  }
  sample_clip_pose(skeleton, clip, 2.0F, pose);
  if (pose[0].translation.x != 9.0F) {
    return 23;
  }
  return 0;
}

/// Rotation midpoint between identity and 90° about Z is exactly the 45°
/// rotation (nlerp equals slerp at the midpoint of equal-weight keys).
int check_rotation_midpoint() {
  const AnimSkeleton skeleton = make_two_joint_skeleton();
  AnimationClip clip{};
  clip.durationSeconds = 1.0F;
  const float times[2] = {0.0F, 1.0F};
  const math::Quat values[2] = {
      math::Quat(0.0F, 0.0F, 0.0F, 1.0F),
      math::Quat(0.0F, 0.0F, 0.70710678F, 0.70710678F)};
  add_quat_track(clip, 0U, AnimInterp::Linear, times, values, 2U);

  JointPose pose[2] = {};
  sample_clip_pose(skeleton, clip, 0.5F, pose);
  if (!near(pose[0].rotation.z, 0.38268343F) ||
      !near(pose[0].rotation.w, 0.92387953F) ||
      !near(pose[0].rotation.x, 0.0F) || !near(pose[0].rotation.y, 0.0F)) {
    return 30;
  }
  return 0;
}

/// Shortest-path blending: blending toward the negated equivalent rotation
/// must not travel the long way around.
int check_shortest_path_blend() {
  JointPose a[1] = {};
  JointPose b[1] = {};
  a[0].rotation = math::Quat(0.0F, 0.0F, 0.0F, 1.0F);
  b[0].rotation = math::Quat(0.0F, 0.0F, -0.70710678F, -0.70710678F);

  JointPose out[1] = {};
  engine::runtime::blend_poses(a, b, 1U, 0.5F, out);
  if (!near(out[0].rotation.z, 0.38268343F) ||
      !near(out[0].rotation.w, 0.92387953F)) {
    return 40;
  }

  a[0].translation = math::Vec3(2.0F, 0.0F, 0.0F);
  b[0].translation = math::Vec3(4.0F, 6.0F, 0.0F);
  engine::runtime::blend_poses(a, b, 1U, 0.5F, out);
  if ((out[0].translation.x != 3.0F) || (out[0].translation.y != 3.0F)) {
    return 41;
  }
  return 0;
}

/// Hierarchy composition: the child's model-space position follows the
/// root's rotation exactly (90° about Z maps (0,1,0) to (-1,0,0)).
int check_hierarchy_composition() {
  const AnimSkeleton skeleton = make_two_joint_skeleton();
  JointPose pose[2] = {};
  pose[0] = skeleton.restPose[0];
  pose[1] = skeleton.restPose[1];
  pose[0].translation = math::Vec3(1.0F, 0.0F, 0.0F);
  pose[0].rotation = math::from_axis_angle(math::Vec3(0.0F, 0.0F, 1.0F),
                                           1.57079632679F);

  math::Mat4 global[2] = {};
  engine::runtime::compute_global_pose(skeleton, pose, global);

  const float childX = global[1].columns[3].x;
  const float childY = global[1].columns[3].y;
  const float childZ = global[1].columns[3].z;
  if (!near(childX, 0.0F) || !near(childY, 0.0F) || !near(childZ, 0.0F)) {
    return 50;
  }

  const float rootX = global[0].columns[3].x;
  if (!near(rootX, 1.0F)) {
    return 51;
  }
  return 0;
}

/// At the bind pose the skinning palette is exactly identity when the
/// inverse binds are the exact inverses of the rest globals.
int check_bind_pose_palette_identity() {
  AnimSkeleton skeleton = make_two_joint_skeleton();
  skeleton.inverseBind[0] = math::Mat4();
  skeleton.inverseBind[1] = math::Mat4();
  skeleton.inverseBind[1].columns[3] = math::Vec4(0.0F, -1.0F, 0.0F, 1.0F);

  JointPose pose[2] = {skeleton.restPose[0], skeleton.restPose[1]};
  math::Mat4 global[2] = {};
  engine::runtime::compute_global_pose(skeleton, pose, global);
  math::Mat4 palette[2] = {};
  engine::runtime::compute_skinning_palette(skeleton, global, palette);

  for (int j = 0; j < 2; ++j) {
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        const float expected = (c == r) ? 1.0F : 0.0F;
        const float *column = &palette[j].columns[c].x;
        if (column[r] != expected) {
          return 60;
        }
      }
    }
  }
  return 0;
}

/// Parent-order validation accepts the ordered skeleton and rejects a
/// child-before-parent layout.
int check_parent_order_validation() {
  AnimSkeleton good = make_two_joint_skeleton();
  if (!engine::runtime::anim_skeleton_parents_ordered(good)) {
    return 70;
  }
  AnimSkeleton bad{};
  bad.jointCount = 2U;
  bad.parents[0] = 1U;
  bad.parents[1] = kInvalidAnimJoint;
  if (engine::runtime::anim_skeleton_parents_ordered(bad)) {
    return 71;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_linear_sampling();
  if (result != 0) {
    return result;
  }
  result = check_step_sampling();
  if (result != 0) {
    return result;
  }
  result = check_rotation_midpoint();
  if (result != 0) {
    return result;
  }
  result = check_shortest_path_blend();
  if (result != 0) {
    return result;
  }
  result = check_hierarchy_composition();
  if (result != 0) {
    return result;
  }
  result = check_bind_pose_palette_identity();
  if (result != 0) {
    return result;
  }
  return check_parent_order_validation();
}
