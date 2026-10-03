// Verifies navigation agents:
// - the World refuses a Nav Agent whose settings are out of range, with the
//   entity unchanged;
// - one step of the walk speeds up by the acceleration to the top speed,
//   brakes to rest at the stopping distance and never past it, follows the
//   path's corners and heights, and measures distance across the ground;
// - agents stepped on a baked level walk around a wall to their
//   destination, turn to face where they walk, fail unreachable and
//   off-mesh destinations without moving, wait for a mesh to load, stop
//   where they stand, keep their speed when sent somewhere new, forget a
//   removed agent or replaced World contents, and walk through a Character
//   Controller when they have one.

#include "engine/runtime/nav_agent.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <system_error>

#include "../test_harness.h"
#include "engine/core/vfs.h"
#include "engine/runtime/navigation_bake.h"
#include "engine/runtime/scene_navigation.h"
#include "engine/runtime/world.h"

namespace {

namespace nav = engine::navigation;
using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::NavAgentComponent;
using engine::runtime::NavAgentFailure;
using engine::runtime::NavAgentInfo;
using engine::runtime::NavAgents;
using engine::runtime::NavAgentStatus;
using engine::runtime::NavAgentWalk;
using engine::runtime::SceneNavigation;
using engine::runtime::World;

engine::tests::TestContext g_tests;

constexpr const char *kMount = "agenttest";
constexpr const char *kDirectory = "nav_agent_test_files";
constexpr const char *kMeshPath = "agenttest/level.navmesh";
constexpr float kDt = 1.0F / 60.0F;

/// One float of rounding at the scale of a 10 m path: positions here are
/// sums of a few products near 10, each correct to half an ulp there
/// (9.5e-7), so two ulps bound the drift between a position and the
/// distance walked to it.
constexpr float kPathRounding = 2.0e-6F;

float ground_distance(const Vec3 &a, const Vec3 &b) noexcept {
  const float dx = b.x - a.x;
  const float dz = b.z - a.z;
  return std::sqrt((dx * dx) + (dz * dz));
}

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

void test_validation() {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  g_tests.check(engine::runtime::nav_agent_is_valid(NavAgentComponent{}),
                "the default agent is valid");
  const Entity entity = world->create_entity();
  const auto refused = [&](NavAgentComponent agent, const char *what) {
    g_tests.check(!world->add_nav_agent(entity, agent), what);
  };
  NavAgentComponent bad{};
  bad.speed = 0.0F;
  refused(bad, "a speed of 0 is refused");
  bad = NavAgentComponent{};
  bad.speed = 100.5F;
  refused(bad, "a speed past 100 is refused");
  bad = NavAgentComponent{};
  bad.acceleration = std::numeric_limits<float>::quiet_NaN();
  refused(bad, "a NaN acceleration is refused");
  bad = NavAgentComponent{};
  bad.angularSpeed = -1.0F;
  refused(bad, "a negative angular speed is refused");
  bad = NavAgentComponent{};
  bad.stoppingDistance = 10.5F;
  refused(bad, "a stopping distance past 10 is refused");
  bad = NavAgentComponent{};
  bad.baseOffset = -10.5F;
  refused(bad, "a base offset below -10 is refused");
  g_tests.check(!world->has_nav_agent(entity),
                "every refusal left the entity without an agent");

  NavAgentComponent good{};
  good.speed = 5.0F;
  NavAgentComponent stored{};
  g_tests.check(world->add_nav_agent(entity, good) &&
                    world->get_nav_agent(entity, &stored) &&
                    (stored.speed == 5.0F),
                "the World holds the agent it was given");
  refused(bad, "a bad agent is refused over a good one");
  g_tests.check(world->get_nav_agent(entity, &stored) && (stored.speed == 5.0F),
                "the refusal left the stored agent unchanged");
}

/// Walks `corners` from their first corner until the walk arrives, at
/// most `limit` steps, checking every step stays on the straight path
/// along +X. Returns the steps taken; *outEnd is where it stopped.
int walk_straight(const NavAgentComponent &settings, const Vec3 *corners,
                  std::size_t count, int limit, Vec3 *outEnd) {
  Vec3 at = corners[0];
  std::size_t next = 0U;
  float speed = 0.0F;
  bool ramped = false;
  bool braking = false;
  bool rampExact = true;
  bool withinTop = true;
  bool travelMatches = true;
  bool brakesMonotone = true;
  bool staysOnLine = true;
  int steps = 0;
  for (; steps < limit; ++steps) {
    const NavAgentWalk walk = engine::runtime::walk_nav_agent_path(
        at, corners, count, next, speed, settings, kDt);
    const float expectedRamp = speed + (settings.acceleration * kDt);
    if (!ramped && !braking && (expectedRamp <= settings.speed) &&
        !walk.arrived && (walk.speed != expectedRamp) &&
        (walk.speed >= speed)) {
      rampExact = false;
    }
    if (walk.speed == settings.speed) {
      ramped = true;
    }
    if (walk.speed < speed) {
      braking = true;
    }
    if (braking && (walk.speed > speed)) {
      brakesMonotone = false;
    }
    withinTop = withinTop && (walk.speed <= settings.speed);
    staysOnLine =
        staysOnLine && (walk.position.y == 0.0F) && (walk.position.z == 0.0F);
    if (!walk.arrived) {
      travelMatches =
          travelMatches && (std::fabs((walk.position.x - at.x) -
                                      (walk.speed * kDt)) <= kPathRounding);
    }
    at = walk.position;
    next = walk.next;
    speed = walk.speed;
    if (walk.arrived) {
      ++steps;
      break;
    }
  }
  g_tests.check(rampExact,
                "each step speeds up by exactly the acceleration times dt");
  g_tests.check(ramped, "the walk reaches its top speed");
  g_tests.check(withinTop, "the speed never passes the top speed");
  g_tests.check(travelMatches, "each step travels its speed times dt");
  g_tests.check(braking && brakesMonotone,
                "the walk brakes, and braking never speeds it up again");
  g_tests.check(staysOnLine, "a straight path is walked on its line");
  *outEnd = at;
  return steps;
}

void test_walk() {
  NavAgentComponent settings{};
  settings.speed = 2.0F;
  settings.acceleration = 4.0F;
  settings.stoppingDistance = 0.0F;
  const Vec3 line[2] = {Vec3(0.0F, 0.0F, 0.0F), Vec3(10.0F, 0.0F, 0.0F)};
  Vec3 end{};
  const int steps = walk_straight(settings, line, 2U, 2000, &end);
  // Half a second (0.5 m) up to 2 m/s, 4.5 s over the 9 m between and half
  // a second (0.5 m) braking is 5.5 s, 330 steps; each step moves at the
  // speed it reaches, which gains about a step at each end.
  g_tests.check(steps == 328, "a 10 m walk takes the steps its speed allows");
  std::printf("  straight walk: %d steps, end x %.9g\n", steps, end.x);
  g_tests.check(std::fabs(end.x - 10.0F) <= kPathRounding,
                "with no stopping distance the walk ends on the last corner");

  settings.stoppingDistance = 0.5F;
  walk_straight(settings, line, 2U, 2000, &end);
  g_tests.check(std::fabs(end.x - 9.5F) <= kPathRounding,
                "the walk stops at its stopping distance, not past it");

  // A corner: along +X to (5, 0, 0), then along +Z.
  const Vec3 bend[3] = {Vec3(0.0F, 0.0F, 0.0F), Vec3(5.0F, 0.0F, 0.0F),
                        Vec3(5.0F, 0.0F, 5.0F)};
  settings = NavAgentComponent{};
  settings.speed = 3.0F;
  settings.stoppingDistance = 0.0F;
  Vec3 at = bend[0];
  std::size_t next = 0U;
  float speed = 0.0F;
  bool onPath = true;
  bool turned = false;
  bool travelAlongPath = true;
  for (int i = 0; i < 2000; ++i) {
    const NavAgentWalk walk = engine::runtime::walk_nav_agent_path(
        at, bend, 3U, next, speed, settings, kDt);
    if (!walk.arrived) {
      // The path length a step removes is its travel, the step that
      // rounds the corner included.
      const float before =
          engine::runtime::nav_agent_path_remaining(at, bend, 3U, next);
      travelAlongPath =
          travelAlongPath && (std::fabs((before - walk.remaining) -
                                        (walk.speed * kDt)) <= kPathRounding);
    }
    const bool firstLeg =
        (walk.position.z == 0.0F) && (walk.position.x <= 5.0F);
    const bool secondLeg =
        (walk.position.x == 5.0F) && (walk.position.z >= 0.0F);
    onPath = onPath && (firstLeg || secondLeg);
    turned = turned || (walk.next == 2U);
    at = walk.position;
    next = walk.next;
    speed = walk.speed;
    if (walk.arrived) {
      break;
    }
  }
  g_tests.check(onPath, "a bent path is walked along its two legs");
  g_tests.check(turned, "passing the corner moves on to the next one");
  g_tests.check(ground_distance(at, bend[2]) <= kPathRounding,
                "the bent walk ends on its last corner");
  g_tests.check(travelAlongPath,
                "each step travels its speed along the path, round the "
                "corner too");

  // Heights follow the path but distance is measured across the ground.
  const Vec3 ramp[3] = {Vec3(0.0F, 0.0F, 0.0F), Vec3(4.0F, 0.0F, 0.0F),
                        Vec3(8.0F, 2.0F, 0.0F)};
  g_tests.check(
      engine::runtime::nav_agent_path_remaining(ramp[0], ramp, 3U, 0U) == 8.0F,
      "the path length counts only the ground distance");
  at = ramp[0];
  next = 0U;
  speed = 0.0F;
  bool followsHeight = true;
  for (int i = 0; i < 2000; ++i) {
    const NavAgentWalk walk = engine::runtime::walk_nav_agent_path(
        at, ramp, 3U, next, speed, settings, kDt);
    const float expectedY =
        (walk.position.x <= 4.0F) ? 0.0F : (0.5F * (walk.position.x - 4.0F));
    followsHeight =
        followsHeight && (std::fabs(walk.position.y - expectedY) <= 1.0e-5F);
    at = walk.position;
    next = walk.next;
    speed = walk.speed;
    if (walk.arrived) {
      break;
    }
  }
  g_tests.check(followsHeight, "the height rises along the sloped leg");

  // A walk with nothing left arrives where it stands.
  NavAgentWalk done = engine::runtime::walk_nav_agent_path(
      Vec3(1.0F, 0.0F, 0.0F), line, 2U, 2U, 1.0F, settings, kDt);
  g_tests.check(done.arrived && (done.position.x == 1.0F) &&
                    (done.speed == 0.0F),
                "past the last corner the walk arrives without moving");
  settings.stoppingDistance = 2.0F;
  done = engine::runtime::walk_nav_agent_path(Vec3(8.5F, 0.0F, 0.0F), line, 2U,
                                              1U, 1.0F, settings, kDt);
  g_tests.check(done.arrived && (done.position.x == 8.5F),
                "within the stopping distance the walk arrives without "
                "moving");
  const Vec3 doubled[3] = {Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 0.0F),
                           Vec3(3.0F, 0.0F, 0.0F)};
  settings.stoppingDistance = 0.0F;
  done = engine::runtime::walk_nav_agent_path(doubled[0], doubled, 3U, 0U, 0.0F,
                                              settings, kDt);
  g_tests.check((done.next == 2U) && (done.position.x > 0.0F),
                "a repeated corner is passed in the same step");
}

