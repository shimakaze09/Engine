// Implements the runtime loaders for cooked skeletal animation assets:
// .skel skeletons and .anim clips read through the VFS and validated
// against the shared binary formats before evaluation ever touches them.

#include "engine/runtime/animation.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "engine/core/animation_asset.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"

namespace engine::runtime {

namespace {

/// Logs one load failure with its virtual path.
bool fail(const char *virtualPath, const char *reason) noexcept {
  char message[192] = {};
  std::snprintf(message, sizeof(message), "%s: %s",
                (virtualPath != nullptr) ? virtualPath : "(null)", reason);
  core::log_message(core::LogLevel::Error, "animation", message);
  return false;
}

/// True when a track's key span [offset, offset + count*stride) fits the
/// payload.
bool span_fits(std::uint32_t offset, std::uint32_t keyCount,
               std::uint32_t stride, std::uint32_t payloadCount) noexcept {
  const std::uint64_t end =
      static_cast<std::uint64_t>(offset) +
      (static_cast<std::uint64_t>(keyCount) * static_cast<std::uint64_t>(stride));
  return end <= payloadCount;
}

} // namespace

bool load_skeleton_asset(const char *virtualPath,
                         AnimSkeleton *outSkeleton) noexcept {
  if ((virtualPath == nullptr) || (outSkeleton == nullptr)) {
    return false;
  }

  void *data = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_binary(virtualPath, &data, &size)) {
    return fail(virtualPath, "skeleton read failed");
  }

  bool ok = false;
  core::SkeletonAssetHeader header{};
  if (size >= sizeof(header)) {
    std::memcpy(&header, data, sizeof(header));
    const std::size_t expected =
        sizeof(header) +
        (static_cast<std::size_t>(header.jointCount) *
         sizeof(core::SkeletonAssetJoint));
    if ((header.magic == core::kSkeletonAssetMagic) &&
        (header.version == core::kSkeletonAssetVersion) &&
        (header.jointCount <= kMaxAnimJoints) && (size >= expected)) {
      AnimSkeleton skeleton{};
      skeleton.jointCount = header.jointCount;
      const auto *records = reinterpret_cast<const core::SkeletonAssetJoint *>(
          static_cast<const std::uint8_t *>(data) + sizeof(header));
      for (std::uint32_t i = 0U; i < header.jointCount; ++i) {
        const core::SkeletonAssetJoint &record = records[i];
        skeleton.parents[i] = record.parent;
        skeleton.nameHashes[i] = record.nameHash;
        std::memcpy(&skeleton.inverseBind[i], record.inverseBind,
                    sizeof(record.inverseBind));
        skeleton.restPose[i].translation =
            math::Vec3(record.restTranslation[0], record.restTranslation[1],
                       record.restTranslation[2]);
        skeleton.restPose[i].rotation =
            math::Quat(record.restRotation[0], record.restRotation[1],
                       record.restRotation[2], record.restRotation[3]);
        skeleton.restPose[i].scale = math::Vec3(
            record.restScale[0], record.restScale[1], record.restScale[2]);
      }
      if (anim_skeleton_parents_ordered(skeleton)) {
        *outSkeleton = skeleton;
        ok = true;
      } else {
        static_cast<void>(fail(virtualPath, "skeleton joints out of order"));
      }
    } else {
      static_cast<void>(fail(virtualPath, "invalid skeleton header"));
    }
  } else {
    static_cast<void>(fail(virtualPath, "skeleton file truncated"));
  }

  core::vfs_free(data);
  return ok;
}

bool load_animation_clip_asset(const char *virtualPath,
                               AnimationClip *outClip) noexcept {
  if ((virtualPath == nullptr) || (outClip == nullptr)) {
    return false;
  }

  void *data = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_binary(virtualPath, &data, &size)) {
    return fail(virtualPath, "clip read failed");
  }

  bool ok = false;
  core::AnimClipAssetHeader header{};
  if (size >= sizeof(header)) {
    std::memcpy(&header, data, sizeof(header));
    const std::size_t expected =
        sizeof(header) +
        (static_cast<std::size_t>(header.trackCount) *
         sizeof(core::AnimClipAssetTrack)) +
        (static_cast<std::size_t>(header.payloadFloatCount) * sizeof(float));
    if ((header.magic == core::kAnimClipAssetMagic) &&
        (header.version == core::kAnimClipAssetVersion) &&
        (header.trackCount <= kMaxAnimTracks) && (size >= expected)) {
      AnimationClip clip{};
      clip.durationSeconds = header.durationSeconds;
      clip.trackCount = header.trackCount;
      const auto *base = static_cast<const std::uint8_t *>(data);
      const auto *records = reinterpret_cast<const core::AnimClipAssetTrack *>(
          base + sizeof(header));
      const auto *payload = reinterpret_cast<const float *>(
          base + sizeof(header) +
          (static_cast<std::size_t>(header.trackCount) *
           sizeof(core::AnimClipAssetTrack)));
      clip.payload.assign(payload, payload + header.payloadFloatCount);

      bool tracksValid = true;
      for (std::uint32_t t = 0U; tracksValid && (t < header.trackCount);
           ++t) {
        const core::AnimClipAssetTrack &record = records[t];
        AnimTrackDesc &track = clip.tracks[t];
        track.joint = record.joint;
        track.target = static_cast<AnimTarget>(record.target);
        track.interpolation = static_cast<AnimInterp>(record.interpolation);
        track.keyCount = record.keyCount;
        track.timesOffset = record.timesOffset;
        track.valuesOffset = record.valuesOffset;
        track.inTangentsOffset = record.inTangentsOffset;
        track.outTangentsOffset = record.outTangentsOffset;

        const std::uint32_t stride = (record.target == 1U) ? 4U : 3U;
        tracksValid =
            (record.target <= 2U) && (record.interpolation <= 2U) &&
            span_fits(record.timesOffset, record.keyCount, 1U,
                      header.payloadFloatCount) &&
            span_fits(record.valuesOffset, record.keyCount, stride,
                      header.payloadFloatCount);
        if (tracksValid && (record.interpolation == 2U)) {
          tracksValid = span_fits(record.inTangentsOffset, record.keyCount,
                                  stride, header.payloadFloatCount) &&
                        span_fits(record.outTangentsOffset, record.keyCount,
                                  stride, header.payloadFloatCount);
        }
      }

      if (tracksValid) {
        *outClip = clip;
        ok = true;
      } else {
        static_cast<void>(fail(virtualPath, "clip track out of bounds"));
      }
    } else {
      static_cast<void>(fail(virtualPath, "invalid clip header"));
    }
  } else {
    static_cast<void>(fail(virtualPath, "clip file truncated"));
  }

  core::vfs_free(data);
  return ok;
}

} // namespace engine::runtime
