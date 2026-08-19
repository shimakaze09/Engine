// Implements the runtime loaders for cooked skeletal animation assets:
// .skel skeletons and .anim clips read through the VFS and validated
// against the shared binary formats before evaluation ever touches them;
// each load also routes through the shared cooked-asset staleness check
// (issue #91) and the cook-generation gate (audit #211) via its owning
// mesh's sidecars.

#include "engine/runtime/animation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "engine/core/animation_asset.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/content/asset_staleness.h"

namespace engine::runtime {

namespace {

// Independent sanity ceiling on top of the file-size-derived bound below:
// a payloadFloatCount within an actual file's size still cannot be trusted
// to be reasonable (a large-but-real file could legitimately claim a size
// that is expensive but unallocatable on a constrained device). 8M floats
// (32 MiB) comfortably covers any authored clip while giving hostile or
// corrupt headers a small-file rejection path that never reaches
// allocation (audit #174).
constexpr std::uint32_t kMaxAnimPayloadFloats = 8U * 1024U * 1024U;

/// Logs one load failure with its virtual path.
bool fail(const char *virtualPath, const char *reason) noexcept {
  char message[192] = {};
  std::snprintf(message, sizeof(message), "%s: %s",
                (virtualPath != nullptr) ? virtualPath : "(null)", reason);
  core::log_message(core::LogLevel::Error, "animation", message);
  return false;
}

/// True when the last `suffixLen` bytes of `text` equal `suffix`.
bool ends_with(const char *text, std::size_t textLen, const char *suffix,
              std::size_t suffixLen) noexcept {
  return (textLen >= suffixLen) &&
         (std::memcmp(text + (textLen - suffixLen), suffix, suffixLen) == 0);
}

/// Derives the cooked mesh path that owns a .skel/.anim sidecar so the
/// staleness check below can reuse the mesh's .meta.json (skeletons and
/// clips are cooked from the same source glTF in the same packer run and
/// carry no sidecar of their own). Mirrors the packer's orphan-sweep
/// convention in cook_stamp.cpp: a skeleton keeps the mesh's stem, a
/// clip's stem carries an extra ".<clipName>" segment. Returns false for
/// paths that don't end in .skel/.anim (builtin/procedural callers stay
/// silent).
bool owning_mesh_virtual_path(const char *virtualPath, char *outPath,
                              std::size_t outSize) noexcept {
  if ((virtualPath == nullptr) || (outPath == nullptr) || (outSize == 0U)) {
    return false;
  }

  constexpr char kSkelSuffix[] = ".skel";
  constexpr char kAnimSuffix[] = ".anim";
  constexpr std::size_t kSkelSuffixLen = sizeof(kSkelSuffix) - 1U;
  constexpr std::size_t kAnimSuffixLen = sizeof(kAnimSuffix) - 1U;
  const std::size_t pathLen = std::strlen(virtualPath);

  std::size_t stemLen = 0U;
  if (ends_with(virtualPath, pathLen, kSkelSuffix, kSkelSuffixLen)) {
    stemLen = pathLen - kSkelSuffixLen;
  } else if (ends_with(virtualPath, pathLen, kAnimSuffix, kAnimSuffixLen)) {
    const std::size_t clipStemLen = pathLen - kAnimSuffixLen;
    std::size_t clipDot = clipStemLen;
    while ((clipDot > 0U) && (virtualPath[clipDot - 1U] != '.')) {
      --clipDot;
    }
    if (clipDot == 0U) {
      return false;
    }
    stemLen = clipDot - 1U;
  } else {
    return false;
  }

  if (stemLen == 0U) {
    return false;
  }

  const int written = std::snprintf(outPath, outSize, "%.*s.mesh",
                                    static_cast<int>(stemLen), virtualPath);
  return (written > 0) && (static_cast<std::size_t>(written) < outSize);
}

/// Routes a cooked .skel/.anim load through the shared once-per-asset
/// staleness check (issue #91) and the cook-generation gate (#211) by
/// resolving and reusing the owning mesh's sidecars — skeletons and clips
/// are outputs of the mesh's cook, so its stamp certifies them. Returns
/// false when that generation is torn or mixed; stays silent and accepts
/// when the virtual prefix isn't mounted or no owning mesh path can be
/// derived, matching the mesh-only check's existing silent boundary for
/// sidecar-less assets.
bool owning_mesh_generation_ok(const char *virtualPath) noexcept {
  char meshVirtualPath[192] = {};
  if (!owning_mesh_virtual_path(virtualPath, meshVirtualPath,
                                sizeof(meshVirtualPath))) {
    return true;
  }

  char osPath[512] = {};
  if (!core::vfs_resolve_os_path(meshVirtualPath, osPath, sizeof(osPath))) {
    return true;
  }

  if (!content::cooked_asset_generation_ok(osPath)) {
    return false;
  }
  content::warn_if_cooked_asset_stale(osPath);
  return true;
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

  if (!owning_mesh_generation_ok(virtualPath)) {
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

  if (!owning_mesh_generation_ok(virtualPath)) {
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
    // 64-bit arithmetic so hostile counts cannot wrap a 32-bit size_t
    // into a passing size check and then a multi-gigabyte allocation.
    const std::uint64_t expected =
        static_cast<std::uint64_t>(sizeof(header)) +
        (static_cast<std::uint64_t>(header.trackCount) *
         sizeof(core::AnimClipAssetTrack)) +
        (static_cast<std::uint64_t>(header.payloadFloatCount) * sizeof(float));
    if ((header.magic == core::kAnimClipAssetMagic) &&
        (header.version == core::kAnimClipAssetVersion) &&
        (header.trackCount <= kMaxAnimTracks) &&
        (header.payloadFloatCount <= kMaxAnimPayloadFloats) &&
        (static_cast<std::uint64_t>(size) >= expected) &&
        std::isfinite(header.durationSeconds) &&
        (header.durationSeconds >= 0.0F)) {
      AnimationClip clip{};
      clip.durationSeconds = header.durationSeconds;
      clip.trackCount = header.trackCount;
      const auto *base = static_cast<const std::uint8_t *>(data);
      const auto *records = reinterpret_cast<const core::AnimClipAssetTrack *>(
          base + sizeof(header));
      // Nothrow allocation: an unallocatable (but header/size-validated)
      // payload is a recoverable load failure, never process termination
      // under the no-exception build (audit #174).
      if (!clip.payload.allocate(header.payloadFloatCount)) {
        static_cast<void>(fail(virtualPath, "clip payload allocation failed"));
      } else {
        if (header.payloadFloatCount > 0U) {
          std::memcpy(clip.payload.data(),
                      base + sizeof(header) +
                          (static_cast<std::size_t>(header.trackCount) *
                           sizeof(core::AnimClipAssetTrack)),
                      static_cast<std::size_t>(header.payloadFloatCount) *
                          sizeof(float));
        }

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
          // Move, not copy: payload is a move-only nothrow buffer, so
          // publishing the decoded clip never allocates a second time
          // (audit #174).
          *outClip = std::move(clip);
          ok = true;
        } else {
          static_cast<void>(fail(virtualPath, "clip track out of bounds"));
        }
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