/// Floor, a wall open past z = 4, a platform off the floor's edge, and a
/// surface over all of them, baked and loaded into `navigation`.
bool build_level(World &world, SceneNavigation &navigation,
                 Entity *outSurface) noexcept {
  const auto box = [&world](const Vec3 &centre, const Vec3 &half) {
    engine::runtime::Transform transform{};
    transform.position = centre;
    const Entity entity = world.create_entity();
    engine::runtime::Collider collider{};
    collider.halfExtents = half;
    return world.add_transform(entity, transform) &&
           world.add_collider(entity, collider);
  };
  engine::runtime::NavMeshSurfaceComponent surface{};
  surface.halfExtents = Vec3(20.0F, 3.0F, 12.0F);
  std::snprintf(surface.navMeshPath, sizeof(surface.navMeshPath), "%s",
                kMeshPath);
  engine::runtime::Transform centre{};
  centre.position = Vec3(5.0F, 0.0F, 0.0F);
  const Entity surfaceEntity = world.create_entity();
  if (!box(Vec3(0.0F, -0.5F, 0.0F), Vec3(10.0F, 0.5F, 10.0F)) ||
      !box(Vec3(0.0F, 1.0F, -3.0F), Vec3(0.5F, 1.0F, 7.0F)) ||
      !box(Vec3(16.5F, -0.5F, 0.0F), Vec3(2.5F, 0.5F, 3.0F)) ||
      !world.add_transform(surfaceEntity, centre) ||
      !world.add_nav_mesh_surface(surfaceEntity, surface)) {
    return false;
  }
  nav::NavMesh mesh{};
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  if (!engine::runtime::bake_nav_mesh_surface(world, surfaceEntity, &mesh) ||
      mesh.empty() || !nav::write_nav_mesh(mesh, &bytes, &size) ||
      !engine::core::vfs_write_binary(kMeshPath, bytes.get(), size)) {
    return false;
  }
  navigation.update(world);
  *outSurface = surfaceEntity;
  return navigation.mesh_for(surfaceEntity) != nullptr;
}

