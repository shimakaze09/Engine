// Implements the runtime animation system: controller JSON parsing into
// fixed registry slots, the per-step state machine (parameter-driven
// transitions, looping, crossfade), clip-timeline event firing, and
// skinning-palette handoff to the renderer.

#include "engine/runtime/animation_system.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/hash.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/core/vfs.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

// Controller slots allocate on first acquire (~114 KB each) so the
// registry costs nothing until a scene actually animates.
std::unique_ptr<AnimControllerData> g_controllers[kMaxAnimControllers]{};

FiredAnimEvent g_firedEvents[kMaxFiredAnimEvents]{};
std::size_t g_firedEventCount = 0U;

/// One queued script-side parameter write drained by update_animations.
struct PendingAnimParam final {
  core::Entity entity{};
  std::uint32_t nameHash = 0U;
  float value = 0.0F;
};

constexpr std::size_t kMaxPendingAnimParams = 64U;
PendingAnimParam g_pendingParams[kMaxPendingAnimParams]{};
std::size_t g_pendingParamCount = 0U;

/// Logs one controller load failure with its path.
void log_controller_error(const char *path, const char *reason) noexcept {
  char message[224] = {};
  std::snprintf(message, sizeof(message), "%s: %s",
                (path != nullptr) ? path : "(null)", reason);
  core::log_message(core::LogLevel::Error, "animation", message);
}

/// Index of the named clip in the controller; kInvalidAnimSlot when absent.
std::uint32_t find_clip(const AnimControllerData &controller,
                        std::uint32_t nameHash) noexcept {
  for (std::uint32_t i = 0U; i < controller.clipCount; ++i) {
    if (controller.clipNameHashes[i] == nameHash) {
      return i;
    }
  }
  return kInvalidAnimSlot;
}

/// Index of the named state in the controller; kInvalidAnimSlot when absent.
std::uint32_t find_state(const AnimControllerData &controller,
                         std::uint32_t nameHash) noexcept {
  for (std::uint32_t i = 0U; i < controller.stateCount; ++i) {
    if (controller.states[i].nameHash == nameHash) {
      return i;
    }
  }
  return kInvalidAnimSlot;
}

/// Reads a required string field into a bounded buffer.
bool read_string_field(const core::JsonParser &parser,
                       const core::JsonValue &object, const char *field,
                       char *out, std::size_t outSize) noexcept {
  core::JsonValue value{};
  return parser.get_object_field(object, field, &value) &&
         parser.copy_string(value, out, outSize);
}

/// Parses the "clips" array: name + cooked .anim path per element.
bool parse_controller_clips(const core::JsonParser &parser,
                            const core::JsonValue &root,
                            AnimControllerData &controller) noexcept {
  core::JsonValue clipsValue{};
  if (!parser.get_object_field(root, "clips", &clipsValue)) {
    return false;
  }
  const std::size_t clipCount = parser.array_size(clipsValue);
  if ((clipCount == 0U) || (clipCount > kMaxAnimClips)) {
    return false;
  }
  for (std::size_t i = 0U; i < clipCount; ++i) {
    core::JsonValue clipValue{};
    char name[64] = {};
    char path[192] = {};
    if (!parser.get_array_element(clipsValue, i, &clipValue) ||
        !read_string_field(parser, clipValue, "name", name, sizeof(name)) ||
        !read_string_field(parser, clipValue, "path", path, sizeof(path))) {
      return false;
    }
    if (!load_animation_clip_asset(path, &controller.clips[i])) {
      return false;
    }
    controller.clipNameHashes[i] = core::fnv1a_32(name);
  }
  controller.clipCount = static_cast<std::uint32_t>(clipCount);
  return true;
}

