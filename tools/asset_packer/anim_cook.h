// Declares skeletal animation cooking: parent-before-child skeleton
// reordering (with the joint remap tracks and vertex weights must apply)
// and the .skel/.anim asset writers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "animation_import.h"
#include "skeleton_import.h"

namespace engine::tools {

/// Reorders the skeleton's joints parent-before-child (stable: among ready
/// joints the lowest original index goes first) and rewrites parent links.
/// outRemap[originalIndex] = cooked index. False on a parent cycle.
bool reorder_skeleton_parent_first(Skeleton *skeleton,
                                   std::vector<std::uint32_t> *outRemap);

/// Writes the cooked .skel file for an already-reordered skeleton.
bool write_skeleton_asset(const char *outputPath, const Skeleton &skeleton);

/// Writes the cooked .anim file, remapping track joints through the
/// skeleton reorder remap.
bool write_anim_clip_asset(const char *outputPath, const AnimClip &clip,
                           const std::vector<std::uint32_t> &jointRemap);

} // namespace engine::tools
