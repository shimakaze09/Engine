// Implements CPU skeletal pose evaluation: clip sampling over the rest
// pose (step, linear, and glTF cubic-Hermite keys), crossfade blending,
// hierarchy composition, and skinning-palette computation.

#include "engine/runtime/animation.h"

#include <cmath>
#include <cstddef>

#include "engine/math/transform.h"

namespace engine::runtime {

namespace {

/// Key interval for a clamped sample time: the index of the key at or
/// before the time, and the normalized fraction toward the next key.
struct KeyCursor final {
  std::size_t index = 0U;
  float fraction = 0.0F;
  bool single = true;
};

/// Locates the sample interval within a sorted key-time span.
KeyCursor locate_keys(const float *times, std::size_t count,
                      float time) noexcept {
  KeyCursor cursor{};
  if (count <= 1U) {
    return cursor;
  }
  cursor.single = false;

  if (time <= times[0]) {
    cursor.index = 0U;
    cursor.fraction = 0.0F;
    return cursor;
  }
  if (time >= times[count - 1U]) {
    cursor.index = count - 2U;
    cursor.fraction = 1.0F;
    return cursor;
  }

  std::size_t low = 0U;
  std::size_t high = count - 1U;
  while (high - low > 1U) {
    const std::size_t mid = (low + high) / 2U;
    if (times[mid] <= time) {
      low = mid;
    } else {
      high = mid;
    }
  }
  cursor.index = low;
  const float span = times[high] - times[low];
  cursor.fraction = (span > 0.0F) ? ((time - times[low]) / span) : 0.0F;
  return cursor;
}

/// glTF cubic-Hermite basis weights for fraction t over key span dt.
struct HermiteWeights final {
  float p0;
  float m0;
  float p1;
  float m1;
};

/// Computes the Hermite basis for a normalized fraction and key span.
HermiteWeights hermite_weights(float t, float dt) noexcept {
  const float t2 = t * t;
  const float t3 = t2 * t;
  return {(2.0F * t3) - (3.0F * t2) + 1.0F,
          dt * (t3 - (2.0F * t2) + t),
          (-2.0F * t3) + (3.0F * t2),
          dt * (t3 - t2)};
}

/// Samples a vec3 track at the cursor.
math::Vec3 sample_vec3(const AnimationClip &clip, const AnimTrackDesc &track,
                       const KeyCursor &cursor, float dt) noexcept {
  const float *values = clip.payload.data() + track.valuesOffset;
  auto value_at = [&](std::size_t key) noexcept {
    return math::Vec3(values[key * 3U + 0U], values[key * 3U + 1U],
                      values[key * 3U + 2U]);
  };

  if (cursor.single) {
    return value_at(0U);
  }
  if (track.interpolation == AnimInterp::Step) {
    return value_at(cursor.fraction >= 1.0F ? cursor.index + 1U
                                            : cursor.index);
  }
  if (track.interpolation == AnimInterp::CubicSpline) {
    const float *outTangents = clip.payload.data() + track.outTangentsOffset;
    const float *inTangents = clip.payload.data() + track.inTangentsOffset;
    const HermiteWeights w = hermite_weights(cursor.fraction, dt);
    const math::Vec3 p0 = value_at(cursor.index);
    const math::Vec3 p1 = value_at(cursor.index + 1U);
    const std::size_t k0 = cursor.index * 3U;
    const std::size_t k1 = (cursor.index + 1U) * 3U;
    const math::Vec3 m0(outTangents[k0], outTangents[k0 + 1U],
                        outTangents[k0 + 2U]);
    const math::Vec3 m1(inTangents[k1], inTangents[k1 + 1U],
                        inTangents[k1 + 2U]);
    return math::Vec3(
        (w.p0 * p0.x) + (w.m0 * m0.x) + (w.p1 * p1.x) + (w.m1 * m1.x),
        (w.p0 * p0.y) + (w.m0 * m0.y) + (w.p1 * p1.y) + (w.m1 * m1.y),
        (w.p0 * p0.z) + (w.m0 * m0.z) + (w.p1 * p1.z) + (w.m1 * m1.z));
  }

  const math::Vec3 a = value_at(cursor.index);
  const math::Vec3 b = value_at(cursor.index + 1U);
  return math::add(a, math::mul(math::sub(b, a), cursor.fraction));
}

/// Shortest-path normalized lerp between two rotations.
math::Quat nlerp_shortest(const math::Quat &a, math::Quat b,
                          float t) noexcept {
  const float dot =
      (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
  if (dot < 0.0F) {
    b = math::Quat{-b.x, -b.y, -b.z, -b.w};
  }
  const math::Quat mixed{a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t),
                         a.z + ((b.z - a.z) * t), a.w + ((b.w - a.w) * t)};
  return math::normalize(mixed);
}

/// Samples a rotation track at the cursor.
math::Quat sample_quat(const AnimationClip &clip, const AnimTrackDesc &track,
                       const KeyCursor &cursor, float dt) noexcept {
  const float *values = clip.payload.data() + track.valuesOffset;
  auto value_at = [&](std::size_t key) noexcept {
    return math::Quat{values[key * 4U + 0U], values[key * 4U + 1U],
                      values[key * 4U + 2U], values[key * 4U + 3U]};
  };

  if (cursor.single) {
    return math::normalize(value_at(0U));
  }
  if (track.interpolation == AnimInterp::Step) {
    return math::normalize(value_at(
        cursor.fraction >= 1.0F ? cursor.index + 1U : cursor.index));
  }
  if (track.interpolation == AnimInterp::CubicSpline) {
    const float *outTangents = clip.payload.data() + track.outTangentsOffset;
    const float *inTangents = clip.payload.data() + track.inTangentsOffset;
    const HermiteWeights w = hermite_weights(cursor.fraction, dt);
    const math::Quat p0 = value_at(cursor.index);
    const math::Quat p1 = value_at(cursor.index + 1U);
    const std::size_t k0 = cursor.index * 4U;
    const std::size_t k1 = (cursor.index + 1U) * 4U;
    const math::Quat result{
        (w.p0 * p0.x) + (w.m0 * outTangents[k0 + 0U]) + (w.p1 * p1.x) +
            (w.m1 * inTangents[k1 + 0U]),
        (w.p0 * p0.y) + (w.m0 * outTangents[k0 + 1U]) + (w.p1 * p1.y) +
            (w.m1 * inTangents[k1 + 1U]),
        (w.p0 * p0.z) + (w.m0 * outTangents[k0 + 2U]) + (w.p1 * p1.z) +
            (w.m1 * inTangents[k1 + 2U]),
        (w.p0 * p0.w) + (w.m0 * outTangents[k0 + 3U]) + (w.p1 * p1.w) +
            (w.m1 * inTangents[k1 + 3U])};
    return math::normalize(result);
  }

  return nlerp_shortest(value_at(cursor.index), value_at(cursor.index + 1U),
                        cursor.fraction);
}

} // namespace

bool anim_skeleton_parents_ordered(const AnimSkeleton &skeleton) noexcept {
  for (std::uint32_t i = 0U; i < skeleton.jointCount; ++i) {
    const std::uint32_t parent = skeleton.parents[i];
    if ((parent != kInvalidAnimJoint) && (parent >= i)) {
      return false;
    }
  }
  return true;
}

void sample_clip_pose(const AnimSkeleton &skeleton, const AnimationClip &clip,
                      float timeSeconds, JointPose *outPose) noexcept {
  if (outPose == nullptr) {
    return;
  }

  for (std::uint32_t i = 0U; i < skeleton.jointCount; ++i) {
    outPose[i] = skeleton.restPose[i];
  }

  float time = timeSeconds;
  if (!(time > 0.0F)) {
    time = 0.0F;
  }
  if (time > clip.durationSeconds) {
    time = clip.durationSeconds;
  }

  for (std::uint32_t t = 0U; t < clip.trackCount; ++t) {
    const AnimTrackDesc &track = clip.tracks[t];
    if ((track.joint >= skeleton.jointCount) || (track.keyCount == 0U)) {
      continue;
    }

    const float *times = clip.payload.data() + track.timesOffset;
    const KeyCursor cursor =
        locate_keys(times, static_cast<std::size_t>(track.keyCount), time);
    const float dt = cursor.single
                         ? 0.0F
                         : (times[cursor.index + 1U] - times[cursor.index]);

    JointPose &pose = outPose[track.joint];
    switch (track.target) {
    case AnimTarget::Translation:
      pose.translation = sample_vec3(clip, track, cursor, dt);
      break;
    case AnimTarget::Rotation:
      pose.rotation = sample_quat(clip, track, cursor, dt);
      break;
    case AnimTarget::Scale:
      pose.scale = sample_vec3(clip, track, cursor, dt);
      break;
    }
  }
}

void blend_poses(const JointPose *a, const JointPose *b, std::size_t count,
                 float weight, JointPose *out) noexcept {
  if ((a == nullptr) || (b == nullptr) || (out == nullptr)) {
    return;
  }
  float t = weight;
  if (!(t > 0.0F)) {
    t = 0.0F;
  }
  if (t > 1.0F) {
    t = 1.0F;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    out[i].translation = math::add(
        a[i].translation,
        math::mul(math::sub(b[i].translation, a[i].translation), t));
    out[i].scale = math::add(
        a[i].scale, math::mul(math::sub(b[i].scale, a[i].scale), t));
    out[i].rotation = nlerp_shortest(a[i].rotation, b[i].rotation, t);
  }
}

void compute_global_pose(const AnimSkeleton &skeleton,
                         const JointPose *localPose,
                         math::Mat4 *outGlobal) noexcept {
  if ((localPose == nullptr) || (outGlobal == nullptr)) {
    return;
  }

  for (std::uint32_t i = 0U; i < skeleton.jointCount; ++i) {
    const math::Mat4 local = math::compose_trs(
        localPose[i].translation, localPose[i].rotation, localPose[i].scale);
    const std::uint32_t parent = skeleton.parents[i];
    if ((parent == kInvalidAnimJoint) || (parent >= i)) {
      outGlobal[i] = local;
    } else {
      outGlobal[i] = math::mul(outGlobal[parent], local);
    }
  }
}

void compute_skinning_palette(const AnimSkeleton &skeleton,
                              const math::Mat4 *globalPose,
                              math::Mat4 *outPalette) noexcept {
  if ((globalPose == nullptr) || (outPalette == nullptr)) {
    return;
  }
  for (std::uint32_t i = 0U; i < skeleton.jointCount; ++i) {
    outPalette[i] = math::mul(globalPose[i], skeleton.inverseBind[i]);
  }
}

} // namespace engine::runtime
