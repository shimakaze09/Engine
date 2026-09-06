// Verifies gamepad input through the production path: a headless
// engine::bootstrap initializes the platform's gamepad subsystem, a
// controller that arrives while the frame pipeline pumps the real event
// loop is opened by the platform and occupies an input slot, its button
// and axis edges reach the public input API and the Lua engine table's
// gamepad constants name them, and its removal frees the slot. The
// controller is SDL's virtual joystick, so no hardware is needed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

#include <SDL3/SDL.h>

#include "engine/core/input.h"
#include "engine/core/platform.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/scripting/scripting.h"
#include "../test_harness.h"

namespace {

constexpr const char *kScriptPath = "gamepad_hotplug_test.lua";

/// Walks upward from the current path until the bundled assets are found
/// and makes that directory current, since the pipeline loads its
/// bootstrap content from "assets/" (ctest starts in the build tree).
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};

  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// Writes the Lua fixture that reads the controller through the script API.
bool write_script_file() noexcept {
  const char *contents =
      "function south_is_down()\n"
      "    if engine.GAMEPAD_BUTTON_SOUTH ~= 0 then\n"
      "        error('GAMEPAD_BUTTON_SOUTH is not the south button code')\n"
      "    end\n"
      "    if engine.GAMEPAD_AXIS_LEFT_X ~= 0 or engine.GAMEPAD_AXIS_RIGHT_TRIGGER ~= 5 then\n"
      "        error('gamepad axis constants are misnumbered')\n"
      "    end\n"
      "    if not engine.is_gamepad_connected() then\n"
      "        error('script sees no gamepad')\n"
      "    end\n"
      "    if not engine.is_gamepad_button_down(engine.GAMEPAD_BUTTON_SOUTH) then\n"
      "        error('script does not see the south button down')\n"
      "    end\n"
      "    if engine.gamepad_axis_value(engine.GAMEPAD_AXIS_LEFT_X) <= 0 then\n"
      "        error('script does not see the left stick deflection')\n"
      "    end\n"
      "end\n";
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

/// Runs frames until the predicate holds or the frame budget is spent;
/// each frame pumps the real platform event loop once.
template <typename Predicate>
bool frames_until(engine::EnginePipeline &pipeline, Predicate predicate,
                  int maxFrames) noexcept {
  for (int i = 0; i < maxFrames; ++i) {
    if (!pipeline.execute_frame()) {
      return false;
    }
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  if (!set_working_directory_with_assets()) {
    ctx.fail("bundled assets located");
    return ctx.finish("gamepad_hotplug");
  }
  static_cast<void>(std::remove(kScriptPath));
  if (!write_script_file()) {
    ctx.fail("fixture written");
    return ctx.finish("gamepad_hotplug");
  }

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    ctx.fail("headless bootstrap");
    static_cast<void>(std::remove(kScriptPath));
    return ctx.finish("gamepad_hotplug");
  }
  ctx.check(engine::core::platform_gamepads_available(),
            "bootstrap initialized the gamepad subsystem");

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    ctx.fail("pipeline initialize");
    engine::shutdown();
    static_cast<void>(std::remove(kScriptPath));
    return ctx.finish("gamepad_hotplug");
  }

  ctx.check(!engine::core::is_gamepad_connected(),
            "no controller before one arrives");

  // A virtual controller arrives: the next frames pump its arrival, the
  // platform opens it, and the input slot follows.
  SDL_VirtualJoystickDesc desc{};
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = 6;
  desc.nbuttons = 15;
  desc.name = "engine virtual gamepad";
  const SDL_JoystickID virtualId = SDL_AttachVirtualJoystick(&desc);
  ctx.check(virtualId != 0U, "virtual controller attached");

  ctx.check(frames_until(
                pipeline,
                []() noexcept { return engine::core::is_gamepad_connected(); },
                20),
            "the pumped arrival connects the controller");
  ctx.check(engine::core::connected_gamepad_count() == 1,
            "exactly one controller slot in use");

  // Button and axis edges through the opened device.
  SDL_Joystick *joystick = SDL_GetJoystickFromID(virtualId);
  ctx.check(joystick != nullptr, "virtual joystick handle");
  if (joystick != nullptr) {
    ctx.check(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH,
                                           true),
              "press south");
    ctx.check(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX,
                                         20000),
              "deflect left stick");
  }
  ctx.check(frames_until(
                pipeline,
                []() noexcept {
                  return engine::core::is_gamepad_button_down(
                             engine::core::kGamepadButton_South) &&
                         (engine::core::gamepad_axis_value(
                              engine::core::kGamepadAxis_LeftX) > 0.0F);
                },
                20),
            "button and axis edges reach the input API");

  // The same state through the Lua API and its named constants.
  ctx.check(engine::scripting::load_script(kScriptPath), "script loaded");
  ctx.check(engine::scripting::call_script_function("south_is_down"),
            "script reads the controller through named constants");

  if (joystick != nullptr) {
    ctx.check(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH,
                                           false),
              "release south");
  }
  ctx.check(frames_until(
                pipeline,
                []() noexcept {
                  return !engine::core::is_gamepad_button_down(
                      engine::core::kGamepadButton_South);
                },
                20),
            "the release edge reaches the input API");

  // Removal frees the slot.
  ctx.check(SDL_DetachVirtualJoystick(virtualId), "virtual controller detached");
  ctx.check(frames_until(
                pipeline,
                []() noexcept { return !engine::core::is_gamepad_connected(); },
                20),
            "the pumped removal disconnects the controller");
  ctx.check(engine::core::connected_gamepad_count() == 0,
            "no controller slot in use after removal");

  pipeline.teardown();
  engine::shutdown();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("gamepad_hotplug");
}
