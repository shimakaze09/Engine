// Verifies Lua acceleration-write semantics (#102): set_acceleration
// composes against the current world gravity (not a hard-coded default),
// both acceleration setters wake a sleeping body when the effective term
// changes, identical rewrites cause no wake churn, and deferred BeginPlay
// writes apply the same semantics at flush time.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;
namespace math = engine::math;

constexpr const char *kScriptPath = "script_acceleration_test.lua";
constexpr float kDt = 1.0F / 60.0F;

/// Writes contents to the temporary test script path.
bool write_script_file(const char *contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0 || file == nullptr) {
    return false;
  }
#else
  file = std::fopen(kScriptPath, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

/// Advances one fixed step in the runtime pipeline's phase order.
bool step_world(rt::World &world) noexcept {
  world.begin_update_phase();
  const bool ok = rt::step_physics(world, kDt) &&
                  rt::resolve_collisions(world, kDt);
  world.commit_update_phase();
  world.begin_render_prep_phase();
  world.end_frame_phase();
  return ok;
}

/// Exact component-wise Vec3 equality (integer-exact float expectations).
bool vec3_equals(const math::Vec3 &value, float x, float y,
                 float z) noexcept {
  return (value.x == x) && (value.y == y) && (value.z == z);
}

// The mirrored falling_rock flow: hold cancels gravity, drop restores it.
constexpr const char *kScript =
    "g_e = nil\n"
    "function setup_body()\n"
    "    g_e = engine.spawn_entity()\n"
    "    if g_e == nil then error('spawn failed') end\n"
    "    engine.set_position(g_e, 0.0, 5.0, 0.0)\n"
    "    if not engine.add_rigid_body(g_e, 1.0) then error('body failed') end\n"
    "end\n"
    "function hold()\n"
    "    if not engine.set_acceleration(g_e, 0.0, 0.0, 0.0) then\n"
    "        error('hold write failed')\n"
    "    end\n"
    "end\n"
    "function drop()\n"
    "    if not engine.set_acceleration(g_e, 0.0, -9.8, 0.0) then\n"
    "        error('drop write failed')\n"
    "    end\n"
    "end\n"
    "function set_custom_gravity()\n"
    "    engine.set_gravity(0.0, -20.0, 0.0)\n"
    "end\n"
    "function request_zero_total()\n"
    "    if not engine.set_acceleration(g_e, 0.0, 0.0, 0.0) then\n"
    "        error('zero-total write failed')\n"
    "    end\n"
    "end\n"
    "function drop_custom()\n"
    "    if not engine.set_acceleration(g_e, 0.0, -20.0, 0.0) then\n"
    "        error('custom drop write failed')\n"
    "    end\n"
    "end\n"
    "function push_additional()\n"
    "    if not engine.set_additional_acceleration(g_e, 5.0, 0.0, 0.0) then\n"
    "        error('additional write failed')\n"
    "    end\n"
    "end\n"
    "function push_additional_huge()\n"
    "    if not engine.set_additional_acceleration(g_e, 1000000.0, 0.0,\n"
    "                                              0.0) then\n"
    "        error('huge additional write failed')\n"
    "    end\n"
    "end\n";

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  ctx.check(write_script_file(kScript), "write test script");
  ctx.check(sc::load_script(kScriptPath), "load test script");
  ctx.check(sc::call_script_function("setup_body"), "body setup");

  const rt::Entity body = world->find_entity_by_index(1U);
  rt::RigidBody *rigidBody = world->get_rigid_body_ptr(body);
  ctx.check(rigidBody != nullptr, "rigid body present");
  if (rigidBody == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }

  // Default gravity: hold() cancels it, drop() restores pure gravity.
  ctx.check(sc::call_script_function("hold"), "hold call");
  ctx.check(vec3_equals(rigidBody->acceleration, 0.0F, 9.8F, 0.0F),
            "hold stores the gravity-cancelling additive term");
  ctx.check(sc::call_script_function("drop"), "drop call");
  ctx.check(vec3_equals(rigidBody->acceleration, 0.0F, 0.0F, 0.0F),
            "drop stores a zero additive term under default gravity");

  // The falling_rock scenario: a sleeping held rock must start falling
  // after the drop write, through the production physics step.
  ctx.check(sc::call_script_function("hold"), "re-hold call");
  rigidBody->velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  rigidBody->sleeping = true;
  rigidBody->sleepFrameCount = 60U;
  ctx.check(sc::call_script_function("drop"), "drop on sleeping body");
  ctx.check(!rigidBody->sleeping && (rigidBody->sleepFrameCount == 0U),
            "acceleration change wakes the sleeping body");
  // setup_body's set_position granted Script authority; the motion command
  // must hand the body back to physics or it stays frozen while awake.
  ctx.check(world->movement_authority(body) == rt::MovementAuthority::None,
            "acceleration write returns the body to physics control");
  rt::Transform before{};
  ctx.check(world->get_transform(body, &before), "read pre-step transform");
  bool stepped = true;
  for (int i = 0; i < 3; ++i) {
    stepped = stepped && step_world(*world);
  }
  ctx.check(stepped, "physics steps run");
  rt::Transform after{};
  ctx.check(world->get_transform(body, &after), "read post-step transform");
  ctx.check(after.position.y < before.position.y,
            "woken body integrates gravity and falls");

  // Identical rewrite: no wake churn for a sleeping body.
  rigidBody->velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  rigidBody->sleeping = true;
  rigidBody->sleepFrameCount = 60U;
  ctx.check(sc::call_script_function("drop"), "no-op drop rewrite");
  ctx.check(rigidBody->sleeping && (rigidBody->sleepFrameCount == 60U),
            "identical acceleration rewrite leaves the body asleep");
  rigidBody->sleeping = false;
  rigidBody->sleepFrameCount = 0U;

  // Custom gravity: the requested total must be composed against it.
  ctx.check(sc::call_script_function("set_custom_gravity"), "set gravity");
  ctx.check(sc::call_script_function("request_zero_total"),
            "zero-total request");
  ctx.check(vec3_equals(rigidBody->acceleration, 0.0F, 20.0F, 0.0F),
            "zero total under (0,-20,0) gravity stores (0,20,0)");
  rigidBody->velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  bool steppedCustom = true;
  for (int i = 0; i < 2; ++i) {
    steppedCustom = steppedCustom && step_world(*world);
  }
  ctx.check(steppedCustom, "custom-gravity steps run");
  ctx.check(rigidBody->velocity.y == 0.0F,
            "requested zero total acceleration holds the body static");

  // set_additional_acceleration: wake on change, no churn, envelope clamp.
  rigidBody->velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  rigidBody->sleeping = true;
  rigidBody->sleepFrameCount = 60U;
  ctx.check(sc::call_script_function("push_additional"), "additional write");
  ctx.check(!rigidBody->sleeping && (rigidBody->sleepFrameCount == 0U),
            "additional acceleration change wakes the sleeping body");
  ctx.check(vec3_equals(rigidBody->acceleration, 5.0F, 0.0F, 0.0F),
            "additional acceleration stored directly");
  rigidBody->sleeping = true;
  rigidBody->sleepFrameCount = 60U;
  ctx.check(sc::call_script_function("push_additional"),
            "no-op additional rewrite");
  ctx.check(rigidBody->sleeping && (rigidBody->sleepFrameCount == 60U),
            "identical additional rewrite leaves the body asleep");
  ctx.check(sc::call_script_function("push_additional_huge"),
            "huge additional write");
  ctx.check(vec3_equals(rigidBody->acceleration, 500.0F, 0.0F, 0.0F),
            "additional acceleration clamps to the stable envelope");

  // Deferred path: a BeginPlay write queues, applies on flush with the
  // gravity captured at call time, and wakes the body then.
  ctx.check(sc::call_script_function("request_zero_total"),
            "reset to zero-total before deferred check");
  rigidBody->sleeping = true;
  rigidBody->sleepFrameCount = 60U;
  world->begin_begin_play_phase();
  ctx.check(sc::call_script_function("drop_custom"),
            "deferred drop call in BeginPlay");
  ctx.check(vec3_equals(rigidBody->acceleration, 0.0F, 20.0F, 0.0F) &&
                rigidBody->sleeping,
            "BeginPlay write stays queued until flush");
  world->end_begin_play_phase();
  sc::flush_deferred_mutations();
  ctx.check(vec3_equals(rigidBody->acceleration, 0.0F, 0.0F, 0.0F),
            "flushed drop applies the gravity-composed term");
  ctx.check(!rigidBody->sleeping && (rigidBody->sleepFrameCount == 0U),
            "flushed drop wakes the body");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("script_acceleration");
}
