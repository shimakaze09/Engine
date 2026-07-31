// Declares the cooked skeletal animation binary formats shared by the
// asset packer (writer) and the runtime (loader): .skel skeletons in
// parent-before-child joint order and .anim sampled clips with one float
// payload. All records are 4-byte members, so the structs are written and
// read directly with no padding.

#pragma once

#include <cstdint>

namespace engine::core {

inline constexpr std::uint32_t kSkeletonAssetMagic = 0x534B454CU;
inline constexpr std::uint32_t kSkeletonAssetVersion = 1U;

/// .skel header; followed by jointCount SkeletonAssetJoint records.
struct SkeletonAssetHeader final {
  std::uint32_t magic = kSkeletonAssetMagic;
  std::uint32_t version = kSkeletonAssetVersion;
  std::uint32_t jointCount = 0U;
  std::uint32_t reserved = 0U;
};

/// One cooked joint. Parent indexes the cooked order (parent-before-child;
/// 0xFFFFFFFF marks a root); nameHash is FNV-1a 32 of the joint name.
struct SkeletonAssetJoint final {
  std::uint32_t parent = 0xFFFFFFFFU;
  std::uint32_t nameHash = 0U;
  float inverseBind[16] = {};
  float restTranslation[3] = {};
  float restRotation[4] = {};
  float restScale[3] = {};
};

inline constexpr std::uint32_t kAnimClipAssetMagic = 0x414E494DU;
inline constexpr std::uint32_t kAnimClipAssetVersion = 1U;

/// .anim header; followed by trackCount AnimClipAssetTrack records, then
/// payloadFloatCount floats holding every track's times/values/tangents.
struct AnimClipAssetHeader final {
  std::uint32_t magic = kAnimClipAssetMagic;
  std::uint32_t version = kAnimClipAssetVersion;
  std::uint32_t trackCount = 0U;
  std::uint32_t payloadFloatCount = 0U;
  float durationSeconds = 0.0F;
  std::uint32_t reserved[3] = {};
};

/// One cooked track. target: 0 translation, 1 rotation, 2 scale.
/// interpolation: 0 linear, 1 step, 2 cubic spline (Hermite tangents).
/// Offsets are float indices into the payload; tangent offsets are zero
/// for non-cubic tracks.
struct AnimClipAssetTrack final {
  std::uint32_t joint = 0U;
  std::uint32_t target = 0U;
  std::uint32_t interpolation = 0U;
  std::uint32_t keyCount = 0U;
  std::uint32_t timesOffset = 0U;
  std::uint32_t valuesOffset = 0U;
  std::uint32_t inTangentsOffset = 0U;
  std::uint32_t outTangentsOffset = 0U;
};

} // namespace engine::core
