// GPU regression for #809: a skinned mesh drawn by the forward path is
// posed. The sample's walking character is drawn with the deferred path
// and shadows off, so every pixel of it comes from a forward draw and
// nothing else in the frame moves; two frames a fixed stretch of walk
// apart must then differ around it. Before the fix the forward programs
// had no skinning, the character stood in bind pose, and the two frames
// were identical.

#include "../gpu_scene_fixture.h"
#include "engine/runtime/animation_system.h"
#include "engine/runtime/scene_serializer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// Where the character stands once the scene is pruned.
const engine::math::Vec3 kFeet(0.0F, 0.0F, 0.0F);

/// Loads the sample's coin run and keeps only the ground and the walking
/// character, still: its script and body go, so it neither steers nor
/// falls, and only its animation can change what the frame shows.
bool author_scene(World &world) noexcept {
  if (!engine::runtime::load_scene(world, "assets/coin_run.scene")) {
    return false;
  }
  const Entity player = world.find_entity_by_name("Player");
  const Entity ground = world.find_entity_by_name("Ground");
  if ((player == kInvalidEntity) || (ground == kInvalidEntity)) {
    return false;
  }
  Entity doomed[64] = {};
  std::size_t doomedCount = 0U;
  world.for_each_alive([&](Entity entity) {
    if ((entity != player) && (entity != ground) && (doomedCount < 64U)) {
      doomed[doomedCount++] = entity;
    }
  });
  for (std::size_t i = 0U; i < doomedCount; ++i) {
    static_cast<void>(world.destroy_entity(doomed[i]));
  }
  static_cast<void>(world.remove_script_component(player));
  static_cast<void>(world.remove_rigid_body(player));
  engine::runtime::Transform transform{};
  if (!world.get_transform(player, &transform)) {
    return false;
  }
  transform.position = kFeet;
  return world.add_transform(player, transform) &&
         engine::tests::look_from(world, engine::math::Vec3(1.2F, 1.1F, 2.6F),
                                  engine::math::Vec3(0.0F, 0.9F, 0.0F));
}

/// Pixels whose brightest channel differs by more than `threshold`.
std::size_t changed_pixels(const CapturedFrame &a, const CapturedFrame &b,
                           int threshold) noexcept {
  if ((a.width != b.width) || (a.height != b.height)) {
    return 0U;
  }
  std::size_t changed = 0U;
  for (std::uint32_t y = 0U; y < a.height; ++y) {
    for (std::uint32_t x = 0U; x < a.width; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        const int delta = static_cast<int>(a.channel(x, y, c)) -
                          static_cast<int>(b.channel(x, y, c));
        if (std::abs(delta) > threshold) {
          ++changed;
          break;
        }
      }
    }
  }
  return changed;
}

int body(engine::EnginePipeline &pipeline, World &world) {
  engine::tests::checked(engine::core::cvar_set_bool("r_deferred", false),
                         "r_deferred");
  engine::tests::checked(engine::core::cvar_set_bool("r_shadows", false),
                         "r_shadows");
  const Entity player = world.find_entity_by_name("Player");
  if ((player == kInvalidEntity) ||
      !engine::runtime::queue_anim_param(player, "speed", 1.0F)) {
    std::fprintf(stderr, "FAIL: the character could not be set walking\n");
    return 10;
  }
  // A fixed step per frame, so the stretch of walk between the captures
  // is the same on every device however fast it draws.
  if (!pipeline.set_frame_delta_override(0.05)) {
    std::fprintf(stderr, "FAIL: the frame delta could not be fixed\n");
    return 11;
  }
  CapturedFrame first{};
  if (!engine::tests::settle_frames(pipeline, 30) ||
      !engine::tests::capture_presented_frame(pipeline, "skinned_forward_a.tga",
                                              &first)) {
    std::fprintf(stderr, "FAIL: no first frame\n");
    return 12;
  }
  CapturedFrame second{};
  if (!engine::tests::settle_frames(pipeline, 7) ||
      !engine::tests::capture_presented_frame(pipeline, "skinned_forward_b.tga",
                                              &second)) {
    std::fprintf(stderr, "FAIL: no second frame\n");
    return 13;
  }
  pipeline.clear_frame_delta_override();

  // Eight colour levels absorb rasteriser noise; a leg that moved changes
  // thousands of pixels, a pose that never changes none.
  constexpr int kThreshold = 8;
  constexpr std::size_t kMinimumChanged = 200U;
  const std::size_t changed = changed_pixels(first, second, kThreshold);
  if (changed < kMinimumChanged) {
    std::fprintf(stderr,
                 "FAIL: the walking character drawn forward did not move "
                 "(%zu pixels changed); it is standing in bind pose\n",
                 changed);
    return 14;
  }
  std::printf("skinned forward gpu: the forward-drawn character walks "
              "(%zu pixels changed)\n",
              changed);
  return 0;
}

} // namespace

int main() {
  return engine::tests::run_gpu_scene_test("skinned_forward_gpu", &body,
                                           &author_scene);
}
