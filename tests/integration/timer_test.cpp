// Verifies timer test behavior for the Engine test suite.

#include <cstdio>
#include <limits>
#include <memory>
#include <new>

#include "engine/runtime/timer_manager.h"
#include "engine/runtime/world.h"

namespace {

// Counters mutated by timer callbacks.
int g_timeoutFired = 0;
int g_intervalFired = 0;
int g_cancelTargetFired = 0;

void on_timeout(engine::runtime::TimerId /*id*/, void * /*ud*/) noexcept {
  ++g_timeoutFired;
}

void on_interval(engine::runtime::TimerId /*id*/, void * /*ud*/) noexcept {
  ++g_intervalFired;
}

void on_cancel_target(engine::runtime::TimerId /*id*/, void * /*ud*/) noexcept {
  ++g_cancelTargetFired;
}

bool test_timeout_fires_once() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();

  g_timeoutFired = 0;
  const auto id = tm.set_timeout(0.5F, on_timeout, nullptr);
  if (id == engine::runtime::kInvalidTimerId) {
    return false;
  }
  if (tm.active_count() != 1U) {
    return false;
  }

  // Tick 0.3s — should NOT fire.
  tm.tick(0.3F);
  if (g_timeoutFired != 0) {
    return false;
  }

  // Tick 0.3s more — total 0.6s, past 0.5s.
  tm.tick(0.3F);
  if (g_timeoutFired != 1) {
    return false;
  }

  // One-shot should be gone now.
  if (tm.active_count() != 0U) {
    return false;
  }

  tm.tick(1.0F);
  if (g_timeoutFired != 1) {
    return false;
  }

  return true;
}

bool test_interval_fires_repeatedly() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();

  g_intervalFired = 0;
  const auto id = tm.set_interval(0.25F, on_interval, nullptr);
  if (id == engine::runtime::kInvalidTimerId) {
    return false;
  }

  // Tick 0.1s — not yet.
  tm.tick(0.1F);
  if (g_intervalFired != 0) {
    return false;
  }

  // Tick 0.2s — total 0.3s, past first 0.25s.
  tm.tick(0.2F);
  if (g_intervalFired != 1) {
    return false;
  }

  // Tick 0.25s — total 0.55s, past second 0.5s.
  tm.tick(0.25F);
  if (g_intervalFired != 2) {
    return false;
  }

  // Cancel.
  tm.cancel(id);
  tm.tick(1.0F);
  if (g_intervalFired != 2) {
    return false;
  }

  return true;
}

bool test_cancel_prevents_fire() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();

  g_cancelTargetFired = 0;
  const auto id = tm.set_timeout(0.5F, on_cancel_target, nullptr);
  if (id == engine::runtime::kInvalidTimerId) {
    return false;
  }

  tm.cancel(id);
  tm.tick(1.0F);
  if (g_cancelTargetFired != 0) {
    return false;
  }

  return true;
}

bool test_stale_timer_ids_do_not_cancel_reused_slots() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();

  g_timeoutFired = 0;
  g_cancelTargetFired = 0;

  const auto cancelledId = tm.set_timeout(1.0F, on_timeout, nullptr);
  if (cancelledId == engine::runtime::kInvalidTimerId) {
    return false;
  }
  tm.cancel(cancelledId);

  const auto replacementAfterCancel =
      tm.set_timeout(0.1F, on_cancel_target, nullptr);
  if ((replacementAfterCancel == engine::runtime::kInvalidTimerId) ||
      (replacementAfterCancel == cancelledId)) {
    return false;
  }
  tm.cancel(cancelledId);
  tm.tick(0.2F);
  if ((g_timeoutFired != 0) || (g_cancelTargetFired != 1)) {
    return false;
  }

  g_timeoutFired = 0;
  g_cancelTargetFired = 0;

  const auto firedId = tm.set_timeout(0.1F, on_timeout, nullptr);
  if (firedId == engine::runtime::kInvalidTimerId) {
    return false;
  }
  tm.tick(0.2F);
  if (g_timeoutFired != 1) {
    return false;
  }

  const auto replacementAfterFire =
      tm.set_timeout(0.1F, on_cancel_target, nullptr);
  if ((replacementAfterFire == engine::runtime::kInvalidTimerId) ||
      (replacementAfterFire == firedId)) {
    return false;
  }
  tm.cancel(firedId);
  tm.tick(0.2F);
  return g_cancelTargetFired == 1;
}

