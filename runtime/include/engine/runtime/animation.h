// Declares skeletal animation types and CPU pose evaluation for the
// runtime: fixed-capacity skeletons, sampled clips with step/linear/cubic
// tracks, local-pose crossfade blending, hierarchy composition, and
// skinning-palette computation. Evaluation is allocation-free and
// deterministic; rotations blend by shortest-path normalized lerp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::runtime {

inline constexpr std::size_t kMaxAnimJoints = 128U;
inline constexpr std::size_t kMaxAnimTracks = kMaxAnimJoints * 3U;
inline constexpr std::uint32_t kInvalidAnimJoint = 0xFFFFFFFFU;

/// One joint's local translation/rotation/scale.
struct JointPose final {
  math::Vec3 translation = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
};

/// Fixed-capacity skeleton. Joints must be stored parent-before-child
/// (parents[i] < i for every non-root joint) — the cooker guarantees this
/// order so hierarchy composition is a single forward pass.
struct AnimSkeleton final {
  std::uint32_t jointCount = 0U;
  std::array<std::uint32_t, kMaxAnimJoints> parents{};
  std::array<math::Mat4, kMaxAnimJoints> inverseBind{};
  std::array<JointPose, kMaxAnimJoints> restPose{};
  std::array<std::uint32_t, kMaxAnimJoints> nameHashes{};
};

/// Enumerates the transform channel an animation track drives.
enum class AnimTarget : std::uint8_t {
  Translation = 0,
  Rotation = 1,
  Scale = 2,
};

/// Enumerates key interpolation modes (glTF semantics; CubicSpline is
/// Hermite with per-key in/out tangents).
enum class AnimInterp : std::uint8_t {
  Linear = 0,
  Step = 1,
  CubicSpline = 2,
};

/// One track's metadata; key data lives in the clip payload as float
/// offsets (times, then values, then cubic in/out tangents when present).
struct AnimTrackDesc final {
  std::uint32_t joint = kInvalidAnimJoint;
  AnimTarget target = AnimTarget::Translation;
  AnimInterp interpolation = AnimInterp::Linear;
  std::uint32_t keyCount = 0U;
  std::uint32_t timesOffset = 0U;
  std::uint32_t valuesOffset = 0U;
  std::uint32_t inTangentsOffset = 0U;
  std::uint32_t outTangentsOffset = 0U;
};

/// A sampled animation clip: a fixed track table over one shared payload
/// buffer (allocated at load time; evaluation only reads it).
struct AnimationClip final {
  float durationSeconds = 0.0F;
  std::uint32_t trackCount = 0U;
  std::array<AnimTrackDesc, kMaxAnimTracks> tracks{};
  std::vector<float> payload{};
};

/// True when every non-root joint's parent index precedes it (the storage
/// order evaluation requires).
bool anim_skeleton_parents_ordered(const AnimSkeleton &skeleton) noexcept;

/// Loads a cooked .skel through the VFS; validates magic, version, joint
/// budget, and the parent-before-child order.
bool load_skeleton_asset(const char *virtualPath,
                         AnimSkeleton *outSkeleton) noexcept;

/// Loads a cooked .anim through the VFS; validates magic, version, track
/// budget, and that every track's keys fit inside the payload.
bool load_animation_clip_asset(const char *virtualPath,
                               AnimationClip *outClip) noexcept;

/// Samples the clip at timeSeconds (clamped to [0, duration]) over the
/// skeleton's rest pose: joints the clip does not animate keep their rest
/// transform. outPose must hold skeleton.jointCount entries.
void sample_clip_pose(const AnimSkeleton &skeleton, const AnimationClip &clip,
                      float timeSeconds, JointPose *outPose) noexcept;

/// Blends two local poses per joint: lerp for translation/scale, shortest
/// path normalized lerp for rotation. weight 0 = a, 1 = b.
void blend_poses(const JointPose *a, const JointPose *b, std::size_t count,
                 float weight, JointPose *out) noexcept;

/// Composes local poses into model-space joint matrices in one forward pass
/// (requires the parent-before-child storage order).
void compute_global_pose(const AnimSkeleton &skeleton,
                         const JointPose *localPose,
                         math::Mat4 *outGlobal) noexcept;

/// Skinning palette per joint: model-space pose times the inverse bind.
void compute_skinning_palette(const AnimSkeleton &skeleton,
                              const math::Mat4 *globalPose,
                              math::Mat4 *outPalette) noexcept;

} // namespace engine::runtime
