// A press and release that land in the same frame's event batch must still
// read as a press (#653). Edges used to be derived by comparing the state
// at the end of this frame with the state at the end of the last one; a
// tap shorter than a frame is up at both ends, so the comparison saw
// nothing and is_key_pressed never fired. At 20 frames a second, a 40 ms
// tap -- an ordinary quick press -- was simply lost.
//
// Drives the production pump, as focus_loss_input_test does: native events
// pushed into the queue, polled, routed and decoded by the pipeline.

#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"

#include <SDL3/SDL.h>

#include <cstdint>
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

bool push_key(SDL_Scancode scancode, bool down, bool repeat) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.scancode = scancode;
  event.key.down = down;
  event.key.repeat = repeat;
  return SDL_PushEvent(&event);
}

bool push_mouse_button(bool down) noexcept {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.down = down;
  return SDL_PushEvent(&event);
}

// A made-up instance id: the pipeline attaches a slot on GAMEPAD_ADDED
// whether or not a real device answers to it.
constexpr SDL_JoystickID kFakePad = 4243;

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

int g_actionPresses = 0;
int g_actionReleases = 0;

void count_action(const char * /*name*/, bool pressed,
                  void * /*userData*/) noexcept {
  if (pressed) {
    ++g_actionPresses;
  } else {
    ++g_actionReleases;
  }
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
      pipeline.teardown();
      engine::shutdown();
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0), "delta override");
    CHECK(pipeline.execute_frame(), "settle frame");

    constexpr engine::core::KeyScancode kW =
        static_cast<engine::core::KeyScancode>(SDL_SCANCODE_W);

    // --- A tap: down and up in one batch.
    CHECK(push_key(SDL_SCANCODE_W, true, false), "push W down");
    CHECK(push_key(SDL_SCANCODE_W, false, false), "push W up");
    CHECK(pipeline.execute_frame(), "the tap frame");
    const bool tapSeen = engine::core::is_key_pressed(kW);
    CHECK(tapSeen, "a tap inside one frame reads as pressed");
    CHECK(engine::core::is_key_released(kW),
          "and as released, so press-and-release handlers both run");
    CHECK(!engine::core::is_key_down(kW),
          "but not as held: it is up at the end of the frame");
    if (!tapSeen) {
      result = 4;
    }

    // Edges last one frame.
    CHECK(pipeline.execute_frame(), "the frame after the tap");
    CHECK(!engine::core::is_key_pressed(kW), "the press edge does not linger");
    CHECK(!engine::core::is_key_released(kW),
          "the release edge does not linger");

    // --- Key auto-repeat is not a new press.
    CHECK(push_key(SDL_SCANCODE_W, true, false), "push W down to hold");
    CHECK(pipeline.execute_frame(), "the hold frame");
    CHECK(engine::core::is_key_pressed(kW), "the first down is a press");
    CHECK(push_key(SDL_SCANCODE_W, true, true), "push an OS repeat");
    CHECK(pipeline.execute_frame(), "the repeat frame");
    CHECK(engine::core::is_key_down(kW), "still held through the repeat");
    CHECK(!engine::core::is_key_pressed(kW),
          "an auto-repeat is not a second press");
    CHECK(push_key(SDL_SCANCODE_W, false, false), "release W");
    CHECK(pipeline.execute_frame(), "the release frame");
    CHECK(engine::core::is_key_released(kW), "the release is an edge");

    // --- The same for a mouse click.
    CHECK(push_mouse_button(true), "push click down");
    CHECK(push_mouse_button(false), "push click up");
    CHECK(pipeline.execute_frame(), "the click frame");
    CHECK(engine::core::is_mouse_button_pressed(0),
          "a click inside one frame reads as pressed");
    CHECK(!engine::core::is_mouse_button_down(0), "and is up afterwards");

    // --- And a gamepad button.
    CHECK(push_gamepad_added(), "push gamepad added");
    CHECK(pipeline.execute_frame(), "the attach frame");
    constexpr auto kSouth = static_cast<std::uint8_t>(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(push_gamepad_button(kSouth, true), "push south down");
    CHECK(push_gamepad_button(kSouth, false), "push south up");
    CHECK(pipeline.execute_frame(), "the button tap frame");
    CHECK(engine::core::is_gamepad_button_pressed(
              engine::core::kGamepadButton_South),
          "a gamepad tap inside one frame reads as pressed");
    CHECK(!engine::core::is_gamepad_button_down(
              engine::core::kGamepadButton_South),
          "and is up afterwards");
    CHECK(pipeline.execute_frame(), "the frame after the button tap");
    CHECK(!engine::core::is_gamepad_button_pressed(
              engine::core::kGamepadButton_South),
          "the gamepad press edge does not linger");

    // --- And through the action mapper, which authors bind to.
    engine::core::InputBinding binding{};
    binding.type = engine::core::InputBindingType::Key;
    binding.code = kW;
    CHECK(engine::core::add_input_action("jump", &binding, 1U),
          "register a jump action on W");
    CHECK(engine::core::set_action_callback("jump", &count_action, nullptr),
          "watch the jump action");
    CHECK(push_key(SDL_SCANCODE_W, true, false), "tap W for jump: down");
    CHECK(push_key(SDL_SCANCODE_W, false, false), "tap W for jump: up");
    CHECK(pipeline.execute_frame(), "the jump tap frame");
    CHECK(g_actionPresses == 1, "a tapped action fires its press once");
    CHECK(pipeline.execute_frame(), "the frame after the jump tap");
    CHECK(g_actionReleases == 1, "and its release on the next frame");

    pipeline.teardown();
  }
  engine::shutdown();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return (result != 0) ? result : 1;
  }
  std::puts("input_tap_edge_test passed");
  return 0;
}
