// Declares glTF skeleton import data for the Engine asset packer.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"

struct cgltf_data;

namespace engine::tools {

inline constexpr std::uint32_t kInvalidSkeletonJoint = UINT32_MAX;
inline constexpr std::size_t kMaxSkeletonJoints = 128U;

/// Stores one imported skeleton joint: its inverse bind transform plus the
/// local rest pose (used for joints a clip does not animate).
struct SkeletonJoint final {
  std::string name{};
  std::uint32_t parent = kInvalidSkeletonJoint;
  std::array<float, 16U> inverseBindMatrix{};
  math::Vec3 restTranslation = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat restRotation = math::Quat();
  math::Vec3 restScale = math::Vec3(1.0F, 1.0F, 1.0F);
};

/// Stores a glTF skin as deterministic engine-facing skeleton data.
struct Skeleton final {
  std::vector<SkeletonJoint> joints{};
  std::uint32_t rootJoint = kInvalidSkeletonJoint;
};

/// Enumerates skeleton import outcomes for explicit tool error paths.
enum class SkeletonImportResult : std::uint8_t {
  Ok,
  NullInput,
  SkinIndexOutOfRange,
  EmptySkin,
  TooManyJoints,
  MissingJoint,
  InvalidInverseBindAccessor,
  DecodeFailed,
  UnsupportedTransform,
};

/// Returns a stable message for a skeleton import result.
const char *skeleton_import_result_message(
    SkeletonImportResult result) noexcept;

/// Parses a glTF skin into engine-facing skeleton data.
bool parse_gltf_skeleton(const cgltf_data *data, std::size_t skinIndex,
                         Skeleton *outSkeleton,
                         SkeletonImportResult *outResult = nullptr);

} // namespace engine::tools