/// Parses the "states" array: name, clip reference, loop, speed.
bool parse_controller_states(const core::JsonParser &parser,
                             const core::JsonValue &root,
                             AnimControllerData &controller) noexcept {
  core::JsonValue statesValue{};
  if (!parser.get_object_field(root, "states", &statesValue)) {
    return false;
  }
  const std::size_t stateCount = parser.array_size(statesValue);
  if ((stateCount == 0U) || (stateCount > kMaxAnimStates)) {
    return false;
  }
  for (std::size_t i = 0U; i < stateCount; ++i) {
    core::JsonValue stateValue{};
    char name[64] = {};
    char clip[64] = {};
    if (!parser.get_array_element(statesValue, i, &stateValue) ||
        !read_string_field(parser, stateValue, "name", name, sizeof(name)) ||
        !read_string_field(parser, stateValue, "clip", clip, sizeof(clip))) {
      return false;
    }
    AnimState &state = controller.states[i];
    state.nameHash = core::fnv1a_32(name);
    state.clipIndex = find_clip(controller, core::fnv1a_32(clip));
    if (state.clipIndex == kInvalidAnimSlot) {
      return false;
    }
    core::JsonValue loopValue{};
    if (parser.get_object_field(stateValue, "loop", &loopValue) &&
        !parser.as_bool(loopValue, &state.loop)) {
      return false;
    }
    core::JsonValue speedValue{};
    if (parser.get_object_field(stateValue, "speed", &speedValue) &&
        !parser.as_float(speedValue, &state.speed)) {
      return false;
    }
    if (!std::isfinite(state.speed)) {
      return false;
    }
  }
  controller.stateCount = static_cast<std::uint32_t>(stateCount);
  return true;
}

/// Parses the optional "transitions" array: from/to states, parameter,
/// comparison ("<", ">", "=="), threshold, and blend seconds.
bool parse_controller_transitions(const core::JsonParser &parser,
                                  const core::JsonValue &root,
                                  AnimControllerData &controller) noexcept {
  core::JsonValue transitionsValue{};
  if (!parser.get_object_field(root, "transitions", &transitionsValue)) {
    return true;
  }
  const std::size_t transitionCount = parser.array_size(transitionsValue);
  if (transitionCount > kMaxAnimTransitions) {
    return false;
  }
  for (std::size_t i = 0U; i < transitionCount; ++i) {
    core::JsonValue transitionValue{};
    char from[64] = {};
    char to[64] = {};
    char param[64] = {};
    char when[8] = {};
    if (!parser.get_array_element(transitionsValue, i, &transitionValue) ||
        !read_string_field(parser, transitionValue, "from", from,
                           sizeof(from)) ||
        !read_string_field(parser, transitionValue, "to", to, sizeof(to)) ||
        !read_string_field(parser, transitionValue, "param", param,
                           sizeof(param)) ||
        !read_string_field(parser, transitionValue, "when", when,
                           sizeof(when))) {
      return false;
    }
    AnimTransition &transition = controller.transitions[i];
    transition.fromState = (std::strcmp(from, "any") == 0)
                               ? kInvalidAnimSlot
                               : find_state(controller, core::fnv1a_32(from));
    transition.toState = find_state(controller, core::fnv1a_32(to));
    if (((transition.fromState == kInvalidAnimSlot) &&
         (std::strcmp(from, "any") != 0)) ||
        (transition.toState == kInvalidAnimSlot)) {
      return false;
    }
    transition.paramHash = core::fnv1a_32(param);
    if (std::strcmp(when, ">") == 0) {
      transition.condition = AnimCondition::Greater;
    } else if (std::strcmp(when, "<") == 0) {
      transition.condition = AnimCondition::Less;
    } else if (std::strcmp(when, "==") == 0) {
      transition.condition = AnimCondition::Equals;
    } else {
      return false;
    }
    core::JsonValue thresholdValue{};
    if (parser.get_object_field(transitionValue, "value", &thresholdValue) &&
        !parser.as_float(thresholdValue, &transition.value)) {
      return false;
    }
    core::JsonValue blendValue{};
    if (parser.get_object_field(transitionValue, "blend", &blendValue) &&
        !parser.as_float(blendValue, &transition.blendSeconds)) {
      return false;
    }
  }
  controller.transitionCount = static_cast<std::uint32_t>(transitionCount);
  return true;
}

/// Parses the optional "events" array: clip reference, time, event name.
bool parse_controller_events(const core::JsonParser &parser,
                             const core::JsonValue &root,
                             AnimControllerData &controller) noexcept {
  core::JsonValue eventsValue{};
  if (!parser.get_object_field(root, "events", &eventsValue)) {
    return true;
  }
  const std::size_t eventCount = parser.array_size(eventsValue);
  if (eventCount > kMaxAnimEvents) {
    return false;
  }
  for (std::size_t i = 0U; i < eventCount; ++i) {
    core::JsonValue eventValue{};
    char clip[64] = {};
    AnimEvent &event = controller.events[i];
    if (!parser.get_array_element(eventsValue, i, &eventValue) ||
        !read_string_field(parser, eventValue, "clip", clip, sizeof(clip)) ||
        !read_string_field(parser, eventValue, "name", event.name,
                           sizeof(event.name))) {
      return false;
    }
    event.clipIndex = find_clip(controller, core::fnv1a_32(clip));
    if (event.clipIndex == kInvalidAnimSlot) {
      return false;
    }
    event.nameHash = core::fnv1a_32(event.name);
    core::JsonValue timeValue{};
    if (parser.get_object_field(eventValue, "time", &timeValue) &&
        !parser.as_float(timeValue, &event.timeSeconds)) {
      return false;
    }
  }
  controller.eventCount = static_cast<std::uint32_t>(eventCount);
  return true;
}