Entity add_agent(World &world, const Vec3 &at,
                 const NavAgentComponent &agent) noexcept {
  engine::runtime::Transform transform{};
  transform.position = at;
  const Entity entity = world.create_entity();
  return (world.add_transform(entity, transform) &&
          world.add_nav_agent(entity, agent))
             ? entity
             : engine::runtime::kInvalidEntity;
}

Vec3 position_of(const World &world, Entity entity) noexcept {
  engine::runtime::Transform transform{};
  static_cast<void>(world.get_transform(entity, &transform));
  return transform.position;
}

NavAgentInfo info_of(const NavAgents &agents, const World &world,
                     Entity entity) noexcept {
  NavAgentInfo info{};
  static_cast<void>(agents.info(world, entity, &info));
  return info;
}

void test_agents() {
  std::unique_ptr<World> world = make_world();
  std::unique_ptr<NavAgents> agents(new (std::nothrow) NavAgents());
  std::unique_ptr<SceneNavigation> navigation(new (std::nothrow)
                                                  SceneNavigation());
  std::unique_ptr<SceneNavigation> unloaded(new (std::nothrow)
                                                SceneNavigation());
  Entity surface{};
  if ((world == nullptr) || (agents == nullptr) || (navigation == nullptr) ||
      (unloaded == nullptr) || !build_level(*world, *navigation, &surface)) {
    g_tests.check(false, "build and bake the level");
    return;
  }

  NavAgentComponent settings{};
  settings.speed = 4.0F;
  settings.acceleration = 8.0F;
  settings.angularSpeed = 720.0F;
  const Entity walker = add_agent(*world, Vec3(-6.0F, 0.0F, -6.0F), settings);
  const Entity stranded = add_agent(*world, Vec3(-6.0F, 0.0F, 6.0F), settings);
  const Entity outside = add_agent(*world, Vec3(0.0F, 0.0F, 40.0F), settings);
  const Entity halted = add_agent(*world, Vec3(-8.0F, 0.0F, 8.0F), settings);
  const Entity plain = world->create_entity();
  static_cast<void>(world->add_transform(plain, engine::runtime::Transform{}));

  g_tests.check(info_of(*agents, *world, walker).status == NavAgentStatus::Idle,
                "an agent never sent anywhere is Idle");
  NavAgentInfo none{};
  g_tests.check(!agents->info(*world, plain, &none),
                "an entity without an agent has no agent to read");
  g_tests.check(!agents->set_destination(*world, plain, Vec3()),
                "an entity without an agent cannot be sent anywhere");
  g_tests.check(!agents->set_destination(
                    *world, walker,
                    Vec3(std::numeric_limits<float>::infinity(), 0.0F, 0.0F)),
                "a destination that is not finite is refused");

  // With no mesh loaded the destination waits rather than fails.
  g_tests.check(
      agents->set_destination(*world, walker, Vec3(6.0F, 0.0F, -6.0F)),
      "an agent is sent to a destination");
  agents->step(*world, *unloaded, kDt);
  g_tests.check(
      (info_of(*agents, *world, walker).status == NavAgentStatus::Pending) &&
          (position_of(*world, walker).x == -6.0F),
      "with no mesh loaded the agent waits where it stands");

  g_tests.check(
      agents->set_destination(*world, stranded, Vec3(16.5F, 0.0F, 0.0F)) &&
          agents->set_destination(*world, outside, Vec3(6.0F, 0.0F, 6.0F)) &&
          agents->set_destination(*world, halted, Vec3(8.0F, 0.0F, 8.0F)),
      "the other agents are sent off");

  const float most = (settings.speed * kDt) + 1.0e-5F;
  bool withinSpeed = true;
  bool pastWallEnd = false;
  bool onFloor = true;
  float speedBeforeResend = 0.0F;
  bool resentKeepsSpeed = true;
  Vec3 haltedAt{};
  bool haltedStill = true;
  int steps = 0;
  for (; steps < 2000; ++steps) {
    const Vec3 before = position_of(*world, walker);
    if (steps == 30) {
      g_tests.check(agents->stop(*world, halted), "a moving agent stops");
      haltedAt = position_of(*world, halted);
    }
    if (steps == 40) {
      // Sent to the same place again mid-walk: it keeps its speed.
      speedBeforeResend = info_of(*agents, *world, walker).speed;
      static_cast<void>(
          agents->set_destination(*world, walker, Vec3(6.0F, 0.0F, -6.0F)));
    }
    agents->step(*world, *navigation, kDt);
    if (steps == 40) {
      resentKeepsSpeed =
          info_of(*agents, *world, walker).speed >= speedBeforeResend;
    }
    const Vec3 after = position_of(*world, walker);
    withinSpeed = withinSpeed && (ground_distance(before, after) <= most);
    pastWallEnd = pastWallEnd || (after.z > 4.0F);
    onFloor = onFloor && (std::fabs(after.y) <= 1.0e-4F);
    if (steps > 30) {
      const Vec3 at = position_of(*world, halted);
      haltedStill = haltedStill && (at.x == haltedAt.x) && (at.z == haltedAt.z);
    }
    if (info_of(*agents, *world, walker).status == NavAgentStatus::Arrived) {
      ++steps;
      break;
    }
  }
  const NavAgentInfo walked = info_of(*agents, *world, walker);
  const Vec3 end = position_of(*world, walker);
  std::printf("  walker: %d steps, end (%.6g, %.6g, %.6g)\n", steps, end.x,
              end.y, end.z);
  g_tests.check(walked.status == NavAgentStatus::Arrived,
                "the walker arrives at its destination");
  g_tests.check(ground_distance(end, Vec3(6.0F, 0.0F, -6.0F)) <=
                    settings.stoppingDistance + kPathRounding,
                "it stops within its stopping distance of the destination");
  g_tests.check(pastWallEnd, "its path goes around the wall's open end");
  g_tests.check(withinSpeed, "no step carries it farther than its speed");
  g_tests.check(onFloor, "it walks on the floor's surface");
  g_tests.check(resentKeepsSpeed,
                "sent somewhere new while walking, it keeps its speed");
  const engine::math::Quat facing = [&] {
    engine::runtime::Transform transform{};
    static_cast<void>(world->get_transform(walker, &transform));
    return transform.rotation;
  }();
  g_tests.check((facing.x == 0.0F) && (facing.z == 0.0F) &&
                    (std::fabs(facing.y) > 0.1F),
                "it turned about the vertical to face where it walked");

  const NavAgentInfo strandedInfo = info_of(*agents, *world, stranded);
  g_tests.check((strandedInfo.status == NavAgentStatus::Failed) &&
                    (strandedInfo.failure == NavAgentFailure::Unreachable) &&
                    (position_of(*world, stranded).x == -6.0F),
                "an unreachable destination fails and the agent stays");
  const NavAgentInfo outsideInfo = info_of(*agents, *world, outside);
  g_tests.check((outsideInfo.status == NavAgentStatus::Failed) &&
                    (outsideInfo.failure == NavAgentFailure::OffMesh) &&
                    (position_of(*world, outside).z == 40.0F),
                "an agent off every mesh fails and stays");
  g_tests.check(
      (info_of(*agents, *world, halted).status == NavAgentStatus::Idle) &&
          haltedStill && (haltedAt.x > -8.0F),
      "a stopped agent is Idle where it stood");

  // A removed agent's state goes with it.
  g_tests.check(world->remove_nav_agent(stranded), "remove an agent");
  agents->step(*world, *navigation, kDt);
  g_tests.check(!agents->info(*world, stranded, &none),
                "a removed agent has nothing to read");
  // Replaced contents name other entities; their state is forgotten.
  world->mark_content_replaced(world->content_epoch());
  g_tests.check(info_of(*agents, *world, walker).status == NavAgentStatus::Idle,
                "replaced World contents forget every agent's state");
}

