// Integration tests for the Lua coroutine scheduler (P1-M2-F).
// Tests: wait(seconds), wait_frames(n), wait_until(condition),
//        error handling, and clear.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

static const char *kTempScript = "coroutine_test.lua";

bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kTempScript, "w") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kTempScript, "w");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fputs(code, f);
  std::fclose(f);
  return true;
}

void remove_script() noexcept { std::remove(kTempScript); }

// Helper: count entities named |name| in the world.
int count_named(engine::runtime::World *w, const char *name) noexcept {
  int n = 0;
  w->for_each_alive([&](engine::runtime::Entity ent) noexcept {
    engine::runtime::NameComponent nc{};
    if (w->get_name_component(ent, &nc) &&
        std::strcmp(nc.name, name) == 0) {
      ++n;
    }
  });
  return n;
}

// -----------------------------------------------------------------------
// 1. wait(0.5) then set flag — verify at correct frame
// -----------------------------------------------------------------------
bool test_wait_seconds() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());
  engine::scripting::set_default_mesh_asset_id(1U);

  const char *script =
      "function on_start()\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait(0.5)\n"
      "    local e = engine.spawn_entity()\n"
      "    engine.set_name(e, 'wait_done')\n"
      "  end)\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }
  engine::scripting::set_frame_time(0.0F, 0.0F);
  engine::scripting::set_frame_index(0U);
  engine::scripting::call_script_function("on_start");

  // Not woken yet (only 0.0s elapsed).
  if (count_named(world.get(), "wait_done") != 0) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  // Tick at t=0.3 — still too early.
  engine::scripting::set_frame_time(0.3F, 0.3F);
  engine::scripting::set_frame_index(1U);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "wait_done") != 0) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  // Tick at t=0.6 — should wake.
  engine::scripting::set_frame_time(0.3F, 0.6F);
  engine::scripting::set_frame_index(2U);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "wait_done") != 1) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::shutdown_scripting();
  remove_script();
  return true;
}

// -----------------------------------------------------------------------
// 2. wait_frames(3) — entity created after 3 frames
// -----------------------------------------------------------------------
bool test_wait_frames() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());
  engine::scripting::set_default_mesh_asset_id(1U);

  const char *script =
      "function on_start()\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait_frames(3)\n"
      "    local e = engine.spawn_entity()\n"
      "    engine.set_name(e, 'frames_done')\n"
      "  end)\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  // Frame 0: start coroutine. wakeAtFrame = 0 + 3 = 3.
  engine::scripting::set_frame_time(0.016F, 0.0F);
  engine::scripting::set_frame_index(0U);
  engine::scripting::call_script_function("on_start");

  // Frames 1, 2 — not ready yet.
  for (std::uint32_t f = 1U; f <= 2U; ++f) {
    engine::scripting::set_frame_index(f);
    engine::scripting::set_frame_time(0.016F, 0.016F * static_cast<float>(f));
    engine::scripting::tick_coroutines();
    if (count_named(world.get(), "frames_done") != 0) {
      engine::scripting::shutdown_scripting();
      remove_script();
      return false;
    }
  }

  // Frame 3 — should wake.
  engine::scripting::set_frame_index(3U);
  engine::scripting::set_frame_time(0.016F, 0.048F);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "frames_done") != 1) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::shutdown_scripting();
  remove_script();
  return true;
}

// -----------------------------------------------------------------------
// 3. wait_until(condition) — resumes when condition returns true
// -----------------------------------------------------------------------
bool test_wait_until() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());
  engine::scripting::set_default_mesh_asset_id(1U);

  // The condition checks a global flag. We set it from Lua after a few ticks.
  const char *script =
      "my_flag = false\n"
      "function on_start()\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait_until(function() return my_flag end)\n"
      "    local e = engine.spawn_entity()\n"
      "    engine.set_name(e, 'cond_done')\n"
      "  end)\n"
      "end\n"
      "function set_flag()\n"
      "  my_flag = true\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::set_frame_time(0.0F, 0.0F);
  engine::scripting::set_frame_index(0U);
  engine::scripting::call_script_function("on_start");

  // Tick a few times — flag is false, should NOT wake.
  for (int i = 1; i <= 3; ++i) {
    engine::scripting::set_frame_index(static_cast<std::uint32_t>(i));
    engine::scripting::set_frame_time(0.016F, 0.016F * static_cast<float>(i));
    engine::scripting::tick_coroutines();
  }
  if (count_named(world.get(), "cond_done") != 0) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  // Set the flag and tick again — should wake.
  engine::scripting::call_script_function("set_flag");
  engine::scripting::set_frame_index(4U);
  engine::scripting::set_frame_time(0.016F, 0.064F);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "cond_done") != 1) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::shutdown_scripting();
  remove_script();
  return true;
}