/// Parses one controller JSON document into a registry slot.
bool parse_controller(const char *virtualPath,
                      AnimControllerData &controller) noexcept {
  char *text = nullptr;
  std::size_t textSize = 0U;
  if (!core::vfs_read_text(virtualPath, &text, &textSize)) {
    log_controller_error(virtualPath, "controller read failed");
    return false;
  }

  bool ok = false;
  core::JsonParser parser{};
  if (parser.parse(text, textSize)) {
    const core::JsonValue *root = parser.root();
    char skeletonPath[192] = {};
    if ((root != nullptr) &&
        read_string_field(parser, *root, "skeleton", skeletonPath,
                          sizeof(skeletonPath)) &&
        load_skeleton_asset(skeletonPath, &controller.skeleton) &&
        parse_controller_clips(parser, *root, controller) &&
        parse_controller_states(parser, *root, controller) &&
        parse_controller_transitions(parser, *root, controller) &&
        parse_controller_events(parser, *root, controller)) {
      char initial[64] = {};
      controller.initialState = 0U;
      if (read_string_field(parser, *root, "initial", initial,
                            sizeof(initial))) {
        const std::uint32_t found =
            find_state(controller, core::fnv1a_32(initial));
        if (found != kInvalidAnimSlot) {
          controller.initialState = found;
        }
      }
      ok = true;
    } else {
      log_controller_error(virtualPath, "invalid controller document");
    }
  } else {
    log_controller_error(virtualPath, "controller JSON parse failed");
  }

  core::vfs_free(text);
  return ok;
}

/// Writes (or adds) a parameter by hash; false when the budget is full.
bool set_param_by_hash(AnimationComponent &component, std::uint32_t nameHash,
                       float value) noexcept {
  for (std::uint32_t i = 0U; i < component.paramCount; ++i) {
    if (component.params[i].nameHash == nameHash) {
      component.params[i].value = value;
      return true;
    }
  }
  if (component.paramCount >= AnimationComponent::kMaxParams) {
    return false;
  }
  component.params[component.paramCount] = AnimParam{nameHash, value};
  ++component.paramCount;
  return true;
}

/// Current value of a named parameter on the component (0 when unset).
float param_value(const AnimationComponent &component,
                  std::uint32_t nameHash) noexcept {
  for (std::uint32_t i = 0U; i < component.paramCount; ++i) {
    if (component.params[i].nameHash == nameHash) {
      return component.params[i].value;
    }
  }
  return 0.0F;
}

/// True when the transition's condition holds for the component.
bool condition_met(const AnimTransition &transition,
                   const AnimationComponent &component) noexcept {
  const float value = param_value(component, transition.paramHash);
  switch (transition.condition) {
  case AnimCondition::Greater:
    return value > transition.value;
  case AnimCondition::Less:
    return value < transition.value;
  case AnimCondition::Equals:
    return value == transition.value;
  }
  return false;
}

/// Records one fired event for this update's queue.
void push_fired_event(core::Entity entity, const AnimEvent &event) noexcept {
  if (g_firedEventCount >= kMaxFiredAnimEvents) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      core::log_message(core::LogLevel::Warning, "animation",
                        "fired-event queue full; animation events dropped");
    }
    return;
  }
  FiredAnimEvent &fired = g_firedEvents[g_firedEventCount];
  fired.entity = entity;
  fired.nameHash = event.nameHash;
  core::copy_string(fired.name, sizeof(fired.name), event.name);
  ++g_firedEventCount;
}

/// Fires every event of the state's clip whose time lies in
/// (previousTime, newTime]; loop wraps fire the tail then the head.
void fire_clip_events(core::Entity entity,
                      const AnimControllerData &controller,
                      std::uint32_t clipIndex, float previousTime,
                      float newTime, bool wrapped,
                      float clipDuration) noexcept {
  for (std::uint32_t i = 0U; i < controller.eventCount; ++i) {
    const AnimEvent &event = controller.events[i];
    if (event.clipIndex != clipIndex) {
      continue;
    }
    const float t = event.timeSeconds;
    const bool hit =
        wrapped ? ((t > previousTime) && (t <= clipDuration)) ||
                      ((t >= 0.0F) && (t <= newTime))
                : ((t > previousTime) && (t <= newTime));
    if (hit) {
      push_fired_event(entity, event);
    }
  }
}