void test_character_agent() {
  std::unique_ptr<World> world = make_world();
  std::unique_ptr<NavAgents> agents(new (std::nothrow) NavAgents());
  std::unique_ptr<SceneNavigation> navigation(new (std::nothrow)
                                                  SceneNavigation());
  Entity surface{};
  if ((world == nullptr) || (agents == nullptr) || (navigation == nullptr) ||
      !build_level(*world, *navigation, &surface)) {
    g_tests.check(false, "build and bake the character's level");
    return;
  }
  // The character's origin is at its feet, so its base offset is 0.
  NavAgentComponent settings{};
  settings.speed = 3.0F;
  const Entity hero = add_agent(*world, Vec3(-6.0F, 0.0F, 6.0F), settings);
  engine::runtime::Collider capsule{};
  capsule.shape = engine::runtime::ColliderShape::Capsule;
  capsule.halfExtents = Vec3(0.3F, 0.6F, 0.3F);
  capsule.localPosition = Vec3(0.0F, 0.9F, 0.0F);
  if ((hero == engine::runtime::kInvalidEntity) ||
      !world->add_collider(hero, capsule) ||
      !world->add_character_controller(
          hero, engine::runtime::CharacterControllerComponent{})) {
    g_tests.check(false, "author the character");
    return;
  }
  g_tests.check(agents->set_destination(*world, hero, Vec3(6.0F, 0.0F, 6.0F)),
                "the character is sent across the floor");
  int steps = 0;
  for (; steps < 2000; ++steps) {
    agents->step(*world, *navigation, kDt);
    if (info_of(*agents, *world, hero).status != NavAgentStatus::Moving) {
      ++steps;
      break;
    }
  }
  const Vec3 end = position_of(*world, hero);
  std::printf("  character: %d steps, end (%.6g, %.6g, %.6g)\n", steps, end.x,
              end.y, end.z);
  const NavAgentInfo info = info_of(*agents, *world, hero);
  g_tests.check(info.status == NavAgentStatus::Arrived,
                "the character arrives through its controller");
  g_tests.check(ground_distance(end, Vec3(6.0F, 0.0F, 6.0F)) <=
                    settings.stoppingDistance + 1.0e-3F,
                "it stops within its stopping distance, give or take its "
                "controller's millimetre of slack");
  engine::runtime::CharacterControllerComponent controller{};
  g_tests.check(world->get_character_controller(hero, &controller) &&
                    controller.grounded,
                "its controller stood it on the floor the whole way");
}

} // namespace

/// Runs the navigation agent suites.
int main() {
  std::error_code ec{};
  std::filesystem::remove_all(kDirectory, ec);
  std::filesystem::create_directories(kDirectory, ec);
  if (!engine::core::initialize_vfs() ||
      !engine::core::mount(kMount, kDirectory)) {
    std::fprintf(stderr, "nav_agent_test: could not mount %s\n", kDirectory);
    return 1;
  }
  test_validation();
  test_walk();
  test_agents();
  test_character_agent();
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(kDirectory, ec);
  return g_tests.finish("nav_agent_test");
}
