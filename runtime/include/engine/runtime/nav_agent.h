// Declares navigation agents' runtime state and their fixed-step walk: a
// script gives an agent a destination, the agent's path is found on the
// scene's navigation meshes, and each fixed step moves it along that path
// at its speed, as Unity's NavMeshAgent and Godot's NavigationAgent3D do.
// The frame pipeline owns one NavAgents and steps it in the Input phase,
// after the scripts' on_fixed_tick of the same step.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

class SceneNavigation;

/// The most corners an agent's path holds; a longer path is refused as
/// NavAgentFailure::TooLong.
inline constexpr std::size_t kMaxNavAgentCorners = 64U;

/// Where an agent is in reaching its destination.
enum class NavAgentStatus : std::uint8_t {
  /// No destination set, or stopped.
  Idle = 0,
  /// A destination is set and its path not yet found: the scene's
  /// navigation meshes have not loaded.
  Pending = 1,
  /// Walking its path.
  Moving = 2,
  /// Within its stopping distance of the destination.
  Arrived = 3,
  /// No path, for the reason in NavAgentFailure.
  Failed = 4,
};

/// Why an agent's destination failed.
enum class NavAgentFailure : std::uint8_t {
  None = 0,
  /// The agent stands on no loaded navigation mesh.
  OffMesh = 1,
  /// No path joins the agent and the destination.
  Unreachable = 2,
  /// The path has more than kMaxNavAgentCorners corners.
  TooLong = 3,
  /// Its Character Controller refused the move (logged by
  /// runtime::move_character).
  CannotMove = 4,
};

/// What a script reads of an agent.
struct NavAgentInfo final {
  NavAgentStatus status = NavAgentStatus::Idle;
  NavAgentFailure failure = NavAgentFailure::None;
  /// The destination last set.
  math::Vec3 destination{};
  /// Path length left to the destination across the ground, in metres; 0
  /// unless Moving or Arrived.
  float remainingDistance = 0.0F;
  /// The speed reached, in metres per second.
  float speed = 0.0F;
};

/// One fixed step of a walk along a path, from walk_nav_agent_path.
struct NavAgentWalk final {
  /// Where the step ends, on the path.
  math::Vec3 position{};
  /// The corner walked toward next.
  std::size_t next = 0U;
  /// The speed reached.
  float speed = 0.0F;
  /// Path length left after the step, across the ground.
  float remaining = 0.0F;
  /// True when the step ends at the stopping distance.
  bool arrived = false;
};

/// Path length from `position` through corners[next..count) across the
/// ground plane: heights are followed but not counted.
float nav_agent_path_remaining(const math::Vec3 &position,
                               const math::Vec3 *corners, std::size_t count,
                               std::size_t next) noexcept;

/// Walks one step of `dt` seconds from `position` along corners[next..)
/// with `settings`, starting at `speed`. The speed rises by the
/// acceleration each step up to the top speed and falls as the stopping
/// distance nears, so the agent brakes to a stop where it would at that
/// deceleration; the step never carries it past the stopping distance.
/// Distances are measured across the ground and heights follow the path.
/// Pure: no World, no allocation.
NavAgentWalk walk_nav_agent_path(const math::Vec3 &position,
                                 const math::Vec3 *corners, std::size_t count,
                                 std::size_t next, float speed,
                                 const NavAgentComponent &settings,
                                 float dt) noexcept;

/// Every agent's destination, path and speed. Fixed capacity: one entry
/// per agent the World can hold, so it allocates nothing.
class NavAgents final {
public:
  NavAgents() noexcept = default;
  NavAgents(const NavAgents &) = delete;
  NavAgents &operator=(const NavAgents &) = delete;

  /// Sends `entity`'s agent to `destination`; its path is found on its
  /// next step. False, logged, with nothing changed, when the entity has
  /// no Nav Agent, is parented, or `destination` is not finite.
  bool set_destination(const World &world, Entity entity,
                       const math::Vec3 &destination) noexcept;
  /// Stops `entity`'s agent where it stands. False when it has no Nav
  /// Agent.
  bool stop(const World &world, Entity entity) noexcept;
  /// Reads `entity`'s agent. False when it has no Nav Agent; an agent
  /// never given a destination reads Idle.
  bool info(const World &world, Entity entity,
            NavAgentInfo *out) const noexcept;

  /// Moves every agent one fixed step of `dt` seconds, in the order the
  /// World stores them: a Pending agent's path is found first, then each
  /// Moving agent walks it, through runtime::move_character when it has a
  /// Character Controller and by setting its transform otherwise, and
  /// turns toward where it walks at its angular speed. The Input phase,
  /// main thread. A World whose contents were replaced forgets every
  /// agent's state.
  void step(World &world, SceneNavigation &navigation, float dt) noexcept;

  /// Forgets every agent's state.
  void clear() noexcept;

private:
  struct Entry final {
    Entity entity{};
    bool used = false;
    NavAgentStatus status = NavAgentStatus::Idle;
    NavAgentFailure failure = NavAgentFailure::None;
    math::Vec3 destination{};
    math::Vec3 corners[kMaxNavAgentCorners]{};
    std::uint32_t cornerCount = 0U;
    std::uint32_t next = 0U;
    float speed = 0.0F;
    float remaining = 0.0F;
    /// Facing about +Y, in radians; read from the transform when the walk
    /// starts.
    float yaw = 0.0F;
  };

  Entry *find(Entity entity) noexcept;
  const Entry *find(Entity entity) const noexcept;
  Entry *acquire(Entity entity) noexcept;
  void sync_epoch(const World &world) noexcept;
  void step_agent(World &world, SceneNavigation &navigation,
                  const NavAgentComponent &settings, Entry &entry,
                  float dt) noexcept;

  Entry m_entries[World::kMaxNavAgentComponents]{};
  std::uint32_t m_contentEpoch = 0U;
  bool m_epochKnown = false;
};

/// Makes `agents` the one scripts' agent calls use; null unbinds. The
/// frame pipeline binds its own while it runs.
void bind_nav_agents(NavAgents *agents) noexcept;
/// The bound NavAgents, or null.
NavAgents *bound_nav_agents() noexcept;

} // namespace engine::runtime
