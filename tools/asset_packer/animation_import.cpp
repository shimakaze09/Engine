// Implements glTF animation clip parsing for the Engine asset packer.

#include "animation_import.h"

#include <array>
#include <cstdio>

#include <cgltf.h>

namespace engine::tools {
namespace {

/// Writes an optional parser result value.
void set_result(AnimationImportResult *outResult,
                AnimationImportResult result) noexcept {
  if (outResult != nullptr) {
    *outResult = result;
  }
}

/// Returns the local skeleton joint index for a glTF node pointer.
std::uint32_t find_joint_index(const cgltf_skin &skin,
                               const cgltf_node *node) noexcept {
  if (node == nullptr) {
    return kInvalidSkeletonJoint;
  }

  for (cgltf_size i = 0U; i < skin.joints_count; ++i) {
    if (skin.joints[i] == node) {
      return static_cast<std::uint32_t>(i);
    }
  }

  return kInvalidSkeletonJoint;
}

/// Converts a glTF interpolation value into an engine value.
AnimInterpolation convert_interpolation(
    cgltf_interpolation_type interpolation) noexcept {
  switch (interpolation) {
  case cgltf_interpolation_type_step:
    return AnimInterpolation::Step;
  case cgltf_interpolation_type_cubic_spline:
    return AnimInterpolation::CubicSpline;
  case cgltf_interpolation_type_linear:
  case cgltf_interpolation_type_max_enum:
    break;
  }

  return AnimInterpolation::Linear;
}

/// Converts a glTF channel target value into an engine value.
bool convert_target(cgltf_animation_path_type path,
                    AnimTrackTarget *outTarget) noexcept {
  if (outTarget == nullptr) {
    return false;
  }

  switch (path) {
  case cgltf_animation_path_type_translation:
    *outTarget = AnimTrackTarget::Translation;
    return true;
  case cgltf_animation_path_type_rotation:
    *outTarget = AnimTrackTarget::Rotation;
    return true;
  case cgltf_animation_path_type_scale:
    *outTarget = AnimTrackTarget::Scale;
    return true;
  case cgltf_animation_path_type_weights:
  case cgltf_animation_path_type_invalid:
  case cgltf_animation_path_type_max_enum:
    break;
  }

  return false;
}

/// Returns the expected glTF output accessor type for a track target.
cgltf_type output_type_for_target(AnimTrackTarget target) noexcept {
  return target == AnimTrackTarget::Rotation ? cgltf_type_vec4
                                             : cgltf_type_vec3;
}

/// Appends one decoded vec3 sample from an accessor.
bool append_vec3_sample(const cgltf_accessor *accessor, cgltf_size sampleIndex,
                        std::vector<math::Vec3> *outValues) {
  if ((accessor == nullptr) || (outValues == nullptr)) {
    return false;
  }

  std::array<float, 3U> values{};
  if (!cgltf_accessor_read_float(accessor, sampleIndex, values.data(),
                                 values.size())) {
    return false;
  }

  outValues->push_back(math::Vec3(values[0U], values[1U], values[2U]));
  return true;
}

/// Appends one decoded quat sample from an accessor.
bool append_quat_sample(const cgltf_accessor *accessor, cgltf_size sampleIndex,
                        std::vector<math::Quat> *outValues) {
  if ((accessor == nullptr) || (outValues == nullptr)) {
    return false;
  }

  std::array<float, 4U> values{};
  if (!cgltf_accessor_read_float(accessor, sampleIndex, values.data(),
                                 values.size())) {
    return false;
  }

  outValues->push_back(
      math::Quat(values[0U], values[1U], values[2U], values[3U]));
  return true;
}

/// Decodes scalar key times from the glTF input accessor.
bool decode_track_times(const cgltf_accessor *input,
                        std::vector<float> *outTimes, float *outDuration) {
  if ((input == nullptr) || (outTimes == nullptr) || (outDuration == nullptr)) {
    return false;
  }

  outTimes->clear();
  outTimes->reserve(static_cast<std::size_t>(input->count));

  float maxTime = 0.0F;
  for (cgltf_size i = 0U; i < input->count; ++i) {
    float time = 0.0F;
    if (!cgltf_accessor_read_float(input, i, &time, 1U)) {
      return false;
    }

    outTimes->push_back(time);
    if (time > maxTime) {
      maxTime = time;
    }
  }

  *outDuration = maxTime;
  return true;
}

/// Decodes transform values and optional cubic tangents from a glTF output accessor.
bool decode_track_values(const cgltf_accessor *output,
                         AnimTrackTarget target,
                         AnimInterpolation interpolation,
                         std::size_t keyCount, AnimTrack *outTrack) {
  if ((output == nullptr) || (outTrack == nullptr)) {
    return false;
  }

  const bool cubic = interpolation == AnimInterpolation::CubicSpline;
  const std::size_t sampleStride = cubic ? 3U : 1U;

  if (target == AnimTrackTarget::Rotation) {
    outTrack->quatValues.reserve(keyCount);
    if (cubic) {
      outTrack->inQuatTangents.reserve(keyCount);
      outTrack->outQuatTangents.reserve(keyCount);
    }
  } else {
    outTrack->vec3Values.reserve(keyCount);
    if (cubic) {
      outTrack->inVec3Tangents.reserve(keyCount);
      outTrack->outVec3Tangents.reserve(keyCount);
    }
  }

  for (std::size_t key = 0U; key < keyCount; ++key) {
    const cgltf_size sample =
        static_cast<cgltf_size>(key * sampleStride);
    if (target == AnimTrackTarget::Rotation) {
      if (cubic &&
          !append_quat_sample(output, sample, &outTrack->inQuatTangents)) {
        return false;
      }
      if (!append_quat_sample(output, sample + (cubic ? 1U : 0U),
                              &outTrack->quatValues)) {
        return false;
      }
      if (cubic &&
          !append_quat_sample(output, sample + 2U,
                              &outTrack->outQuatTangents)) {
        return false;
      }
    } else {
      if (cubic &&
          !append_vec3_sample(output, sample, &outTrack->inVec3Tangents)) {
        return false;
      }
      if (!append_vec3_sample(output, sample + (cubic ? 1U : 0U),
                              &outTrack->vec3Values)) {
        return false;
      }
      if (cubic &&
          !append_vec3_sample(output, sample + 2U,
                              &outTrack->outVec3Tangents)) {
        return false;
      }
    }
  }

  return true;
}

} // namespace

/// Returns a stable message for an animation import result.
const char *animation_import_result_message(
    AnimationImportResult result) noexcept {
  switch (result) {
  case AnimationImportResult::Ok:
    return "ok";
  case AnimationImportResult::NullInput:
    return "null input";
  case AnimationImportResult::AnimationIndexOutOfRange:
    return "animation index out of range";
  case AnimationImportResult::SkinIndexOutOfRange:
    return "skin index out of range";
  case AnimationImportResult::EmptyAnimation:
    return "animation has no channels";
  case AnimationImportResult::TooManyTracks:
    return "animation exceeds supported track count";
  case AnimationImportResult::InvalidChannel:
    return "animation channel is invalid";
  case AnimationImportResult::UnsupportedTarget:
    return "animation channel target is unsupported";
  case AnimationImportResult::TargetNotInSkin:
    return "animation channel target is not a skin joint";
  case AnimationImportResult::InvalidInputAccessor:
    return "animation input accessor is invalid";
  case AnimationImportResult::InvalidOutputAccessor:
    return "animation output accessor is invalid";
  case AnimationImportResult::DecodeFailed:
    return "failed to decode animation accessor";
  }

  return "unknown animation import result";
}

/// Parses a glTF animation into engine-facing joint transform tracks.
bool parse_gltf_animation(const cgltf_data *data, std::size_t animationIndex,
                          std::size_t skinIndex, AnimClip *outClip,
                          AnimationImportResult *outResult) {
  if ((data == nullptr) || (outClip == nullptr)) {
    set_result(outResult, AnimationImportResult::NullInput);
    return false;
  }

  if (animationIndex >= static_cast<std::size_t>(data->animations_count)) {
    set_result(outResult, AnimationImportResult::AnimationIndexOutOfRange);
    return false;
  }

  if (skinIndex >= static_cast<std::size_t>(data->skins_count)) {
    set_result(outResult, AnimationImportResult::SkinIndexOutOfRange);
    return false;
  }

  const cgltf_animation &animation = data->animations[animationIndex];
  const cgltf_skin &skin = data->skins[skinIndex];
  if ((animation.channels == nullptr) || (animation.channels_count == 0U)) {
    set_result(outResult, AnimationImportResult::EmptyAnimation);
    return false;
  }

  if (animation.channels_count > kMaxAnimationTracks) {
    set_result(outResult, AnimationImportResult::TooManyTracks);
    return false;
  }

  AnimClip parsed{};
  if (animation.name != nullptr) {
    parsed.name = animation.name;
  } else {
    char fallbackName[32] = {};
    std::snprintf(fallbackName, sizeof(fallbackName), "animation_%u",
                  static_cast<unsigned>(animationIndex));
    parsed.name = fallbackName;
  }
  parsed.tracks.reserve(static_cast<std::size_t>(animation.channels_count));

  for (cgltf_size i = 0U; i < animation.channels_count; ++i) {
    const cgltf_animation_channel &channel = animation.channels[i];
    if ((channel.sampler == nullptr) || (channel.target_node == nullptr)) {
      set_result(outResult, AnimationImportResult::InvalidChannel);
      return false;
    }

    AnimTrackTarget target = AnimTrackTarget::Translation;
    if (!convert_target(channel.target_path, &target)) {
      set_result(outResult, AnimationImportResult::UnsupportedTarget);
      return false;
    }

    const std::uint32_t joint = find_joint_index(skin, channel.target_node);
    if (joint == kInvalidSkeletonJoint) {
      set_result(outResult, AnimationImportResult::TargetNotInSkin);
      return false;
    }

    const cgltf_animation_sampler &sampler = *channel.sampler;
    const cgltf_accessor *input = sampler.input;
    const cgltf_accessor *output = sampler.output;
    if ((input == nullptr) || (input->type != cgltf_type_scalar) ||
        (input->count == 0U)) {
      set_result(outResult, AnimationImportResult::InvalidInputAccessor);
      return false;
    }

    const AnimInterpolation interpolation =
        convert_interpolation(sampler.interpolation);
    const std::size_t sampleMultiplier =
        interpolation == AnimInterpolation::CubicSpline ? 3U : 1U;
    if ((output == nullptr) ||
        (output->type != output_type_for_target(target)) ||
        (output->count != input->count * sampleMultiplier)) {
      set_result(outResult, AnimationImportResult::InvalidOutputAccessor);
      return false;
    }

    AnimTrack track{};
    track.joint = joint;
    track.target = target;
    track.interpolation = interpolation;

    float trackDuration = 0.0F;
    if (!decode_track_times(input, &track.times, &trackDuration) ||
        !decode_track_values(output, target, interpolation,
                             static_cast<std::size_t>(input->count),
                             &track)) {
      set_result(outResult, AnimationImportResult::DecodeFailed);
      return false;
    }

    if (trackDuration > parsed.durationSeconds) {
      parsed.durationSeconds = trackDuration;
    }
    parsed.tracks.push_back(track);
  }

  *outClip = parsed;
  set_result(outResult, AnimationImportResult::Ok);
  return true;
}

} // namespace engine::tools