// -----------------------------------------------------------------------
// 4. Chained waits — a coroutine that uses wait + wait_frames in sequence
// -----------------------------------------------------------------------
bool test_chained_waits() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());
  engine::scripting::set_default_mesh_asset_id(1U);

  const char *script =
      "function on_start()\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait(0.1)\n"
      "    local e1 = engine.spawn_entity()\n"
      "    engine.set_name(e1, 'chain_step1')\n"
      "    engine.wait_frames(2)\n"
      "    local e2 = engine.spawn_entity()\n"
      "    engine.set_name(e2, 'chain_step2')\n"
      "  end)\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::set_frame_time(0.0F, 0.0F);
  engine::scripting::set_frame_index(0U);
  engine::scripting::call_script_function("on_start");

  // Tick at t=0.15 (frame 1) — wait(0.1) should wake, step1 created,
  // then immediately re-yields on wait_frames(2), wakeAtFrame = 1 + 2 = 3.
  engine::scripting::set_frame_index(1U);
  engine::scripting::set_frame_time(0.15F, 0.15F);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "chain_step1") != 1) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }
  if (count_named(world.get(), "chain_step2") != 0) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  // Frame 2 — not ready.
  engine::scripting::set_frame_index(2U);
  engine::scripting::set_frame_time(0.016F, 0.166F);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "chain_step2") != 0) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  // Frame 3 — should wake.
  engine::scripting::set_frame_index(3U);
  engine::scripting::set_frame_time(0.016F, 0.182F);
  engine::scripting::tick_coroutines();
  if (count_named(world.get(), "chain_step2") != 1) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::shutdown_scripting();
  remove_script();
  return true;
}

// -----------------------------------------------------------------------
// 5. Error in coroutine — faulted coroutine is removed, doesn't crash
// -----------------------------------------------------------------------
bool test_error_handling() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());
  engine::scripting::set_default_mesh_asset_id(1U);

  const char *script =
      "function on_start()\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait(0.1)\n"
      "    error('intentional test error')\n"
      "  end)\n"
      "  -- Second coroutine should survive the first's error.\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait(0.1)\n"
      "    local e = engine.spawn_entity()\n"
      "    engine.set_name(e, 'survivor')\n"
      "  end)\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::set_frame_time(0.0F, 0.0F);
  engine::scripting::set_frame_index(0U);
  engine::scripting::call_script_function("on_start");

  // Wake both coroutines.
  engine::scripting::set_frame_index(1U);
  engine::scripting::set_frame_time(0.2F, 0.2F);
  engine::scripting::tick_coroutines();

  // The errored coroutine should be cleaned up, the survivor should succeed.
  if (count_named(world.get(), "survivor") != 1) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::shutdown_scripting();
  remove_script();
  return true;
}

// -----------------------------------------------------------------------
// 6. clear_coroutines — all pending coroutines are discarded
// -----------------------------------------------------------------------
bool test_clear() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());
  engine::scripting::set_default_mesh_asset_id(1U);

  const char *script =
      "function on_start()\n"
      "  engine.start_coroutine(function()\n"
      "    engine.wait(10.0)\n"
      "    local e = engine.spawn_entity()\n"
      "    engine.set_name(e, 'should_not_exist')\n"
      "  end)\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::set_frame_time(0.0F, 0.0F);
  engine::scripting::set_frame_index(0U);
  engine::scripting::call_script_function("on_start");

  // Clear before wake time.
  engine::scripting::clear_coroutines();

  // Advance past wake time and tick — it should NOT fire.
  engine::scripting::set_frame_time(11.0F, 11.0F);
  engine::scripting::set_frame_index(1U);
  engine::scripting::tick_coroutines();

  if (count_named(world.get(), "should_not_exist") != 0) {
    engine::scripting::shutdown_scripting();
    remove_script();
    return false;
  }

  engine::scripting::shutdown_scripting();
  remove_script();
  return true;
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    bool (*fn)() noexcept;
  };

  const TestCase tests[] = {
      {"wait_seconds", test_wait_seconds},
      {"wait_frames", test_wait_frames},
      {"wait_until", test_wait_until},
      {"chained_waits", test_chained_waits},
      {"error_handling", test_error_handling},
      {"clear_coroutines", test_clear},
  };

  int failures = 0;
  for (const auto &t : tests) {
    const bool ok = t.fn();
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
    if (!ok) {
      ++failures;
    }
  }
  return failures;
}
