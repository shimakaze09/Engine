// Integration test for generated Lua bindings (P1-M2-G4f).
// Verifies generated_bindings.cpp compiles and generated wrappers work at
// runtime.

#include <cstdio>
#include <memory>
#include <new>

#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

static const char *kTempScript = "bindgen_test.lua";

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

bool test_generated_bindings() noexcept {
  if (!engine::scripting::initialize_scripting()) {
    return false;
  }

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    engine::scripting::shutdown_scripting();
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());

  engine::scripting::set_frame_time(0.016F, 1.0F);
  engine::scripting::set_frame_index(60U);

  const char *script =
      "function run_generated_bindings()\n"
      "  local dt = engine.delta_time()\n"
      "  local et = engine.elapsed_time()\n"
      "  local fc = engine.frame_count()\n"
      "  if type(dt) ~= 'number' or dt < 0 then error('delta_time bad') end\n"
      "  if type(et) ~= 'number' or et < 0 then error('elapsed_time bad') end\n"
      "  if type(fc) ~= 'number' or fc < 0 then error('frame_count bad') end\n"
      "\n"
      "  local count = engine.get_entity_count()\n"
      "  if type(count) ~= 'number' or count < 0 then error('entity_count "
      "bad') end\n"
      "\n"
      "  if type(engine.is_god_mode()) ~= 'boolean' then error('god_mode "
      "type') end\n"
      "  if type(engine.is_noclip()) ~= 'boolean' then error('noclip type') "
      "end\n"
      "  if type(engine.is_gamepad_connected()) ~= 'boolean' then "
      "error('gamepad type') end\n"
      "\n"
      "  if type(engine.is_key_down(0)) ~= 'boolean' then error('is_key_down "
      "type') end\n"
      "  if type(engine.is_key_pressed(0)) ~= 'boolean' then "
      "error('is_key_pressed type') end\n"
      "  if type(engine.is_gamepad_button_down(0)) ~= 'boolean' then "
      "error('pad_button type') end\n"
      "  if type(engine.is_action_down('move')) ~= 'boolean' then "
      "error('action_down type') end\n"
      "  if type(engine.is_action_pressed('move')) ~= 'boolean' then "
      "error('action_pressed type') end\n"
      "  if type(engine.get_action_value('move')) ~= 'number' then "
      "error('action_value type') end\n"
      "  if type(engine.get_axis_value('move')) ~= 'number' then "
      "error('axis_value type') end\n"
      "\n"
      "  if not engine.set_game_mode('test_mode') then error('set_game_mode "
      "fail') end\n"
      "  if type(engine.get_game_mode()) ~= 'string' then error('get_game_mode "
      "type') end\n"
      "  if not engine.set_game_state('test_state') then error('set_game_state "
      "fail') end\n"
      "  if engine.get_game_state() ~= 'test_state' then error('game_state "
      "mismatch') end\n"
      "\n"
      "  if type(engine.is_alive(1)) ~= 'boolean' then error('is_alive type') "
      "end\n"
      "  if type(engine.has_light(1)) ~= 'boolean' then error('has_light "
      "type') end\n"
      "\n"
      "  engine.set_camera_fov(75.0)\n"
      "  engine.set_master_volume(0.5)\n"
      "  engine.stop_all_sounds()\n"
      "end\n";

  if (!write_script(script) || !engine::scripting::load_script(kTempScript)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  const bool ok =
      engine::scripting::call_script_function("run_generated_bindings");

  remove_script();
  engine::scripting::shutdown_scripting();
  return ok;
}

} // namespace

int main() {
  std::printf("  bindgen_test::generated_bindings ... ");
  const bool ok = test_generated_bindings();
  std::printf(ok ? "PASS\n" : "FAIL\n");
  return ok ? 0 : 1;
}
