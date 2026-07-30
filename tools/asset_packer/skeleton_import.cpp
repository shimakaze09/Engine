// Implements glTF skin parsing for the Engine asset packer.

#include "skeleton_import.h"

#include <array>
#include <cmath>
#include <cstdio>

#include <cgltf.h>

#include "engine/math/mat4.h"

namespace engine::tools {
namespace {

constexpr std::array<float, 16U> kIdentityMatrix = {
    1.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 0.0F, 1.0F,
};

/// Writes an optional parser result value.
void set_result(SkeletonImportResult *outResult,
                SkeletonImportResult result) noexcept {
  if (outResult != nullptr) {
    *outResult = result;
  }
}

/// Decomposes a node's local transform into TRS rest-pose values. Handles
/// both TRS-authored and matrix-authored nodes (scale from column lengths,
/// rotation from the scale-normalized linear block).
void decompose_rest_pose(const cgltf_node *node, math::Vec3 *outTranslation,
                         math::Quat *outRotation,
                         math::Vec3 *outScale) noexcept {
  std::array<float, 16U> local{};
  cgltf_node_transform_local(node, local.data());

  math::Mat4 matrix{};
  for (std::size_t column = 0U; column < 4U; ++column) {
    matrix.columns[column] =
        math::Vec4(local[column * 4U + 0U], local[column * 4U + 1U],
                   local[column * 4U + 2U], local[column * 4U + 3U]);
  }

  *outTranslation =
      math::Vec3(matrix.columns[3].x, matrix.columns[3].y, matrix.columns[3].z);

  math::Vec3 scale(
      std::sqrt(matrix.columns[0].x * matrix.columns[0].x +
                matrix.columns[0].y * matrix.columns[0].y +
                matrix.columns[0].z * matrix.columns[0].z),
      std::sqrt(matrix.columns[1].x * matrix.columns[1].x +
                matrix.columns[1].y * matrix.columns[1].y +
                matrix.columns[1].z * matrix.columns[1].z),
      std::sqrt(matrix.columns[2].x * matrix.columns[2].x +
                matrix.columns[2].y * matrix.columns[2].y +
                matrix.columns[2].z * matrix.columns[2].z));
  *outScale = scale;

  math::Mat4 rotationOnly = matrix;
  rotationOnly.columns[3] = math::Vec4(0.0F, 0.0F, 0.0F, 1.0F);
  for (std::size_t column = 0U; column < 3U; ++column) {
    const float axisScale = (column == 0U) ? scale.x
                            : (column == 1U) ? scale.y
                                             : scale.z;
    if (axisScale > 1.0e-8F) {
      rotationOnly.columns[column].x /= axisScale;
      rotationOnly.columns[column].y /= axisScale;
      rotationOnly.columns[column].z /= axisScale;
    }
  }
  *outRotation = math::normalize(math::from_mat4(rotationOnly));
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

} // namespace

/// Returns a stable message for a skeleton import result.
const char *skeleton_import_result_message(
    SkeletonImportResult result) noexcept {
  switch (result) {
  case SkeletonImportResult::Ok:
    return "ok";
  case SkeletonImportResult::NullInput:
    return "null input";
  case SkeletonImportResult::SkinIndexOutOfRange:
    return "skin index out of range";
  case SkeletonImportResult::EmptySkin:
    return "skin has no joints";
  case SkeletonImportResult::TooManyJoints:
    return "skin exceeds supported joint count";
  case SkeletonImportResult::MissingJoint:
    return "skin contains a null joint";
  case SkeletonImportResult::InvalidInverseBindAccessor:
    return "inverse bind matrices accessor is invalid";
  case SkeletonImportResult::DecodeFailed:
    return "failed to decode inverse bind matrix";
  }

  return "unknown skeleton import result";
}

/// Parses a glTF skin into engine-facing skeleton data.
bool parse_gltf_skeleton(const cgltf_data *data, std::size_t skinIndex,
                         Skeleton *outSkeleton,
                         SkeletonImportResult *outResult) {
  if ((data == nullptr) || (outSkeleton == nullptr)) {
    set_result(outResult, SkeletonImportResult::NullInput);
    return false;
  }

  if (skinIndex >= static_cast<std::size_t>(data->skins_count)) {
    set_result(outResult, SkeletonImportResult::SkinIndexOutOfRange);
    return false;
  }

  const cgltf_skin &skin = data->skins[skinIndex];
  if ((skin.joints == nullptr) || (skin.joints_count == 0U)) {
    set_result(outResult, SkeletonImportResult::EmptySkin);
    return false;
  }

  if (skin.joints_count > kMaxSkeletonJoints) {
    set_result(outResult, SkeletonImportResult::TooManyJoints);
    return false;
  }

  const cgltf_accessor *inverseBindMatrices = skin.inverse_bind_matrices;
  if (inverseBindMatrices != nullptr) {
    if ((inverseBindMatrices->type != cgltf_type_mat4) ||
        (inverseBindMatrices->count < skin.joints_count)) {
      set_result(outResult, SkeletonImportResult::InvalidInverseBindAccessor);
      return false;
    }
  }

  Skeleton parsed{};
  parsed.joints.resize(static_cast<std::size_t>(skin.joints_count));
  parsed.rootJoint = find_joint_index(skin, skin.skeleton);

  for (cgltf_size i = 0U; i < skin.joints_count; ++i) {
    const cgltf_node *jointNode = skin.joints[i];
    if (jointNode == nullptr) {
      set_result(outResult, SkeletonImportResult::MissingJoint);
      return false;
    }

    SkeletonJoint &joint = parsed.joints[static_cast<std::size_t>(i)];
    if (jointNode->name != nullptr) {
      joint.name = jointNode->name;
    } else {
      char fallbackName[32] = {};
      std::snprintf(fallbackName, sizeof(fallbackName), "joint_%u",
                    static_cast<unsigned>(i));
      joint.name = fallbackName;
    }

    joint.parent = find_joint_index(skin, jointNode->parent);
    decompose_rest_pose(jointNode, &joint.restTranslation, &joint.restRotation,
                        &joint.restScale);
    joint.inverseBindMatrix = kIdentityMatrix;
    if (inverseBindMatrices != nullptr) {
      if (!cgltf_accessor_read_float(inverseBindMatrices, i,
                                     joint.inverseBindMatrix.data(),
                                     joint.inverseBindMatrix.size())) {
        set_result(outResult, SkeletonImportResult::DecodeFailed);
        return false;
      }
    }

    if ((parsed.rootJoint == kInvalidSkeletonJoint) &&
        (joint.parent == kInvalidSkeletonJoint)) {
      parsed.rootJoint = static_cast<std::uint32_t>(i);
    }
  }

  *outSkeleton = parsed;
  set_result(outResult, SkeletonImportResult::Ok);
  return true;
}

} // namespace engine::tools
