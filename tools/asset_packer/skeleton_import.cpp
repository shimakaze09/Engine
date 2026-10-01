// Implements glTF skin parsing for the Engine asset packer.

#include "skeleton_import.h"

#include <array>
#include <cmath>
#include <cstdio>

#include <cgltf.h>

#include "engine/math/mat4.h"
#include "engine/math/transform.h"

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

/// Loads a cgltf column-major 16-float matrix into a math::Mat4.
math::Mat4 mat4_from_array(const std::array<float, 16U> &values) noexcept {
  math::Mat4 matrix{};
  for (std::size_t column = 0U; column < 4U; ++column) {
    matrix.columns[column] =
        math::Vec4(values[column * 4U + 0U], values[column * 4U + 1U],
                   values[column * 4U + 2U], values[column * 4U + 3U]);
  }
  return matrix;
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

/// Composes a joint's local matrix relative to its nearest joint ancestor,
/// mathematically flattening any non-joint intermediary nodes between them.
/// Reports the
/// joint ancestor (nullptr for a root) through outJointAncestor; false when
/// the ancestor walk exceeds the sanity depth cap.
bool compose_joint_local(const cgltf_skin &skin, const cgltf_node *jointNode,
                         math::Mat4 *outLocal,
                         const cgltf_node **outJointAncestor) noexcept {
  std::array<float, 16U> local{};
  cgltf_node_transform_local(jointNode, local.data());
  math::Mat4 composed = mat4_from_array(local);

  constexpr int kMaxAncestorDepth = 256;
  int depth = 0;
  const cgltf_node *ancestor = jointNode->parent;
  while ((ancestor != nullptr) &&
         (find_joint_index(skin, ancestor) == kInvalidSkeletonJoint)) {
    if (++depth > kMaxAncestorDepth) {
      return false;
    }
    std::array<float, 16U> ancestorLocal{};
    cgltf_node_transform_local(ancestor, ancestorLocal.data());
    composed = math::mul(mat4_from_array(ancestorLocal), composed);
    ancestor = ancestor->parent;
  }

  *outLocal = composed;
  *outJointAncestor = ancestor;
  return true;
}

/// Decomposes a joint-relative local matrix into TRS rest-pose values
/// through math::decompose_trs, so the importer and the editor share one
/// rule. Rejects transforms a TRS decomposition cannot represent —
/// negative determinant (mirroring), degenerate zero-scale axes, and
/// shear beyond exporter noise (math::kTrsShearTolerance) — instead of
/// emitting a corrupt rotation.
bool decompose_rest_pose(const math::Mat4 &matrix, math::Vec3 *outTranslation,
                         math::Quat *outRotation,
                         math::Vec3 *outScale) noexcept {
  // The shared TRS rule (shear, singular scale, non-finite values), plus a
  // refusal of a mirrored joint, which a skeleton pose cannot hold.
  return math::decompose_trs(matrix, outTranslation, outRotation, outScale) &&
         (outScale->z > 0.0F);
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
  case SkeletonImportResult::UnsupportedTransform:
    return "joint transform is not TRS-decomposable (negative scale, shear, "
           "zero-scale axis, or ancestor chain too deep)";
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

    math::Mat4 jointLocal{};
    const cgltf_node *jointAncestor = nullptr;
    if (!compose_joint_local(skin, jointNode, &jointLocal, &jointAncestor)) {
      set_result(outResult, SkeletonImportResult::UnsupportedTransform);
      return false;
    }
    joint.parent = find_joint_index(skin, jointAncestor);
    if (!decompose_rest_pose(jointLocal, &joint.restTranslation,
                             &joint.restRotation, &joint.restScale)) {
      set_result(outResult, SkeletonImportResult::UnsupportedTransform);
      return false;
    }
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
