// Declares the runtime animation system: animation controller assets
// (skeleton, clips, and a minimal state machine parsed from JSON), the
// fixed-slot controller registry, per-step evaluation with crossfade and
// clip-timeline events, and the parameter API gameplay drives.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"
#include "engine/runtime/animation.h"

namespace engine::runtime {

class World;

inline constexpr std::size_t kMaxAnimControllers = 16U;
inline constexpr std::size_t kMaxAnimClips = 8U;
inline constexpr std::size_t kMaxAnimStates = 8U;
inline constexpr std::size_t kMaxAnimTransitions = 16U;
inline constexpr std::size_t kMaxAnimEvents = 16U;
inline constexpr std::size_t kMaxFiredAnimEvents = 32U;
inline constexpr std::uint32_t kInvalidAnimSlot = 0xFFFFFFFFU;

/// Comparison a transition applies to its parameter value.
enum class AnimCondition : std::uint8_t {
  Greater = 0,
  Less = 1,
  Equals = 2,
};

/// One state machine state: a clip index with loop and speed settings.
struct AnimState final {
  std::uint32_t nameHash = 0U;
  std::uint32_t clipIndex = 0U;
  bool loop = true;
  float speed = 1.0F;
};

/// One transition: taken from fromState (kInvalidAnimSlot = any state)
/// when the named parameter satisfies the condition; the new state blends
/// in over blendSeconds.
struct AnimTransition final {
  std::uint32_t fromState = kInvalidAnimSlot;
  std::uint32_t toState = 0U;
  std::uint32_t paramHash = 0U;
  AnimCondition condition = AnimCondition::Greater;
  float value = 0.0F;
  float blendSeconds = 0.15F;
};

/// One clip-timeline event (e.g. a footstep) fired when playback crosses
/// timeSeconds within the owning clip.
struct AnimEvent final {
  static constexpr std::size_t kMaxNameLength = 31U; // +1 for null
  std::uint32_t clipIndex = 0U;
  float timeSeconds = 0.0F;
  std::uint32_t nameHash = 0U;
  char name[kMaxNameLength + 1U] = {};
};

/// One loaded animation controller: the skeleton, its clips, and the
/// state machine description shared by every entity referencing the same
/// controller JSON.
struct AnimControllerData final {
  bool active = false;
  char sourcePath[128] = {};
  AnimSkeleton skeleton{};
  std::uint32_t clipCount = 0U;
  AnimationClip clips[kMaxAnimClips]{};
  std::uint32_t clipNameHashes[kMaxAnimClips] = {};
  std::uint32_t initialState = 0U;
  std::uint32_t stateCount = 0U;
  AnimState states[kMaxAnimStates]{};
  std::uint32_t transitionCount = 0U;
  AnimTransition transitions[kMaxAnimTransitions]{};
  std::uint32_t eventCount = 0U;
  AnimEvent events[kMaxAnimEvents]{};
};

/// One event fired during the last update_animations call.
struct FiredAnimEvent final {
  core::Entity entity{};
  std::uint32_t nameHash = 0U;
  char name[AnimEvent::kMaxNameLength + 1U] = {};
};

/// Loads (or returns the cached slot of) the controller JSON at the VFS
/// path; kInvalidAnimSlot on parse or budget failure (logged).
std::uint32_t acquire_anim_controller(const char *virtualPath) noexcept;

/// Read access to a loaded controller; nullptr when the slot is empty.
const AnimControllerData *get_anim_controller(std::uint32_t slot) noexcept;

/// Clears every cached controller (scene teardown and tests).
void reset_anim_controllers() noexcept;

/// Advances every AnimationComponent by dt seconds: resolves controllers,
/// evaluates transitions and crossfades, computes skinning palettes, hands
/// them to the renderer, and stamps each component's paletteSlot.
void update_animations(World &world, float dt) noexcept;

/// Sets (or adds) a named parameter on the entity's animation component;
/// false when the entity has no component or the parameter budget is full.
bool set_anim_param(World &world, core::Entity entity, const char *name,
                    float value) noexcept;

/// Number of events fired by the last update_animations call.
std::size_t fired_anim_event_count() noexcept;

/// One fired event by index; nullptr when out of range.
const FiredAnimEvent *fired_anim_event_at(std::size_t index) noexcept;

} // namespace engine::runtime
