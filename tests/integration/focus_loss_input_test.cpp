// Regression for #312's focus-loss criterion: a key held when the window
// loses focus must read as released, not stay down until it happens to be
// pressed again. Before the fix nothing handled SDL_EVENT_WINDOW_FOCUS_LOST,
// so Alt-Tab while holding a movement key left the character walking.
//
// Drives the production pump: events are pushed into SDL's queue and the
// pipeline's input stage polls, routes and decodes them, so the test covers
// the routing as well as the handler -- a focus event the pipeline swallowed
// before it reached input would fail here.

#include "engine/core/input.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found.
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
        std::filesystem::exists(
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

bool push_key(SDL_Scancode scancode, bool down) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.scancode = scancode;
  event.key.down = down;
  return SDL_PushEvent(&event);
}

bool push_mouse_button(std::uint8_t button, bool down) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.button = button;
  event.button.down = down;
  return SDL_PushEvent(&event);
}

// A made-up instance id: the pipeline attaches a slot on GAMEPAD_ADDED
// whether or not a real device answers to it, which is all the release
// needs to be observed.
constexpr SDL_JoystickID kFakePad = 4242;

bool push_gamepad_added() noexcept {
  SDL_Event event{};
  event.type = SDL_EVENT_GAMEPAD_ADDED;
  event.gdevice.which = kFakePad;
  return SDL_PushEvent(&event);
}

bool push_gamepad_button(std::uint8_t button, bool down) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN
                    : SDL_EVENT_GAMEPAD_BUTTON_UP;
  event.gbutton.which = kFakePad;
  event.gbutton.button = button;
  event.gbutton.down = down;
  return SDL_PushEvent(&event);
}

bool push_gamepad_axis(std::uint8_t axis, std::int16_t value) noexcept {
  SDL_Event event{};
  event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  event.gaxis.which = kFakePad;
  event.gaxis.axis = axis;
  event.gaxis.value = value;
  return SDL_PushEvent(&event);
}

bool push_focus_lost() noexcept {
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  return SDL_PushEvent(&event);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }

  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0),
          "frame delta override");
    CHECK(pipeline.execute_frame(), "settle frame");

    constexpr engine::core::KeyScancode kW =
        static_cast<engine::core::KeyScancode>(SDL_SCANCODE_W);

    // A controller, so the gamepad release has something to act on.
    CHECK(push_gamepad_added(), "push gamepad added");
    CHECK(pipeline.execute_frame(), "the attach frame");
    CHECK(engine::core::is_gamepad_connected(0), "the controller is attached");

    // Hold W, the left mouse button, the south face button and the left
    // stick hard left, and prove all four arrived: without this the
    // release checks below would pass on input never pressed.
    CHECK(push_key(SDL_SCANCODE_W, true), "push W down");
    CHECK(push_mouse_button(SDL_BUTTON_LEFT, true), "push left button down");
    CHECK(push_gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, true),
          "push south button down");
    CHECK(push_gamepad_axis(SDL_GAMEPAD_AXIS_LEFTX, -32000),
          "push left stick left");
    CHECK(pipeline.execute_frame(), "the press frame");
    CHECK(engine::core::is_key_down(kW), "W is held");
    CHECK(engine::core::is_mouse_button_down(0), "the left button is held");
    CHECK(engine::core::is_gamepad_button_down(
              engine::core::kGamepadButton_South, 0),
          "the south button is held");
    CHECK(engine::core::gamepad_axis_value(engine::core::kGamepadAxis_LeftX,
                                           8000, 0) < -0.5F,
          "the left stick is held left");

    // Still held a frame later with no new events -- held state persists,
    // which is what makes a missed release stick.
    CHECK(pipeline.execute_frame(), "a quiet frame");
    CHECK(engine::core::is_key_down(kW), "W stays held with no events");

    // The window loses focus. No key-up and no button-up will ever arrive
    // for these; the release has to come from the focus change.
    CHECK(push_focus_lost(), "push focus lost");
    CHECK(pipeline.execute_frame(), "the focus-lost frame");
    const bool keyReleased = !engine::core::is_key_down(kW);
    CHECK(keyReleased, "W reads released after focus loss");
    CHECK(engine::core::is_key_released(kW),
          "the release is an edge this frame, so release handlers run");
    CHECK(!engine::core::is_mouse_button_down(0),
          "the left button reads released after focus loss");
    CHECK(!engine::core::is_gamepad_button_down(
              engine::core::kGamepadButton_South, 0),
          "the south button reads released after focus loss");
    CHECK(engine::core::gamepad_axis_value(engine::core::kGamepadAxis_LeftX,
                                           8000, 0) == 0.0F,
          "the left stick reads centred after focus loss");
    CHECK(engine::core::is_gamepad_connected(0),
          "the controller is still attached -- focus loss releases input, "
          "it does not unplug anything");
    if (!keyReleased) {
      result = 4;
    }

    // And the edge is one frame, not a latch.
    CHECK(pipeline.execute_frame(), "the frame after");
    CHECK(!engine::core::is_key_released(kW),
          "the release edge lasts one frame");

    pipeline.teardown();
  }
  engine::shutdown();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return (result != 0) ? result : 1;
  }
  std::puts("focus_loss_input_test passed");
  return 0;
}