bool test_clear_removes_all() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();

  g_timeoutFired = 0;
  g_intervalFired = 0;
  tm.set_timeout(0.1F, on_timeout, nullptr);
  tm.set_interval(0.1F, on_interval, nullptr);

  if (tm.active_count() != 2U) {
    return false;
  }

  tm.clear();

  if (tm.active_count() != 0U) {
    return false;
  }

  tm.tick(1.0F);
  if ((g_timeoutFired != 0) || (g_intervalFired != 0)) {
    return false;
  }

  return true;
}

bool test_null_callback_rejected() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();
  const auto id1 = tm.set_timeout(1.0F, nullptr, nullptr);
  const auto id2 = tm.set_interval(1.0F, nullptr, nullptr);
  if ((id1 != engine::runtime::kInvalidTimerId) ||
      (id2 != engine::runtime::kInvalidTimerId)) {
    return false;
  }
  return true;
}

/// Non-finite timings are rejected by the live setters: NaN passes the
/// old `<= 0` interval guard and would fire on every tick forever, Inf
/// would never fire and leak its slot.
bool test_non_finite_timings_rejected() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  auto &tm = world->timer_manager();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  if (tm.set_timeout(nan, on_timeout, nullptr) !=
      engine::runtime::kInvalidTimerId) {
    return false;
  }
  if (tm.set_timeout(inf, on_timeout, nullptr) !=
      engine::runtime::kInvalidTimerId) {
    return false;
  }
  if (tm.set_interval(nan, on_interval, nullptr) !=
      engine::runtime::kInvalidTimerId) {
    return false;
  }
  if (tm.set_interval(inf, on_interval, nullptr) !=
      engine::runtime::kInvalidTimerId) {
    return false;
  }
  return tm.active_count() == 0U;
}

bool test_timer_per_world() noexcept {
  // Two separate worlds have independent timer managers.
  std::unique_ptr<engine::runtime::World> worldA(new (std::nothrow)
                                                     engine::runtime::World());
  std::unique_ptr<engine::runtime::World> worldB(new (std::nothrow)
                                                     engine::runtime::World());
  if ((worldA == nullptr) || (worldB == nullptr)) {
    return false;
  }

  g_timeoutFired = 0;
  g_intervalFired = 0;

  worldA->timer_manager().set_timeout(0.1F, on_timeout, nullptr);
  worldB->timer_manager().set_timeout(0.1F, on_interval, nullptr);

  worldA->timer_manager().tick(0.2F);
  if ((g_timeoutFired != 1) || (g_intervalFired != 0)) {
    return false;
  }

  worldB->timer_manager().tick(0.2F);
  if ((g_timeoutFired != 1) || (g_intervalFired != 1)) {
    return false;
  }

  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int failures = 0;

  const auto run = [&failures](const char *name, bool (*fn)() noexcept) {
    if (!fn()) {
      std::printf("FAIL: %s\n", name);
      ++failures;
    } else {
      std::printf("PASS: %s\n", name);
    }
  };

  run("test_timeout_fires_once", test_timeout_fires_once);
  run("test_interval_fires_repeatedly", test_interval_fires_repeatedly);
  run("test_cancel_prevents_fire", test_cancel_prevents_fire);
  run("test_stale_timer_ids_do_not_cancel_reused_slots",
      test_stale_timer_ids_do_not_cancel_reused_slots);
  run("test_clear_removes_all", test_clear_removes_all);
  run("test_null_callback_rejected", test_null_callback_rejected);
  run("test_non_finite_timings_rejected", test_non_finite_timings_rejected);
  run("test_timer_per_world", test_timer_per_world);

  if (failures > 0) {
    std::printf("\n%d test(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nAll timer tests passed.\n");
  return 0;
}
