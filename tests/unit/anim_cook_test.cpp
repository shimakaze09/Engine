// Verifies the skeletal animation cook and load roundtrip: stable
// parent-before-child reordering (with cycle rejection), byte-exact .skel
// joint data through the VFS loader, track joint remapping in cooked
// .anim clips, exact sampled poses from loaded data, and corrupt-header
// rejection.

#include "anim_cook.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "engine/core/animation_asset.h"
#include "engine/core/hash.h"
#include "engine/core/nothrow_buffer.h"
#include "engine/core/vfs.h"
#include "engine/runtime/animation.h"

namespace {

using engine::runtime::AnimationClip;
using engine::runtime::AnimInterp;
using engine::runtime::AnimSkeleton;
using engine::runtime::AnimTarget;
using engine::runtime::JointPose;
using engine::tools::AnimClip;
using engine::tools::AnimInterpolation;
using engine::tools::AnimTrack;
using engine::tools::AnimTrackTarget;
using engine::tools::kInvalidSkeletonJoint;
using engine::tools::Skeleton;
using engine::tools::SkeletonJoint;
namespace math = engine::math;

constexpr const char *kSkelPath = "anim_cook_test.skel";
constexpr const char *kAnimPath = "anim_cook_test.anim";
constexpr const char *kMountPrefix = "animcook";
constexpr const char *kSkelVirtualPath = "animcook/anim_cook_test.skel";
constexpr const char *kAnimVirtualPath = "animcook/anim_cook_test.anim";

/// Removes a temporary test file when it exists.
void remove_file(const char *path) noexcept {
  if (path != nullptr) {
    static_cast<void>(std::remove(path));
  }
}

/// Three joints stored child-first: hand(parent arm), arm(parent root),
/// root. Every joint carries distinct rest and inverse-bind data so the
/// roundtrip checks catch any field mix-up.
Skeleton make_out_of_order_skeleton() {
  Skeleton skeleton{};
  skeleton.joints.resize(3U);
  skeleton.joints[0].name = "hand";
  skeleton.joints[0].parent = 1U;
  skeleton.joints[1].name = "arm";
  skeleton.joints[1].parent = 2U;
  skeleton.joints[2].name = "root";
  skeleton.joints[2].parent = kInvalidSkeletonJoint;
  skeleton.rootJoint = 2U;
  for (std::size_t i = 0U; i < 3U; ++i) {
    SkeletonJoint &joint = skeleton.joints[i];
    for (std::size_t k = 0U; k < 16U; ++k) {
      joint.inverseBindMatrix[k] =
          static_cast<float>((i * 100U) + k) * 0.25F;
    }
    const float base = static_cast<float>(i + 1U);
    joint.restTranslation = math::Vec3(base, base + 1.0F, base + 2.0F);
    joint.restScale = math::Vec3(base, 1.0F, 1.0F);
  }
  skeleton.joints[1].restRotation = math::Quat(0.5F, 0.5F, 0.5F, 0.5F);
  return skeleton;
}

/// Clip over the ORIGINAL joint indices of make_out_of_order_skeleton:
/// linear translation on hand(0), step rotation on root(2), cubic
/// translation with zero tangents on arm(1).
AnimClip make_source_clip() {
  AnimClip clip{};
  clip.name = "walk";
  clip.durationSeconds = 1.0F;

  AnimTrack handTranslation{};
  handTranslation.joint = 0U;
  handTranslation.target = AnimTrackTarget::Translation;
  handTranslation.interpolation = AnimInterpolation::Linear;
  handTranslation.times = {0.0F, 1.0F};
  handTranslation.vec3Values = {math::Vec3(1.0F, 2.0F, 3.0F),
                                math::Vec3(3.0F, 6.0F, 9.0F)};
  clip.tracks.push_back(handTranslation);

  AnimTrack rootRotation{};
  rootRotation.joint = 2U;
  rootRotation.target = AnimTrackTarget::Rotation;
  rootRotation.interpolation = AnimInterpolation::Step;
  rootRotation.times = {0.0F, 1.0F};
  rootRotation.quatValues = {
      math::Quat(0.0F, 0.0F, 0.0F, 1.0F),
      math::Quat(0.0F, 0.0F, 0.70710678F, 0.70710678F)};
  clip.tracks.push_back(rootRotation);

  AnimTrack armTranslation{};
  armTranslation.joint = 1U;
  armTranslation.target = AnimTrackTarget::Translation;
  armTranslation.interpolation = AnimInterpolation::CubicSpline;
  armTranslation.times = {0.0F, 1.0F};
  armTranslation.vec3Values = {math::Vec3(0.0F, 0.0F, 0.0F),
                               math::Vec3(1.0F, 0.0F, 0.0F)};
  armTranslation.inVec3Tangents = {math::Vec3(0.0F, 0.0F, 0.0F),
                                   math::Vec3(0.0F, 0.0F, 0.0F)};
  armTranslation.outVec3Tangents = {math::Vec3(0.0F, 0.0F, 0.0F),
                                    math::Vec3(0.0F, 0.0F, 0.0F)};
  clip.tracks.push_back(armTranslation);

  return clip;
}

/// Cooks the source skeleton and clip to the working directory and mounts
/// it for the runtime loaders; fills outRemap with the reorder remap.
bool cook_and_mount(std::vector<std::uint32_t> *outRemap) {
  Skeleton skeleton = make_out_of_order_skeleton();
  if (!engine::tools::reorder_skeleton_parent_first(&skeleton, outRemap)) {
    return false;
  }
  if (!engine::tools::write_skeleton_asset(kSkelPath, skeleton)) {
    return false;
  }
  const AnimClip clip = make_source_clip();
  if (!engine::tools::write_anim_clip_asset(kAnimPath, clip, *outRemap)) {
    return false;
  }
  if (!engine::core::initialize_vfs()) {
    return false;
  }
  return engine::core::mount(kMountPrefix, ".");
}

/// EXPECTATION: the stable reorder places root, arm, hand (lowest original
/// index first among ready joints), so remap = {2, 1, 0} and every parent
/// precedes its child.
int check_reorder_stable() {
  Skeleton skeleton = make_out_of_order_skeleton();
  std::vector<std::uint32_t> remap{};
  if (!engine::tools::reorder_skeleton_parent_first(&skeleton, &remap)) {
    std::puts("reorder failed on a valid skeleton");
    return 1;
  }
  if ((remap.size() != 3U) || (remap[0] != 2U) || (remap[1] != 1U) ||
      (remap[2] != 0U)) {
    std::puts("reorder remap mismatch");
    return 1;
  }
  if ((skeleton.joints[0].name != "root") ||
      (skeleton.joints[1].name != "arm") ||
      (skeleton.joints[2].name != "hand")) {
    std::puts("reorder joint order mismatch");
    return 1;
  }
  if ((skeleton.joints[0].parent != kInvalidSkeletonJoint) ||
      (skeleton.joints[1].parent != 0U) || (skeleton.joints[2].parent != 1U) ||
      (skeleton.rootJoint != 0U)) {
    std::puts("reorder parent rewrite mismatch");
    return 1;
  }
  return 0;
}

/// EXPECTATION: a two-joint parent cycle can never make progress, so the
/// reorder reports failure instead of looping.
int check_reorder_rejects_cycle() {
  Skeleton skeleton{};
  skeleton.joints.resize(2U);
  skeleton.joints[0].name = "a";
  skeleton.joints[0].parent = 1U;
  skeleton.joints[1].name = "b";
  skeleton.joints[1].parent = 0U;
  std::vector<std::uint32_t> remap{};
  if (engine::tools::reorder_skeleton_parent_first(&skeleton, &remap)) {
    std::puts("reorder accepted a parent cycle");
    return 1;
  }
  return 0;
}

/// EXPECTATION: every cooked joint field survives the write/load roundtrip
/// byte-exactly — parents in cooked order, FNV-1a name hashes, inverse
/// binds, and the full rest pose.
int check_skeleton_roundtrip() {
  std::vector<std::uint32_t> remap{};
  if (!cook_and_mount(&remap)) {
    std::puts("cook_and_mount failed");
    return 1;
  }

  AnimSkeleton loaded{};
  if (!engine::runtime::load_skeleton_asset(kSkelVirtualPath, &loaded)) {
    std::puts("load_skeleton_asset failed");
    return 1;
  }
  if (loaded.jointCount != 3U) {
    std::puts("loaded joint count mismatch");
    return 1;
  }
  if ((loaded.parents[0] != engine::runtime::kInvalidAnimJoint) ||
      (loaded.parents[1] != 0U) || (loaded.parents[2] != 1U)) {
    std::puts("loaded parent order mismatch");
    return 1;
  }
  if ((loaded.nameHashes[0] != engine::core::fnv1a_32("root")) ||
      (loaded.nameHashes[1] != engine::core::fnv1a_32("arm")) ||
      (loaded.nameHashes[2] != engine::core::fnv1a_32("hand"))) {
    std::puts("loaded name hash mismatch");
    return 1;
  }

  const Skeleton source = make_out_of_order_skeleton();
  for (std::uint32_t original = 0U; original < 3U; ++original) {
    const SkeletonJoint &joint = source.joints[original];
    const std::uint32_t cooked = remap[original];
    if (std::memcmp(&loaded.inverseBind[cooked],
                    joint.inverseBindMatrix.data(),
                    sizeof(float) * 16U) != 0) {
      std::puts("loaded inverse bind mismatch");
      return 1;
    }
    const JointPose &rest = loaded.restPose[cooked];
    if ((rest.translation.x != joint.restTranslation.x) ||
        (rest.translation.y != joint.restTranslation.y) ||
        (rest.translation.z != joint.restTranslation.z) ||
        (rest.rotation.x != joint.restRotation.x) ||
        (rest.rotation.y != joint.restRotation.y) ||
        (rest.rotation.z != joint.restRotation.z) ||
        (rest.rotation.w != joint.restRotation.w) ||
        (rest.scale.x != joint.restScale.x) ||
        (rest.scale.y != joint.restScale.y) ||
        (rest.scale.z != joint.restScale.z)) {
      std::puts("loaded rest pose mismatch");
      return 1;
    }
  }
  return 0;
}

/// EXPECTATION: cooked tracks land on the remapped joints (hand -> 2,
/// root -> 0, arm -> 1) and sampling the loaded clip at t=0.5 yields the
/// exact hand-computed values: linear midpoint (2, 4, 6), step holds the
/// identity rotation, cubic with zero tangents gives x = 0.5, and the
/// unanimated arm scale keeps its rest value.
int check_clip_roundtrip_sampling() {
  std::vector<std::uint32_t> remap{};
  if (!cook_and_mount(&remap)) {
    std::puts("cook_and_mount failed");
    return 1;
  }

  AnimSkeleton skeleton{};
  if (!engine::runtime::load_skeleton_asset(kSkelVirtualPath, &skeleton)) {
    std::puts("load_skeleton_asset failed");
    return 1;
  }
  AnimationClip clip{};
  if (!engine::runtime::load_animation_clip_asset(kAnimVirtualPath, &clip)) {
    std::puts("load_animation_clip_asset failed");
    return 1;
  }
  if ((clip.durationSeconds != 1.0F) || (clip.trackCount != 3U)) {
    std::puts("loaded clip header mismatch");
    return 1;
  }
  if ((clip.tracks[0].joint != 2U) || (clip.tracks[1].joint != 0U) ||
      (clip.tracks[2].joint != 1U)) {
    std::puts("loaded track joint remap mismatch");
    return 1;
  }
  if ((clip.tracks[0].interpolation != AnimInterp::Linear) ||
      (clip.tracks[1].interpolation != AnimInterp::Step) ||
      (clip.tracks[2].interpolation != AnimInterp::CubicSpline) ||
      (clip.tracks[1].target != AnimTarget::Rotation)) {
    std::puts("loaded track metadata mismatch");
    return 1;
  }

  JointPose pose[3] = {};
  engine::runtime::sample_clip_pose(skeleton, clip, 0.5F, pose);
  if ((pose[2].translation.x != 2.0F) || (pose[2].translation.y != 4.0F) ||
      (pose[2].translation.z != 6.0F)) {
    std::puts("sampled linear translation mismatch");
    return 1;
  }
  if ((pose[0].rotation.x != 0.0F) || (pose[0].rotation.y != 0.0F) ||
      (pose[0].rotation.z != 0.0F) || (pose[0].rotation.w != 1.0F)) {
    std::puts("sampled step rotation mismatch");
    return 1;
  }
  if ((pose[1].translation.x != 0.5F) || (pose[1].translation.y != 0.0F) ||
      (pose[1].translation.z != 0.0F)) {
    std::puts("sampled cubic translation mismatch");
    return 1;
  }
  if (pose[1].scale.x != 2.0F) {
    std::puts("unanimated channel lost its rest value");
    return 1;
  }
  return 0;
}

/// EXPECTATION: a corrupted magic word fails the load instead of feeding
/// garbage to evaluation.
int check_bad_magic_rejected() {
  std::vector<std::uint32_t> remap{};
  if (!cook_and_mount(&remap)) {
    std::puts("cook_and_mount failed");
    return 1;
  }

  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kSkelPath, "rb+") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kSkelPath, "rb+");
