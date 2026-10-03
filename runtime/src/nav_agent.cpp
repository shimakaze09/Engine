// Implements navigation agents: destinations scripts set, paths found on
// the scene's navigation meshes, and the fixed-step walk along them that
// moves each agent's entity, through its Character Controller when it has
// one.

#include "engine/runtime/nav_agent.h"

#include <cmath>
#include <cstdio>

#include "engine/core/logging.h"
#include "engine/math/quat.h"
#include "engine/math/scalar.h"
#include "engine/runtime/character_controller.h"
#include "engine/runtime/scene_navigation.h"

namespace engine::runtime {

namespace {

constexpr const char *kLogChannel = "navigation";

/// How far, across the ground, a Character Controller may end from where
/// the walk put it and still count as on the path: farther, it was held
/// back by something, and it keeps walking toward the corner it had not
/// reached.
constexpr float kOnPathSlack = 1.0e-3F;

/// Slack on the stopping distance for an agent a Character Controller
/// moves, whose skin keeps it a hair from where the walk put it.
constexpr float kArriveSlack = 1.0e-3F;

constexpr float kTwoPi = 2.0F * math::kDetPi;
constexpr float kDegreesToRadians = math::kDetPi / 180.0F;

NavAgents *g_bound = nullptr;

float ground_distance(const math::Vec3 &from, const math::Vec3 &to) noexcept {
  const float dx = to.x - from.x;
  const float dz = to.z - from.z;
  return std::sqrt((dx * dx) + (dz * dz));
}

/// `angle` brought into [-pi, pi]; it is never more than one turn out.
float wrap_angle(float angle) noexcept {
  if (angle > math::kDetPi) {
    angle -= kTwoPi;
  } else if (angle < -math::kDetPi) {
    angle += kTwoPi;
  }
  return angle;
}

bool finite(const math::Vec3 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool has_loaded_mesh(const SceneNavigation &navigation) noexcept {
  for (std::size_t i = 0U; i < navigation.count(); ++i) {
    if (navigation.mesh_at(i) != nullptr) {
      return true;
    }
  }
  return false;
}

NavAgentFailure failure_for(navigation::NavPathResult result) noexcept {
  switch (result) {
  case navigation::NavPathResult::OffMesh:
    return NavAgentFailure::OffMesh;
  case navigation::NavPathResult::Unreachable:
    return NavAgentFailure::Unreachable;
  case navigation::NavPathResult::TooLong:
    return NavAgentFailure::TooLong;
  case navigation::NavPathResult::Found:
    break;
  }
  return NavAgentFailure::None;
}

void log_refusal(Entity entity, const char *why) noexcept {
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "nav agent destination refused for entity %u: %s",
                static_cast<unsigned>(entity.index), why);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

} // namespace

float nav_agent_path_remaining(const math::Vec3 &position,
                               const math::Vec3 *corners, std::size_t count,
                               std::size_t next) noexcept {
  if ((corners == nullptr) || (next >= count)) {
    return 0.0F;
  }
  float total = ground_distance(position, corners[next]);
  for (std::size_t i = next + 1U; i < count; ++i) {
    total += ground_distance(corners[i - 1U], corners[i]);
  }
  return total;
}

NavAgentWalk walk_nav_agent_path(const math::Vec3 &position,
                                 const math::Vec3 *corners, std::size_t count,
                                 std::size_t next, float speed,
                                 const NavAgentComponent &settings,
                                 float dt) noexcept {
  NavAgentWalk walk{};
  walk.position = position;
  walk.next = next;
  const float remaining =
      nav_agent_path_remaining(position, corners, count, next);
  const float stop = settings.stoppingDistance;
  if ((corners == nullptr) || (next >= count) || (remaining <= stop)) {
    walk.remaining = remaining;
    walk.arrived = true;
    return walk;
  }
  const float toStop = remaining - stop;
  const float acceleration = settings.acceleration;
  float velocity = speed + (acceleration * dt);
  if (velocity > settings.speed) {
    velocity = settings.speed;
  }
  // The fastest speed from which the acceleration still brakes to rest
  // at the stopping distance.
  const float braking = std::sqrt(2.0F * acceleration * toStop);
  if (velocity > braking) {
    velocity = braking;
  }
  // Braking alone only nears the stop; one step of acceleration is the
  // least speed, so the last steps close the distance.
  const float least = (acceleration * dt < settings.speed) ? (acceleration * dt)
                                                           : settings.speed;
  if (velocity < least) {
    velocity = least;
  }
  float travel = velocity * dt;
  bool arrived = false;
  if (travel >= toStop) {
    travel = toStop;
    arrived = true;
  }
  float left = travel;
  math::Vec3 at = position;
  std::size_t corner = next;
  while (corner < count) {
    const float segment = ground_distance(at, corners[corner]);
    if (segment <= left) {
      at = corners[corner];
      left -= segment;
      ++corner;
      continue;
    }
    const float t = left / segment;
    at = math::add(at, math::mul(math::sub(corners[corner], at), t));
    break;
  }
  walk.position = at;
  walk.next = corner;
  walk.arrived = arrived;
  walk.speed = arrived ? 0.0F : velocity;
  walk.remaining = arrived ? stop : (remaining - travel);
  return walk;
}

NavAgents::Entry *NavAgents::find(Entity entity) noexcept {
  for (Entry &entry : m_entries) {
    if (entry.used && (entry.entity == entity)) {
      return &entry;
    }
  }
  return nullptr;
}

const NavAgents::Entry *NavAgents::find(Entity entity) const noexcept {
  for (const Entry &entry : m_entries) {
    if (entry.used && (entry.entity == entity)) {
      return &entry;
    }
  }
  return nullptr;
}

NavAgents::Entry *NavAgents::acquire(Entity entity) noexcept {
  Entry *entry = find(entity);
  if (entry != nullptr) {
    return entry;
  }
  for (Entry &candidate : m_entries) {
    if (!candidate.used) {
      candidate = Entry{};
      candidate.used = true;
      candidate.entity = entity;
      return &candidate;
    }
  }
  return nullptr;
}

void NavAgents::sync_epoch(const World &world) noexcept {
  if (!m_epochKnown || (m_contentEpoch != world.content_epoch())) {
    clear();
    m_contentEpoch = world.content_epoch();
    m_epochKnown = true;
  }
}

void NavAgents::clear() noexcept {
  for (Entry &entry : m_entries) {
    entry = Entry{};
  }
  m_epochKnown = false;
}

bool NavAgents::set_destination(const World &world, Entity entity,
                                const math::Vec3 &destination) noexcept {
  sync_epoch(world);
  if (!world.has_nav_agent(entity)) {
    log_refusal(entity, "it has no Nav Agent");
    return false;
  }
  if (!finite(destination)) {
    log_refusal(entity, "the destination is not finite");
    return false;
  }
  Transform transform{};
  if (!world.get_transform(entity, &transform)) {
    log_refusal(entity, "it has no transform");
    return false;
  }
  if (transform.parentId != kInvalidPersistentId) {
    log_refusal(entity, "it is parented; an agent is a transform root");
    return false;
  }
  // One entry per agent the World can hold, and every entry whose agent
  // is gone is dropped each step, so an agent always finds one.
  Entry *entry = acquire(entity);
  if (entry == nullptr) {
    log_refusal(entity, "no agent state is free");
    return false;
  }
  // A new destination keeps the speed reached, as re-pathing does.
  const bool moving = (entry->status == NavAgentStatus::Moving);
  entry->status = NavAgentStatus::Pending;
  entry->failure = NavAgentFailure::None;
  entry->destination = destination;
  entry->cornerCount = 0U;
  entry->next = 0U;
  entry->remaining = 0.0F;
  if (!moving) {
    entry->speed = 0.0F;
  }
  return true;
}

bool NavAgents::stop(const World &world, Entity entity) noexcept {
  sync_epoch(world);
  if (!world.has_nav_agent(entity)) {
    return false;
  }
  Entry *entry = find(entity);
  if (entry != nullptr) {
    entry->status = NavAgentStatus::Idle;
    entry->failure = NavAgentFailure::None;
    entry->cornerCount = 0U;
    entry->next = 0U;
    entry->speed = 0.0F;
    entry->remaining = 0.0F;
  }
  return true;
}

bool NavAgents::info(const World &world, Entity entity,
                     NavAgentInfo *out) const noexcept {
  if ((out == nullptr) || !world.has_nav_agent(entity)) {
    return false;
  }
  *out = NavAgentInfo{};
  // State from replaced World contents names other entities.
  if (!m_epochKnown || (m_contentEpoch != world.content_epoch())) {
    return true;
  }
  const Entry *entry = find(entity);
  if (entry != nullptr) {
    out->status = entry->status;
    out->failure = entry->failure;
    out->destination = entry->destination;
    out->remainingDistance = entry->remaining;
    out->speed = entry->speed;
  }
  return true;
}

void NavAgents::step(World &world, SceneNavigation &navigation,
                     float dt) noexcept {
  sync_epoch(world);
  for (Entry &entry : m_entries) {
    if (entry.used && !world.has_nav_agent(entry.entity)) {
      entry = Entry{};
    }
  }
  // Moving an entity rewrites its transform, never the agents' storage,
  // so the dense order holds through the walk.
  const std::size_t count = world.nav_agent_count();
  for (std::size_t i = 0U; i < count; ++i) {
    const NavAgentComponent *settings = world.nav_agent_at(i);
    Entry *entry = find(world.nav_agent_entity_at(i));
    if ((settings == nullptr) || (entry == nullptr) ||
        ((entry->status != NavAgentStatus::Pending) &&
         (entry->status != NavAgentStatus::Moving))) {
      continue;
    }
    const NavAgentComponent copy = *settings;
    step_agent(world, navigation, copy, *entry, dt);
  }
}

void NavAgents::step_agent(World &world, SceneNavigation &navigation,
                           const NavAgentComponent &settings, Entry &entry,
                           float dt) noexcept {
  Transform transform{};
  if (!world.get_transform(entry.entity, &transform)) {
    return;
  }
  const math::Vec3 surface(transform.position.x,
                           transform.position.y - settings.baseOffset,
                           transform.position.z);
  if (entry.status == NavAgentStatus::Pending) {
    // The scene's meshes load in the frame's asset stage; until one has,
    // the path waits rather than fails.
    if (!has_loaded_mesh(navigation)) {
      return;
    }
    std::size_t found = 0U;
    const navigation::NavPathResult result = navigation.find_path(
        surface, entry.destination, entry.corners, kMaxNavAgentCorners, &found);
    if (result != navigation::NavPathResult::Found) {
      entry.status = NavAgentStatus::Failed;
      entry.failure = failure_for(result);
      entry.speed = 0.0F;
      entry.remaining = 0.0F;
      return;
    }
    entry.status = NavAgentStatus::Moving;
    entry.cornerCount = static_cast<std::uint32_t>(found);
    entry.next = 0U;
    float pitch = 0.0F;
    float yaw = 0.0F;
    float roll = 0.0F;
    static_cast<void>(math::to_euler(transform.rotation, &pitch, &yaw, &roll));
    entry.yaw = yaw;
  }

  const NavAgentWalk walk =
      walk_nav_agent_path(surface, entry.corners, entry.cornerCount, entry.next,
                          entry.speed, settings, dt);
  const math::Vec3 displacement = math::sub(walk.position, surface);

  const bool turns = (settings.angularSpeed > 0.0F);
  if (turns) {
    const float dx = displacement.x;
    const float dz = displacement.z;
    if (((dx * dx) + (dz * dz)) > 1.0e-12F) {
      // Forward is -Z, so facing (dx, dz) is a turn of atan2(-dx, -dz)
      // about +Y.
      const float target = math::det_atan2(-dx, -dz);
      float delta = wrap_angle(target - entry.yaw);
      const float most = settings.angularSpeed * kDegreesToRadians * dt;
      if (delta > most) {
        delta = most;
      } else if (delta < -most) {
        delta = -most;
      }
      entry.yaw = wrap_angle(entry.yaw + delta);
    }
  }

  bool arrived = false;
  if (world.has_character_controller(entry.entity)) {
    CharacterMoveOutcome outcome{};
    if (!move_character(world, entry.entity, displacement, &outcome)) {
      entry.status = NavAgentStatus::Failed;
      entry.failure = NavAgentFailure::CannotMove;
      entry.speed = 0.0F;
      entry.remaining = 0.0F;
      return;
    }
    Transform moved{};
    if (!world.get_transform(entry.entity, &moved)) {
      return;
    }
    if (turns) {
      moved.rotation = math::from_euler(0.0F, entry.yaw, 0.0F);
      static_cast<void>(world.add_transform(entry.entity, moved));
    }
    const math::Vec3 actual(moved.position.x,
                            moved.position.y - settings.baseOffset,
                            moved.position.z);
    // Held back short of where the walk went, it still heads for the
    // corner it had not reached.
    if (ground_distance(actual, walk.position) <= kOnPathSlack) {
      entry.next = static_cast<std::uint32_t>(walk.next);
    }
    entry.remaining = nav_agent_path_remaining(actual, entry.corners,
                                               entry.cornerCount, entry.next);
    arrived = walk.arrived ||
              (entry.remaining <= settings.stoppingDistance + kArriveSlack);
  } else {
    transform.position = math::add(transform.position, displacement);
    if (turns) {
      transform.rotation = math::from_euler(0.0F, entry.yaw, 0.0F);
    }
    static_cast<void>(world.add_transform(entry.entity, transform));
    entry.next = static_cast<std::uint32_t>(walk.next);
    entry.remaining = walk.remaining;
    arrived = walk.arrived;
  }
  entry.speed = arrived ? 0.0F : walk.speed;
  if (arrived) {
    entry.status = NavAgentStatus::Arrived;
  }
}

void bind_nav_agents(NavAgents *agents) noexcept { g_bound = agents; }

NavAgents *bound_nav_agents() noexcept { return g_bound; }

} // namespace engine::runtime