/// Advances one state's local clip time; reports wraps for event firing.
float advance_state_time(const AnimControllerData &controller,
                         std::uint32_t stateIndex, float time, float dt,
                         bool *outWrapped) noexcept {
  const AnimState &state = controller.states[stateIndex];
  const AnimationClip &clip = controller.clips[state.clipIndex];
  const float duration = clip.durationSeconds;
  float newTime = time + (dt * state.speed);
  *outWrapped = false;
  if ((duration <= 0.0F) || !std::isfinite(duration)) {
    return 0.0F;
  }
  // A non-finite time (overflowed accumulation or a poisoned dt) would
  // otherwise spin the wrap below forever or corrupt every later sample.
  if (!std::isfinite(newTime)) {
    return 0.0F;
  }
  if (newTime < 0.0F) {
    if (!state.loop) {
      return 0.0F;
    }
    newTime = std::fmod(newTime, duration);
    return (newTime < 0.0F) ? (newTime + duration) : newTime;
  }
  if (newTime > duration) {
    if (state.loop) {
      newTime = std::fmod(newTime, duration);
      *outWrapped = true;
    } else {
      newTime = duration;
    }
  }
  return newTime;
}

} // namespace

std::uint32_t acquire_anim_controller(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return kInvalidAnimSlot;
  }

  std::uint32_t freeSlot = kInvalidAnimSlot;
  for (std::uint32_t i = 0U; i < kMaxAnimControllers; ++i) {
    if (g_controllers[i] != nullptr) {
      if (std::strcmp(g_controllers[i]->sourcePath, virtualPath) == 0) {
        return i;
      }
    } else if (freeSlot == kInvalidAnimSlot) {
      freeSlot = i;
    }
  }
  if (freeSlot == kInvalidAnimSlot) {
    log_controller_error(virtualPath, "controller slots exhausted");
    return kInvalidAnimSlot;
  }

  std::unique_ptr<AnimControllerData> controller(new (std::nothrow)
                                                     AnimControllerData());
  if (controller == nullptr) {
    log_controller_error(virtualPath, "controller allocation failed");
    return kInvalidAnimSlot;
  }
  if (!parse_controller(virtualPath, *controller)) {
    return kInvalidAnimSlot;
  }
  core::copy_string(controller->sourcePath, sizeof(controller->sourcePath),
                    virtualPath);
  controller->active = true;
  g_controllers[freeSlot] = std::move(controller);
  return freeSlot;
}

const AnimControllerData *get_anim_controller(std::uint32_t slot) noexcept {
  if ((slot >= kMaxAnimControllers) || (g_controllers[slot] == nullptr)) {
    return nullptr;
  }
  return g_controllers[slot].get();
}

void reset_anim_controllers() noexcept {
  for (std::unique_ptr<AnimControllerData> &controller : g_controllers) {
    controller.reset();
  }
  g_firedEventCount = 0U;
  g_pendingParamCount = 0U;
}

