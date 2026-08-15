// Host for the runtime skeleton/animation staleness regression (issue #91):
// mounts a cooked-asset directory and loads a .skel and its sibling .anim
// clip through the production loaders so the driving CMake script can
// assert the shared once-per-asset staleness warning on stdout.

#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/animation.h"

namespace {

constexpr const char *kMountPrefix = "assets";

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: asset_stale_skeletal_load_host <mount_dir> "
                 "<skel_relpath> <anim_relpath>\n");
    return 1;
  }

  if (!engine::core::initialize_logging()) {
    return 2;
  }
  if (!engine::core::initialize_vfs()) {
    return 2;
  }
  if (!engine::core::mount(kMountPrefix, argv[1])) {
    std::fprintf(stderr, "error: failed to mount %s\n", argv[1]);
    return 2;
  }

  char skelPath[256] = {};
  char animPath[256] = {};
  std::snprintf(skelPath, sizeof(skelPath), "%s/%s", kMountPrefix, argv[2]);
  std::snprintf(animPath, sizeof(animPath), "%s/%s", kMountPrefix, argv[3]);

  // Load order: skeleton, clip, skeleton again — proves .skel and .anim
  // both route through the shared once-per-asset check (not just one of
  // them) while a repeated load of the same asset still dedupes.
  engine::runtime::AnimSkeleton skeleton{};
  if (!engine::runtime::load_skeleton_asset(skelPath, &skeleton)) {
    std::fprintf(stderr, "error: first skeleton load failed: %s\n", skelPath);
    return 3;
  }

  engine::runtime::AnimationClip clip{};
  if (!engine::runtime::load_animation_clip_asset(animPath, &clip)) {
    std::fprintf(stderr, "error: clip load failed: %s\n", animPath);
    return 4;
  }

  engine::runtime::AnimSkeleton skeletonAgain{};
  if (!engine::runtime::load_skeleton_asset(skelPath, &skeletonAgain)) {
    std::fprintf(stderr, "error: second skeleton load failed: %s\n",
                 skelPath);
    return 5;
  }

  std::printf("LOAD_RESULT ok ok ok\n");
  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return 0;
}