#endif
  if (file == nullptr) {
    std::puts("could not reopen cooked skeleton");
    return 1;
  }
  const std::uint32_t badMagic = 0xDEADBEEFU;
  const bool wrote =
      std::fwrite(&badMagic, sizeof(badMagic), 1U, file) == 1U;
  static_cast<void>(std::fclose(file));
  if (!wrote) {
    std::puts("could not corrupt cooked skeleton");
    return 1;
  }

  AnimSkeleton loaded{};
  if (engine::runtime::load_skeleton_asset(kSkelVirtualPath, &loaded)) {
    std::puts("loader accepted a corrupt magic word");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit H-17): a cooked clip whose header carries a
/// non-finite duration is rejected at load, before any state machine can
/// divide or wrap by it.
int check_non_finite_duration_rejected() {
  std::vector<std::uint32_t> remap{};
  if (!cook_and_mount(&remap)) {
    std::puts("cook_and_mount failed");
    return 1;
  }

  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kAnimPath, "rb+") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kAnimPath, "rb+");
#endif
  if (file == nullptr) {
    std::puts("could not reopen cooked clip");
    return 1;
  }
  const float nanDuration = std::numeric_limits<float>::quiet_NaN();
  bool wrote =
      std::fseek(file, static_cast<long>(offsetof(
                           engine::core::AnimClipAssetHeader,
                           durationSeconds)),
                 SEEK_SET) == 0;
  wrote = wrote &&
          (std::fwrite(&nanDuration, sizeof(nanDuration), 1U, file) == 1U);
  static_cast<void>(std::fclose(file));
  if (!wrote) {
    std::puts("could not corrupt cooked clip duration");
    return 1;
  }

  AnimationClip clip{};
  if (engine::runtime::load_animation_clip_asset(kAnimVirtualPath, &clip)) {
    std::puts("loader accepted a non-finite clip duration");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit #174): a cooked clip whose header claims a
/// payloadFloatCount above the loader's independent sanity ceiling is
/// rejected before any allocation is attempted, even though the file on
/// disk is only a few hundred bytes (nowhere near enough to honestly back
/// that many floats). The rejected clip is left untouched rather than
/// partially populated.
int check_hostile_payload_float_count_rejected() {
  std::vector<std::uint32_t> remap{};
  if (!cook_and_mount(&remap)) {
    std::puts("cook_and_mount failed");
    return 1;
  }

  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kAnimPath, "rb+") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kAnimPath, "rb+");
