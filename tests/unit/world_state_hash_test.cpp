// Pins what World::state_hash observes: two worlds built the same way hash
// the same, and each state the hash promises to cover (entity identity,
// transform bits, body velocity to the ulp, sleep state, timers, animation
// state) changes it when it changes, so a determinism divergence in any of
// them is visible.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/runtime/world.h"

namespace {

namespace rt = engine::runtime;

void timer_callback(rt::TimerId, void *) noexcept {}

/// One body with a transform, a rigid body and a sphere collider.
rt::Entity add_body(rt::World &world, float x, float velocityX) noexcept {
  rt::Transform transform{};
  transform.position = engine::math::Vec3(x, 1.0F, 0.0F);
  const rt::Entity entity = world.create_scene_object(transform);
  rt::RigidBody body{};
  body.velocity = engine::math::Vec3(velocityX, 0.0F, 0.0F);
  rt::Collider collider{};
  collider.shape = rt::ColliderShape::Sphere;
  if ((entity == rt::kInvalidEntity) || !world.add_rigid_body(entity, body) ||
      !world.add_collider(entity, collider)) {
    return rt::kInvalidEntity;
  }
  return entity;
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  auto a = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  auto b = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if ((a == nullptr) || (b == nullptr)) {
    std::printf("FAIL: world allocation\n");
    return 1;
  }

  ctx.check(a->state_hash() == b->state_hash(), "two empty worlds agree");
  const std::uint64_t emptyHash = a->state_hash();

  const rt::Entity bodyA = add_body(*a, 0.0F, 1.0F);
  const rt::Entity bodyB = add_body(*b, 0.0F, 1.0F);
  ctx.check((bodyA != rt::kInvalidEntity) && (bodyB != rt::kInvalidEntity),
            "bodies added");
  ctx.check(a->state_hash() != emptyHash, "an entity changes the hash");
  ctx.check(a->state_hash() == b->state_hash(),
            "two worlds built the same way agree");
  ctx.check(a->state_hash() == a->state_hash(),
            "the hash is a pure function of the state");

  // --- Velocity to the ulp ---
  {
    const std::uint64_t before = b->state_hash();
    rt::RigidBody *body = b->get_rigid_body_ptr(bodyB);
    ctx.check(body != nullptr, "body pointer");
    if (body != nullptr) {
      const float original = body->velocity.x;
      body->velocity.x = std::nextafter(original, 2.0F);
      ctx.check(b->state_hash() != before,
                "one ulp of velocity changes the hash");
      body->velocity.x = original;
      ctx.check(b->state_hash() == before, "restoring the velocity restores it");
    }
  }

  // --- Sleep state ---
  {
    const std::uint64_t before = b->state_hash();
    rt::RigidBody *body = b->get_rigid_body_ptr(bodyB);
    if (body != nullptr) {
      body->sleeping = true;
      ctx.check(b->state_hash() != before, "sleeping changes the hash");
      body->sleeping = false;
    }
  }

  // --- Transform bits and parent ---
  {
    const std::uint64_t before = b->state_hash();
    rt::Transform transform{};
    ctx.check(b->get_transform(bodyB, &transform), "read transform");
    transform.position.z = std::nextafter(transform.position.z, 1.0F);
    ctx.check(b->add_transform(bodyB, transform), "write transform");
    ctx.check(b->state_hash() != before, "one ulp of position changes the hash");
    transform.position.z = 0.0F;
    ctx.check(b->add_transform(bodyB, transform), "restore transform");
    ctx.check(b->state_hash() == before, "restoring the position restores it");
  }

  // --- Entity identity: the same content under a recycled slot differs ---
  {
    const std::uint64_t before = b->state_hash();
    ctx.check(b->destroy_entity(bodyB), "destroy body");
    const rt::Entity recycled = add_body(*b, 0.0F, 1.0F);
    ctx.check((recycled != rt::kInvalidEntity) &&
                  (recycled.index == bodyB.index) &&
                  (recycled.generation != bodyB.generation),
              "the slot was recycled with a new generation");
    ctx.check(b->state_hash() != before,
              "a recycled generation changes the hash");
  }

  // --- Timers ---
  {
    const std::uint64_t before = a->state_hash();
    const rt::TimerId timer =
        a->timer_manager().set_timeout(1.0F, &timer_callback, nullptr);
    ctx.check(timer != rt::kInvalidTimerId, "timer set");
    ctx.check(a->state_hash() != before, "an active timer changes the hash");
    const std::uint64_t armed = a->state_hash();
    static_cast<void>(a->timer_manager().tick(0.25F));
    ctx.check(a->state_hash() != armed, "timer time changes the hash");
    a->timer_manager().cancel(timer);
  }

  // --- Animation state ---
  {
    const rt::Entity actor = a->create_scene_object();
    rt::AnimationComponent animation{};
    ctx.check(a->add_animation_component(actor, animation), "add animation");
    const std::uint64_t before = a->state_hash();
    rt::AnimationComponent *live = a->get_animation_component_ptr(actor);
    ctx.check(live != nullptr, "animation pointer");
    if (live != nullptr) {
      live->stateTime = 0.5F;
      ctx.check(a->state_hash() != before, "animation time changes the hash");
      live->stateTime = 0.0F;
      live->currentState = 1U;
      ctx.check(a->state_hash() != before, "animation state changes the hash");
      live->currentState = 0U;
      live->paletteSlot = 3U;
      ctx.check(a->state_hash() == before,
                "a resolved renderer slot is not simulation state");
    }
  }

  return ctx.finish("world_state_hash");
}
