// Verifies the runtime animation system end to end on cooked fixtures:
// controller JSON parsing (clips, states, transitions, events, initial
// state), controller caching, state-machine updates on a live World
// (initial bind, parameter-driven transition with crossfade, looping),
// clip-timeline event firing, and skin-palette handoff to the renderer.

#include "engine/runtime/animation_system.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include "anim_cook.h"
#include "engine/core/hash.h"
#include "engine/core/vfs.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/world.h"

namespace {

using engine::runtime::AnimationComponent;
using engine::runtime::kInvalidAnimSlot;
namespace math = engine::math;
namespace tools = engine::tools;

constexpr const char *kSkelPath = "anim_controller_test.skel";
constexpr const char *kIdlePath = "anim_controller_test.idle.anim";
constexpr const char *kWalkPath = "anim_controller_test.walk.anim";
constexpr const char *kControllerPath = "anim_controller_test.animctrl.json";
constexpr const char *kMountPrefix = "animctrl";
constexpr const char *kControllerVirtualPath =
    "animctrl/anim_controller_test.animctrl.json";
constexpr float kFixedDt = 1.0F / 60.0F;

/// Removes a temporary test file when it exists.
void remove_file(const char *path) noexcept {
  if (path != nullptr) {
    static_cast<void>(std::remove(path));
  }
}

/// Deletes every fixture file this suite writes.
void cleanup_files() noexcept {
  remove_file(kSkelPath);
  remove_file(kIdlePath);
  remove_file(kWalkPath);
  remove_file(kControllerPath);
}

/// Writes a text file for the controller JSON fixture.
bool write_text(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

/// One translation track with two keys on the given joint.
tools::AnimTrack make_translation_track(std::uint32_t joint, float duration,
                                        const math::Vec3 &from,
                                        const math::Vec3 &to) {
  tools::AnimTrack track{};
  track.joint = joint;
  track.target = tools::AnimTrackTarget::Translation;
  track.interpolation = tools::AnimInterpolation::Linear;
  track.times = {0.0F, duration};
  track.vec3Values = {from, to};
  return track;
}

/// Cooks the two-joint skeleton, the idle and walk clips, and the
/// controller JSON, then mounts them for the runtime loaders.
bool cook_fixtures() {
  tools::Skeleton skeleton{};
  skeleton.joints.resize(2U);
  skeleton.joints[0].name = "root";
  skeleton.joints[0].parent = tools::kInvalidSkeletonJoint;
  skeleton.joints[1].name = "tip";
  skeleton.joints[1].parent = 0U;
  skeleton.joints[1].restTranslation = math::Vec3(0.0F, 1.0F, 0.0F);
  skeleton.rootJoint = 0U;
  for (tools::SkeletonJoint &joint : skeleton.joints) {
    joint.inverseBindMatrix = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                               0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                               0.0F, 0.0F, 0.0F, 1.0F};
  }

  std::vector<std::uint32_t> remap{};
  if (!tools::reorder_skeleton_parent_first(&skeleton, &remap) ||
      !tools::write_skeleton_asset(kSkelPath, skeleton)) {
    return false;
  }

  tools::AnimClip idle{};
  idle.name = "idle";
  idle.durationSeconds = 1.0F;
  idle.tracks.push_back(make_translation_track(
      1U, 1.0F, math::Vec3(0.0F, 1.0F, 0.0F), math::Vec3(0.0F, 1.0F, 0.0F)));

  tools::AnimClip walk{};
  walk.name = "walk";
  walk.durationSeconds = 0.4F;
  walk.tracks.push_back(make_translation_track(
      1U, 0.4F, math::Vec3(0.0F, 1.0F, 0.0F), math::Vec3(0.0F, 1.0F, 1.0F)));

  if (!tools::write_anim_clip_asset(kIdlePath, idle, remap) ||
      !tools::write_anim_clip_asset(kWalkPath, walk, remap)) {
    return false;
  }

  const char *controller =
      "{"
      "\"skeleton\":\"animctrl/anim_controller_test.skel\","
      "\"clips\":["
      "{\"name\":\"idle\",\"path\":\"animctrl/anim_controller_test.idle.anim\"},"
      "{\"name\":\"walk\",\"path\":\"animctrl/anim_controller_test.walk.anim\"}"
      "],"
      "\"initial\":\"idle\","
      "\"states\":["
      "{\"name\":\"idle\",\"clip\":\"idle\",\"loop\":true},"
      "{\"name\":\"walk\",\"clip\":\"walk\",\"loop\":true}"
      "],"
      "\"transitions\":["
      "{\"from\":\"idle\",\"to\":\"walk\",\"param\":\"speed\",\"when\":\">\","
      "\"value\":0.5,\"blend\":0.1},"
      "{\"from\":\"walk\",\"to\":\"idle\",\"param\":\"speed\",\"when\":\"<\","
      "\"value\":0.5,\"blend\":0.1}"
      "],"
      "\"events\":["
      "{\"clip\":\"walk\",\"time\":0.1,\"name\":\"footstep\"}"
      "]"
      "}";
  if (!write_text(kControllerPath, controller)) {
    return false;
  }

  if (!engine::core::initialize_vfs()) {
    return false;
  }
  return engine::core::mount(kMountPrefix, ".");
}

/// EXPECTATION: the controller JSON parses into exactly 2 clips, 2 states,
/// 2 transitions, 1 event, initial state "idle", and re-acquiring the same
/// path returns the same cached slot.
int check_controller_parse_and_cache() {
  engine::runtime::reset_anim_controllers();
  const std::uint32_t slot =
      engine::runtime::acquire_anim_controller(kControllerVirtualPath);
  if (slot == kInvalidAnimSlot) {
    std::puts("controller failed to load");
    return 1;
  }
  const engine::runtime::AnimControllerData *controller =
      engine::runtime::get_anim_controller(slot);
  if (controller == nullptr) {
    std::puts("controller slot not readable");
    return 1;
  }
  if ((controller->clipCount != 2U) || (controller->stateCount != 2U) ||
      (controller->transitionCount != 2U) || (controller->eventCount != 1U)) {
    std::puts("controller counts mismatch");
    return 1;
  }
  if (controller->states[controller->initialState].nameHash !=
      engine::core::fnv1a_32("idle")) {
    std::puts("controller initial state mismatch");
    return 1;
  }
  if ((controller->skeleton.jointCount != 2U) ||
      (controller->events[0].nameHash != engine::core::fnv1a_32("footstep"))) {
    std::puts("controller skeleton/event mismatch");
    return 1;
  }
  if (engine::runtime::acquire_anim_controller(kControllerVirtualPath) !=
      slot) {
    std::puts("controller was not cached");
    return 1;
  }
  return 0;
}

/// EXPECTATION: the first update binds the component to its controller in
/// the initial state and hands exactly one palette (slot 0, 2 joints) to
/// the renderer; setting speed above the threshold transitions to "walk"
/// with an active crossfade; playback past the 0.4s walk clip wraps; and
/// the footstep event at t=0.1 fires exactly once per walk loop.
int check_state_machine_and_events() {
  engine::runtime::reset_anim_controllers();
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto entity = world->create_entity();
  AnimationComponent component{};
  std::snprintf(component.controllerPath, sizeof(component.controllerPath),
                "%s", kControllerVirtualPath);
  if (!world->add_animation_component(entity, component)) {
    std::puts("add_animation_component failed");
    return 1;
  }

  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *bound =
      world->get_animation_component_ptr(entity);
  if ((bound == nullptr) || (bound->controllerSlot == kInvalidAnimSlot)) {
    std::puts("component did not bind its controller");
    return 1;
  }
  if ((bound->currentState != 0U) || (bound->paletteSlot != 0U)) {
    std::puts("initial state or palette slot mismatch");
    return 1;
  }
  if (engine::renderer::skin_palette_count() != 1U) {
    std::puts("renderer palette count mismatch");
    return 1;
  }

  if (!engine::runtime::set_anim_param(*world, entity, "speed", 1.0F)) {
    std::puts("set_anim_param failed");
    return 1;
  }
  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *walking =
      world->get_animation_component_ptr(entity);
  if ((walking == nullptr) || (walking->currentState != 1U)) {
    std::puts("transition to walk did not happen");
    return 1;
  }
  if (!(walking->blendRemaining > 0.0F)) {
    std::puts("crossfade did not start");
    return 1;
  }

  // 60 more fixed steps = 1.0s of walk (duration 0.4s): the clip must
  // wrap and the footstep at t=0.1 must fire on each of the three passes
  // through that timestamp (t=0.1, 0.5, 0.9).
  std::size_t footstepCount = 0U;
  for (int i = 0; i < 60; ++i) {
    engine::runtime::update_animations(*world, kFixedDt);
    for (std::size_t e = 0U; e < engine::runtime::fired_anim_event_count();
         ++e) {
      const engine::runtime::FiredAnimEvent *fired =
          engine::runtime::fired_anim_event_at(e);
      if ((fired != nullptr) &&
          (fired->nameHash == engine::core::fnv1a_32("footstep"))) {
        ++footstepCount;
      }
    }
  }
  if (footstepCount != 3U) {
    std::printf("footstep count mismatch: %zu\n", footstepCount);
    return 1;
  }
  const AnimationComponent *looped =
      world->get_animation_component_ptr(entity);
  if ((looped == nullptr) || !(looped->stateTime < 0.4F) ||
      !(looped->stateTime >= 0.0F)) {
    std::puts("walk clip did not wrap");
    return 1;
  }

  if (!engine::runtime::set_anim_param(*world, entity, "speed", 0.0F)) {
    std::puts("set_anim_param reset failed");
    return 1;
  }
  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *idleAgain =
      world->get_animation_component_ptr(entity);
  if ((idleAgain == nullptr) || (idleAgain->currentState != 0U)) {
    std::puts("transition back to idle did not happen");
    return 1;
  }

  engine::renderer::set_skin_palettes(nullptr, 0U);
  return 0;
}

/// EXPECTATION: a parameter queued through the phase-safe script entry is
/// drained at the start of the next update, so the same update already
/// takes the idle-to-walk transition.
int check_queued_params() {
  engine::runtime::reset_anim_controllers();
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto entity = world->create_entity();
  AnimationComponent component{};
  std::snprintf(component.controllerPath, sizeof(component.controllerPath),
                "%s", kControllerVirtualPath);
  if (!world->add_animation_component(entity, component)) {
    std::puts("add_animation_component failed");
    return 1;
  }
  engine::runtime::update_animations(*world, kFixedDt);

  if (!engine::runtime::queue_anim_param(entity, "speed", 1.0F)) {
    std::puts("queue_anim_param failed");
    return 1;
  }
  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *walking =
      world->get_animation_component_ptr(entity);
  if ((walking == nullptr) || (walking->currentState != 1U)) {
    std::puts("queued param did not drive the transition");
    return 1;
  }
  engine::renderer::set_skin_palettes(nullptr, 0U);
  return 0;
}

/// EXPECTATION: reset_anim_controllers releases every cached slot (the
/// scene-load and shutdown paths call it), the released slot reads as
/// empty, and the same path re-acquires a valid slot afterwards — so
/// repeated scene loads can never exhaust the fixed registry.
int check_reset_releases_slots() {
  engine::runtime::reset_anim_controllers();
  const std::uint32_t slot =
      engine::runtime::acquire_anim_controller(kControllerVirtualPath);
  if ((slot == kInvalidAnimSlot) ||
      (engine::runtime::get_anim_controller(slot) == nullptr)) {
    std::puts("initial acquire failed");
    return 1;
  }

  engine::runtime::reset_anim_controllers();
  if (engine::runtime::get_anim_controller(slot) != nullptr) {
    std::puts("reset left the slot readable");
    return 1;
  }

  const std::uint32_t reacquired =
      engine::runtime::acquire_anim_controller(kControllerVirtualPath);
  if (reacquired == kInvalidAnimSlot) {
    std::puts("re-acquire after reset failed");
    return 1;
  }
  if (engine::runtime::get_anim_controller(reacquired) == nullptr) {
    std::puts("re-acquired slot not readable");
    return 1;
  }
  return 0;
}

/// EXPECTATION: a missing controller path leaves the component unbound and
/// publishes zero palettes instead of crashing.
int check_missing_controller() {
  engine::runtime::reset_anim_controllers();
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto entity = world->create_entity();
  AnimationComponent component{};
  std::snprintf(component.controllerPath, sizeof(component.controllerPath),
                "%s", "animctrl/does_not_exist.json");
  if (!world->add_animation_component(entity, component)) {
    std::puts("add_animation_component failed");
    return 1;
  }

  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *bound =
      world->get_animation_component_ptr(entity);
  if ((bound == nullptr) || (bound->controllerSlot != kInvalidAnimSlot) ||
      (bound->paletteSlot != kInvalidAnimSlot)) {
    std::puts("missing controller was not handled");
    return 1;
  }
  if (engine::renderer::skin_palette_count() != 0U) {
    std::puts("palette count should be zero");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit H-17): non-finite or extreme playback speeds cannot
/// hang update_animations — an infinite speed resets the state time to
/// zero, and a huge finite speed wraps in constant time to a finite state
/// time inside [0, duration) of the looping 1.0s idle clip.
int check_extreme_speed_cannot_hang() {
  engine::runtime::reset_anim_controllers();
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const auto entity = world->create_entity();
  AnimationComponent component{};
  std::snprintf(component.controllerPath, sizeof(component.controllerPath),
                "%s", kControllerVirtualPath);
  if (!world->add_animation_component(entity, component)) {
    std::puts("add_animation_component failed");
    return 1;
  }
  engine::runtime::update_animations(*world, kFixedDt);

  AnimationComponent *mutableComponent =
      world->get_animation_component_ptr(entity);
  if (mutableComponent == nullptr) {
    return 1;
  }
  mutableComponent->playbackSpeed = std::numeric_limits<float>::infinity();
  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *afterInfinite =
      world->get_animation_component_ptr(entity);
  if ((afterInfinite == nullptr) || (afterInfinite->stateTime != 0.0F)) {
    std::puts("infinite speed did not reset the state time");
    return 1;
  }

  mutableComponent = world->get_animation_component_ptr(entity);
  mutableComponent->playbackSpeed = 1.0e30F;
  engine::runtime::update_animations(*world, kFixedDt);
  const AnimationComponent *afterExtreme =
      world->get_animation_component_ptr(entity);
  if ((afterExtreme == nullptr) || !(afterExtreme->stateTime >= 0.0F) ||
      !(afterExtreme->stateTime < 1.0F)) {
    std::puts("extreme speed did not wrap into the clip duration");
    return 1;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!cook_fixtures()) {
    cleanup_files();
    std::puts("fixture cook failed");
    return 1;
  }

  int result = check_controller_parse_and_cache();
  if (result == 0) {
    result = check_state_machine_and_events();
  }
  if (result == 0) {
    result = check_queued_params();
  }
  if (result == 0) {
    result = check_reset_releases_slots();
  }
  if (result == 0) {
    result = check_missing_controller();
  }
  if (result == 0) {
    result = check_extreme_speed_cannot_hang();
  }
  engine::runtime::reset_anim_controllers();
  cleanup_files();
  return result;
}
