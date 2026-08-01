// Implements skeletal animation cooking: stable parent-before-child
// skeleton reordering and deterministic .skel/.anim writers over the
// shared binary formats in engine/core/animation_asset.h.

#include "anim_cook.h"

#include <cstdio>
#include <cstring>

#include "engine/core/animation_asset.h"
#include "engine/core/atomic_file.h"
#include "engine/core/hash.h"

namespace engine::tools {

std::string sanitize_clip_name(const std::string &name, std::size_t index) {
  std::string cleaned{};
  cleaned.reserve(name.size());
  for (const char c : name) {
    const bool keep = ((c >= 'a') && (c <= 'z')) ||
                      ((c >= 'A') && (c <= 'Z')) ||
                      ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-');
    cleaned.push_back(keep ? c : '_');
  }
  if (cleaned.empty()) {
    cleaned = "clip" + std::to_string(index);
  }
  return cleaned;
}

bool derive_unique_clip_name(const std::string &clipName, std::size_t index,
                             std::unordered_set<std::string> *usedNames,
                             std::string *outName) {
  if ((usedNames == nullptr) || (outName == nullptr)) {
    return false;
  }
  *outName = sanitize_clip_name(clipName, index);

  // Collision detection folds case: "Walk" and "walk" are one file on
  // Windows and default macOS filesystems, so the second would silently
  // overwrite the first there. The cooked file keeps the original
  // sanitized spelling.
  std::string collisionKey = *outName;
  for (char &c : collisionKey) {
    if ((c >= 'A') && (c <= 'Z')) {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return usedNames->insert(collisionKey).second;
}

namespace {

/// Appends one track's keys to the payload and fills its cooked record.
void pack_track(const AnimTrack &track, std::uint32_t cookedJoint,
                core::AnimClipAssetTrack *outRecord,
                std::vector<float> *payload) {
  outRecord->joint = cookedJoint;
  outRecord->target = static_cast<std::uint32_t>(track.target);
  outRecord->interpolation = static_cast<std::uint32_t>(track.interpolation);
  outRecord->keyCount = static_cast<std::uint32_t>(track.times.size());

  outRecord->timesOffset = static_cast<std::uint32_t>(payload->size());
  payload->insert(payload->end(), track.times.begin(), track.times.end());

  const bool isRotation = track.target == AnimTrackTarget::Rotation;
  outRecord->valuesOffset = static_cast<std::uint32_t>(payload->size());
  if (isRotation) {
    for (const math::Quat &q : track.quatValues) {
      payload->push_back(q.x);
      payload->push_back(q.y);
      payload->push_back(q.z);
      payload->push_back(q.w);
    }
  } else {
    for (const math::Vec3 &v : track.vec3Values) {
      payload->push_back(v.x);
      payload->push_back(v.y);
      payload->push_back(v.z);
    }
  }

  outRecord->inTangentsOffset = 0U;
  outRecord->outTangentsOffset = 0U;
  if (track.interpolation == AnimInterpolation::CubicSpline) {
    outRecord->inTangentsOffset = static_cast<std::uint32_t>(payload->size());
    if (isRotation) {
      for (const math::Quat &q : track.inQuatTangents) {
        payload->push_back(q.x);
        payload->push_back(q.y);
        payload->push_back(q.z);
        payload->push_back(q.w);
      }
    } else {
      for (const math::Vec3 &v : track.inVec3Tangents) {
        payload->push_back(v.x);
        payload->push_back(v.y);
        payload->push_back(v.z);
      }
    }
    outRecord->outTangentsOffset = static_cast<std::uint32_t>(payload->size());
    if (isRotation) {
      for (const math::Quat &q : track.outQuatTangents) {
        payload->push_back(q.x);
        payload->push_back(q.y);
        payload->push_back(q.z);
        payload->push_back(q.w);
      }
    } else {
      for (const math::Vec3 &v : track.outVec3Tangents) {
        payload->push_back(v.x);
        payload->push_back(v.y);
        payload->push_back(v.z);
      }
    }
  }
}

} // namespace

bool reorder_skeleton_parent_first(Skeleton *skeleton,
                                   std::vector<std::uint32_t> *outRemap) {
  if ((skeleton == nullptr) || (outRemap == nullptr)) {
    return false;
  }

  const std::size_t count = skeleton->joints.size();
  outRemap->assign(count, kInvalidSkeletonJoint);

  std::vector<SkeletonJoint> cooked{};
  cooked.reserve(count);
  std::vector<bool> placed(count, false);

  while (cooked.size() < count) {
    bool progressed = false;
    for (std::size_t i = 0U; i < count; ++i) {
      if (placed[i]) {
        continue;
      }
      const std::uint32_t parent = skeleton->joints[i].parent;
      const bool parentReady =
          (parent == kInvalidSkeletonJoint) ||
          ((parent < count) && placed[parent]);
      if (!parentReady) {
        continue;
      }
      (*outRemap)[i] = static_cast<std::uint32_t>(cooked.size());
      cooked.push_back(skeleton->joints[i]);
      placed[i] = true;
      progressed = true;
    }
    if (!progressed) {
      return false;
    }
  }

  for (SkeletonJoint &joint : cooked) {
    if (joint.parent != kInvalidSkeletonJoint) {
      joint.parent = (*outRemap)[joint.parent];
    }
  }
  if (skeleton->rootJoint != kInvalidSkeletonJoint) {
    skeleton->rootJoint = (*outRemap)[skeleton->rootJoint];
  }
  skeleton->joints = cooked;
  return true;
}

bool write_skeleton_asset(const char *outputPath, const Skeleton &skeleton) {
  if ((outputPath == nullptr) ||
      (skeleton.joints.size() > kMaxSkeletonJoints)) {
    return false;
  }

  core::SkeletonAssetHeader header{};
  header.jointCount = static_cast<std::uint32_t>(skeleton.joints.size());

  // Assemble in memory and commit atomically so an interrupted cook can
  // never leave a truncated .skel behind (audit H-20).
  std::vector<unsigned char> buffer(
      sizeof(header) +
      (skeleton.joints.size() * sizeof(core::SkeletonAssetJoint)));
  std::memcpy(buffer.data(), &header, sizeof(header));

  for (std::size_t i = 0U; i < skeleton.joints.size(); ++i) {
    const SkeletonJoint &joint = skeleton.joints[i];
    core::SkeletonAssetJoint record{};
    record.parent = joint.parent;
    record.nameHash = core::fnv1a_32(joint.name.c_str());
    std::memcpy(record.inverseBind, joint.inverseBindMatrix.data(),
                sizeof(record.inverseBind));
    record.restTranslation[0] = joint.restTranslation.x;
    record.restTranslation[1] = joint.restTranslation.y;
    record.restTranslation[2] = joint.restTranslation.z;
    record.restRotation[0] = joint.restRotation.x;
    record.restRotation[1] = joint.restRotation.y;
    record.restRotation[2] = joint.restRotation.z;
    record.restRotation[3] = joint.restRotation.w;
    record.restScale[0] = joint.restScale.x;
    record.restScale[1] = joint.restScale.y;
    record.restScale[2] = joint.restScale.z;
    std::memcpy(buffer.data() + sizeof(header) +
                    (i * sizeof(core::SkeletonAssetJoint)),
                &record, sizeof(record));
  }

  return core::atomic_write_file(outputPath, buffer.data(), buffer.size());
}

bool write_anim_clip_asset(const char *outputPath, const AnimClip &clip,
                           const std::vector<std::uint32_t> &jointRemap) {
  if ((outputPath == nullptr) || (clip.tracks.size() > kMaxAnimationTracks)) {
    return false;
  }

  std::vector<core::AnimClipAssetTrack> records{};
  records.reserve(clip.tracks.size());
  std::vector<float> payload{};
  for (const AnimTrack &track : clip.tracks) {
    if (track.joint >= jointRemap.size()) {
      return false;
    }
    core::AnimClipAssetTrack record{};
    pack_track(track, jointRemap[track.joint], &record, &payload);
    records.push_back(record);
  }

  core::AnimClipAssetHeader header{};
  header.trackCount = static_cast<std::uint32_t>(records.size());
  header.payloadFloatCount = static_cast<std::uint32_t>(payload.size());
  header.durationSeconds = clip.durationSeconds;

  // Assemble in memory and commit atomically so an interrupted cook can
  // never leave a truncated .anim behind (audit H-20).
  const std::size_t recordBytes =
      records.size() * sizeof(core::AnimClipAssetTrack);
  const std::size_t payloadBytes = payload.size() * sizeof(float);
  std::vector<unsigned char> buffer(sizeof(header) + recordBytes +
                                    payloadBytes);
  std::memcpy(buffer.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(buffer.data() + sizeof(header), records.data(), recordBytes);
  }
  if (!payload.empty()) {
    std::memcpy(buffer.data() + sizeof(header) + recordBytes, payload.data(),
                payloadBytes);
  }

  return core::atomic_write_file(outputPath, buffer.data(), buffer.size());
}

} // namespace engine::tools