void update_animations(World &world, float dt) noexcept {
  static renderer::SkinPalette palettes[renderer::kMaxSkinPalettes]{};
  std::size_t paletteCount = 0U;
  g_firedEventCount = 0U;

  for (std::size_t i = 0U; i < g_pendingParamCount; ++i) {
    const PendingAnimParam &pending = g_pendingParams[i];
    AnimationComponent *component =
        world.get_animation_component_ptr(pending.entity);
    if (component != nullptr) {
      static_cast<void>(
          set_param_by_hash(*component, pending.nameHash, pending.value));
    }
  }
  g_pendingParamCount = 0U;

  world.for_each<AnimationComponent>([&](core::Entity entity,
                                         const AnimationComponent &) {
    AnimationComponent *component = world.get_animation_component_ptr(entity);
    if (component == nullptr) {
      return;
    }
    component->paletteSlot = kInvalidAnimSlot;

    if (component->controllerSlot == kInvalidAnimSlot) {
      component->controllerSlot =
          acquire_anim_controller(component->controllerPath);
      if (component->controllerSlot == kInvalidAnimSlot) {
        return;
      }
      const AnimControllerData &controller =
          *g_controllers[component->controllerSlot];
      component->currentState = controller.initialState;
      component->previousState = controller.initialState;
      component->stateTime = 0.0F;
      component->blendRemaining = 0.0F;
    }

    const AnimControllerData *controller =
        get_anim_controller(component->controllerSlot);
    if ((controller == nullptr) ||
        (component->currentState >= controller->stateCount)) {
      return;
    }

    if (component->playing) {
      const float scaledDt = dt * component->playbackSpeed;

      for (std::uint32_t i = 0U; i < controller->transitionCount; ++i) {
        const AnimTransition &transition = controller->transitions[i];
        const bool fromMatches =
            (transition.fromState == kInvalidAnimSlot) ||
            (transition.fromState == component->currentState);
        if (fromMatches && (transition.toState != component->currentState) &&
            condition_met(transition, *component)) {
          component->previousState = component->currentState;
          component->previousStateTime = component->stateTime;
          component->currentState = transition.toState;
          component->stateTime = 0.0F;
          component->blendDuration = transition.blendSeconds;
          component->blendRemaining = transition.blendSeconds;
          break;
        }
      }

      const float previousTime = component->stateTime;
      bool wrapped = false;
      component->stateTime = advance_state_time(
          *controller, component->currentState, component->stateTime,
          scaledDt, &wrapped);
      const AnimState &state = controller->states[component->currentState];
      fire_clip_events(entity, *controller, state.clipIndex, previousTime,
                       component->stateTime, wrapped,
                       controller->clips[state.clipIndex].durationSeconds);

      if (component->blendRemaining > 0.0F) {
        bool previousWrapped = false;
        component->previousStateTime = advance_state_time(
            *controller, component->previousState,
            component->previousStateTime, scaledDt, &previousWrapped);
        component->blendRemaining -= scaledDt;
        if (component->blendRemaining < 0.0F) {
          component->blendRemaining = 0.0F;
        }
      }
    }

    if (paletteCount >= renderer::kMaxSkinPalettes) {
      return;
    }

    const AnimSkeleton &skeleton = controller->skeleton;
    const AnimState &state = controller->states[component->currentState];
    static JointPose pose[kMaxAnimJoints];
    sample_clip_pose(skeleton, controller->clips[state.clipIndex],
                     component->stateTime, pose);

    if ((component->blendRemaining > 0.0F) &&
        (component->blendDuration > 0.0F) &&
        (component->previousState < controller->stateCount)) {
      const AnimState &previous =
          controller->states[component->previousState];
      static JointPose previousPose[kMaxAnimJoints];
      sample_clip_pose(skeleton, controller->clips[previous.clipIndex],
                       component->previousStateTime, previousPose);
      const float currentWeight =
          1.0F - (component->blendRemaining / component->blendDuration);
      blend_poses(previousPose, pose, skeleton.jointCount, currentWeight,
                  pose);
    }

    static math::Mat4 globalPose[kMaxAnimJoints];
    compute_global_pose(skeleton, pose, globalPose);
    renderer::SkinPalette &palette = palettes[paletteCount];
    compute_skinning_palette(skeleton, globalPose, palette.joints.data());
    palette.jointCount = skeleton.jointCount;
    component->paletteSlot = static_cast<std::uint32_t>(paletteCount);
    ++paletteCount;
  });

  renderer::set_skin_palettes(palettes, paletteCount);
}

bool set_anim_param(World &world, core::Entity entity, const char *name,
                    float value) noexcept {
  if ((name == nullptr) || (name[0] == '\0')) {
    return false;
  }
  AnimationComponent *component = world.get_animation_component_ptr(entity);
  if (component == nullptr) {
    return false;
  }
  return set_param_by_hash(*component, core::fnv1a_32(name), value);
}

bool queue_anim_param(core::Entity entity, const char *name,
                      float value) noexcept {
  if ((name == nullptr) || (name[0] == '\0')) {
    return false;
  }
  if (g_pendingParamCount >= kMaxPendingAnimParams) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      core::log_message(core::LogLevel::Warning, "animation",
                        "pending param queue full; parameter writes dropped");
    }
    return false;
  }
  PendingAnimParam &pending = g_pendingParams[g_pendingParamCount];
  pending.entity = entity;
  pending.nameHash = core::fnv1a_32(name);
  pending.value = value;
  ++g_pendingParamCount;
  return true;
}

std::size_t fired_anim_event_count() noexcept { return g_firedEventCount; }

const FiredAnimEvent *fired_anim_event_at(std::size_t index) noexcept {
  if (index >= g_firedEventCount) {
    return nullptr;
  }
  return &g_firedEvents[index];
}

} // namespace engine::runtime