#endif
  if (file == nullptr) {
    std::puts("could not reopen cooked clip");
    return 1;
  }
  const std::uint32_t hostileCount = 0xFFFFFFF0U;
  bool wrote =
      std::fseek(file, static_cast<long>(offsetof(
                           engine::core::AnimClipAssetHeader,
                           payloadFloatCount)),
                 SEEK_SET) == 0;
  wrote = wrote &&
          (std::fwrite(&hostileCount, sizeof(hostileCount), 1U, file) == 1U);
  static_cast<void>(std::fclose(file));
  if (!wrote) {
    std::puts("could not corrupt cooked clip payload count");
    return 1;
  }

  AnimationClip clip{};
  if (engine::runtime::load_animation_clip_asset(kAnimVirtualPath, &clip)) {
    std::puts("loader accepted a hostile payload float count");
    return 1;
  }
  if ((clip.trackCount != 0U) || !clip.payload.empty()) {
    std::puts("rejected clip left a partial payload");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit #174, allocator-controlled): a payload size no real
/// machine can satisfy returns false from allocate() instead of
/// terminating the process (std::vector::resize's std::bad_alloc would
/// abort under the no-exception build), and leaves the buffer empty. This
/// exercises the exact allocation primitive load_animation_clip_asset now
/// uses for AnimationClip::payload, standing in for true OOM fault
/// injection.
int check_payload_allocate_survives_unallocatable_size() {
  engine::core::NothrowBuffer<float> buffer{};
  constexpr std::size_t kUnallocatable =
      std::numeric_limits<std::size_t>::max() / (sizeof(float) * 2U);
  if (buffer.allocate(kUnallocatable)) {
    std::puts("unallocatable payload size unexpectedly succeeded");
    return 1;
  }
  if (!buffer.empty() || (buffer.size() != 0U) || (buffer.data() != nullptr)) {
    std::puts("failed allocate() left the buffer in a non-empty state");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit H-20): clip names that sanitize to the same cooked
/// file name are rejected instead of silently overwriting the earlier
/// clip's output; distinct names and the empty-name fallback pass.
int check_duplicate_sanitized_clip_names_rejected() {
  std::unordered_set<std::string> usedNames{};
  std::string name{};

  if (!engine::tools::derive_unique_clip_name("Walk Fast", 0U, &usedNames,
                                              &name) ||
      (name != "Walk_Fast")) {
    std::puts("first clip name rejected or sanitized wrong");
    return 1;
  }
  if (engine::tools::derive_unique_clip_name("Walk/Fast", 1U, &usedNames,
                                             &name)) {
    std::puts("colliding sanitized clip name accepted");
    return 1;
  }
  if (!engine::tools::derive_unique_clip_name("Walk-Fast", 2U, &usedNames,
                                              &name) ||
      (name != "Walk-Fast")) {
    std::puts("distinct clip name rejected");
    return 1;
  }
  // Case-only variants are one file on Windows/macOS filesystems
  // (review item 2): the collision key folds case while the accepted
  // name keeps its original spelling.
  if (engine::tools::derive_unique_clip_name("walk-fast", 5U, &usedNames,
                                             &name)) {
    std::puts("case-only clip name collision accepted");
    return 1;
  }
  if (!engine::tools::derive_unique_clip_name("", 3U, &usedNames, &name) ||
      (name != "clip3")) {
    std::puts("empty clip name fallback wrong");
    return 1;
  }
  if (engine::tools::derive_unique_clip_name("clip3", 4U, &usedNames,
                                             &name)) {
    std::puts("fallback-name collision accepted");
    return 1;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_reorder_stable();
  if (result != 0) {
    return result;
  }
  result = check_reorder_rejects_cycle();
  if (result != 0) {
    return result;
  }
  result = check_skeleton_roundtrip();
  if (result != 0) {
    return result;
  }
  result = check_clip_roundtrip_sampling();
  if (result != 0) {
    return result;
  }
  result = check_bad_magic_rejected();
  if (result != 0) {
    remove_file(kSkelPath);
    remove_file(kAnimPath);
    return result;
  }
  result = check_non_finite_duration_rejected();
  if (result != 0) {
    remove_file(kSkelPath);
    remove_file(kAnimPath);
    return result;
  }
  result = check_hostile_payload_float_count_rejected();
  if (result != 0) {
    remove_file(kSkelPath);
    remove_file(kAnimPath);
    return result;
  }
  result = check_payload_allocate_survives_unallocatable_size();
  if (result != 0) {
    remove_file(kSkelPath);
    remove_file(kAnimPath);
    return result;
  }
  result = check_duplicate_sanitized_clip_names_rejected();
  remove_file(kSkelPath);
  remove_file(kAnimPath);
  return result;
}
