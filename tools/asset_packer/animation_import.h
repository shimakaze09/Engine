// Declares glTF animation clip import data for the Engine asset packer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "skeleton_import.h"

struct cgltf_data;

namespace engine::tools {

inline constexpr std::size_t kMaxAnimationTracks = kMaxSkeletonJoints * 3U;

/// Enumerates transform channels supported by skeletal animation clips.
enum class AnimTrackTarget : std::uint8_t {
  Translation,
  Rotation,
  Scale,
};

/// Enumerates interpolation modes supported by imported animation tracks.
enum class AnimInterpolation : std::uint8_t {
  Linear,
  Step,
  CubicSpline,
};

/// Stores one joint transform animation track decoded from glTF accessors.
struct AnimTrack final {
  std::uint32_t joint = kInvalidSkeletonJoint;
  AnimTrackTarget target = AnimTrackTarget::Translation;
  AnimInterpolation interpolation = AnimInterpolation::Linear;
  std::vector<float> times{};
  std::vector<math::Vec3> vec3Values{};
  std::vector<math::Quat> quatValues{};
  std::vector<math::Vec3> inVec3Tangents{};
  std::vector<math::Vec3> outVec3Tangents{};
  std::vector<math::Quat> inQuatTangents{};
  std::vector<math::Quat> outQuatTangents{};
};

/// Stores a glTF animation as deterministic engine-facing clip data.
struct AnimClip final {
  std::string name{};
  float durationSeconds = 0.0F;
  std::vector<AnimTrack> tracks{};
};

/// Enumerates animation import outcomes for explicit tool error paths.
enum class AnimationImportResult : std::uint8_t {
  Ok,
  NullInput,
  AnimationIndexOutOfRange,
  SkinIndexOutOfRange,
  EmptyAnimation,
  TooManyTracks,
  InvalidChannel,
  UnsupportedTarget,
  TargetNotInSkin,
  InvalidInputAccessor,
  InvalidOutputAccessor,
  DecodeFailed,
};

/// Returns a stable message for an animation import result.
const char *animation_import_result_message(
    AnimationImportResult result) noexcept;

/// Parses a glTF animation into engine-facing joint transform tracks.
bool parse_gltf_animation(const cgltf_data *data, std::size_t animationIndex,
                          std::size_t skinIndex, AnimClip *outClip,
                          AnimationImportResult *outResult = nullptr);

} // namespace engine::tools
