// Verifies engine pipeline test behavior for the Engine test suite.

#include "engine/core/input.h"
#include "engine/math/transform.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "spatial_transform_util.h"

#include <cmath>

#include <SDL3/SDL.h>

namespace {

engine::runtime::World *g_lastEditorWorld = nullptr;
bool g_editorCaptureKeyboard = false;
bool g_editorCaptureMouse = false;
int g_editorProcessedEvents = 0;

/// Captures editor bridge world binding for pipeline cleanup tests.
void capture_editor_world(engine::runtime::World *world) noexcept {
  g_lastEditorWorld = world;
}

/// Records an editor event and begins keyboard capture.
void process_editor_event_and_capture_keyboard(void * /*sdlEvent*/) noexcept {
  ++g_editorProcessedEvents;
  g_editorCaptureKeyboard = true;
}

/// Records an editor event and begins mouse capture.
void process_editor_event_and_capture_mouse(void * /*sdlEvent*/) noexcept {
  ++g_editorProcessedEvents;
  g_editorCaptureMouse = true;
}

/// Records an editor event without requesting capture.
void process_editor_event_without_capture(void * /*sdlEvent*/) noexcept {
  ++g_editorProcessedEvents;
}

/// Returns the current keyboard capture state for input routing tests.
bool wants_keyboard_capture() noexcept { return g_editorCaptureKeyboard; }
/// Returns the current mouse capture state for input routing tests.
bool wants_mouse_capture() noexcept { return g_editorCaptureMouse; }
/// Returns no capture for input routing tests.
bool wants_no_capture() noexcept { return false; }

/// Returns whether two spatial-test scalar values match tightly.
bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

/// Verifies component-local directions inherit world rotation without having
/// their authored magnitude changed.
int check_local_direction_rotation() {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Quat rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 0.0F, 1.0F), kHalfPi);
  const engine::math::Vec3 direction =
      engine::runtime::detail::rotate_local_direction(
          rotation, engine::math::Vec3(2.0F, 0.0F, 0.0F));

  return nearly_equal(direction.x, 0.0F) && nearly_equal(direction.y, 2.0F) &&
                 nearly_equal(direction.z, 0.0F)
             ? 0
             : 1;
}

/// Verifies conservative AABB extents include world rotation, non-uniform
/// scale, and negative scale signs.
int check_transformed_aabb_half_extents() {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Mat4 world = engine::math::compose_trs(
      engine::math::Vec3(10.0F, 20.0F, 30.0F),
      engine::math::from_axis_angle(engine::math::Vec3(0.0F, 0.0F, 1.0F),
                                    kHalfPi),
      engine::math::Vec3(-2.0F, 3.0F, 4.0F));
  const engine::math::Vec3 half =
      engine::runtime::detail::transformed_aabb_half_extents(
          world, engine::math::Vec3(1.0F, 2.0F, 3.0F));

  return nearly_equal(half.x, 6.0F) && nearly_equal(half.y, 2.0F) &&
                 nearly_equal(half.z, 12.0F)
             ? 0
             : 1;
}

int check_construction_destruction() {
  engine::EnginePipeline pipeline;
  // Without engine::bootstrap(), initialize() should fail gracefully.
  // We only verify that construction and destruction don't crash.
  static_cast<void>(pipeline.initialize(0U));
  pipeline.teardown();
  return 0;
}

/// Verifies initialize failure does not leave the editor bound to a stale
/// world.
int check_initialize_failure_clears_editor_world() {
  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_editor_world;
  g_lastEditorWorld = nullptr;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EnginePipeline pipeline;
  const bool initialized = pipeline.initialize(0U);
  if (initialized) {
    pipeline.teardown();
    engine::runtime::set_editor_bridge(nullptr);
    return 1;
  }

  if (g_lastEditorWorld != nullptr) {
    pipeline.teardown();
    engine::runtime::set_editor_bridge(nullptr);
    return 2;
  }

  pipeline.teardown();
  engine::runtime::set_editor_bridge(nullptr);
  return 0;
}

/// Verifies editor processing can enable capture before gameplay input mutates.
int check_editor_capture_after_event_processing_skips_gameplay_input() {
  engine::core::shutdown_input();
  if (!engine::core::initialize_input()) {
    return 1;
  }

  g_editorCaptureKeyboard = false;
  g_editorProcessedEvents = 0;

  engine::runtime::EditorBridge bridge{};
  bridge.process_event = &process_editor_event_and_capture_keyboard;
  bridge.wants_capture_keyboard = &wants_keyboard_capture;

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.scancode = static_cast<SDL_Scancode>(engine::core::kKey_A);

  engine::core::begin_input_frame();
  const engine::runtime::InputEventRoute route =
      engine::runtime::process_editor_input_event(&bridge, &event);
  if (route == engine::runtime::InputEventRoute::Gameplay) {
    engine::core::input_process_event(&event);
  }
  engine::core::end_input_frame();

  const bool keyDown = engine::core::is_key_down(engine::core::kKey_A);
  engine::core::shutdown_input();

  if (g_editorProcessedEvents != 1) {
    return 2;
  }
  if (route != engine::runtime::InputEventRoute::EditorCaptured) {
    return 3;
  }
  if (keyDown) {
    return 4;
  }

  return 0;
}

/// Verifies editor mouse capture skips gameplay mouse state mutation.
int check_editor_mouse_capture_after_event_processing_skips_gameplay_input() {
  engine::core::shutdown_input();
  if (!engine::core::initialize_input()) {
    return 1;
  }

  g_editorCaptureMouse = false;
  g_editorProcessedEvents = 0;

  engine::runtime::EditorBridge bridge{};
  bridge.process_event = &process_editor_event_and_capture_mouse;
  bridge.wants_capture_mouse = &wants_mouse_capture;

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;

  engine::core::begin_input_frame();
  const engine::runtime::InputEventRoute route =
      engine::runtime::process_editor_input_event(&bridge, &event);
  if (route == engine::runtime::InputEventRoute::Gameplay) {
    engine::core::input_process_event(&event);
  }
  engine::core::end_input_frame();

  const bool mouseDown = engine::core::is_mouse_button_down(0);
  engine::core::shutdown_input();

  if (g_editorProcessedEvents != 1) {
    return 2;
  }
  if (route != engine::runtime::InputEventRoute::EditorCaptured) {
    return 3;
  }
  if (mouseDown) {
    return 4;
  }

  return 0;
}

/// Verifies non-captured editor events still reach gameplay input.
int check_uncaptured_editor_event_reaches_gameplay_input() {
  engine::core::shutdown_input();
  if (!engine::core::initialize_input()) {
    return 1;
  }

  g_editorProcessedEvents = 0;

  engine::runtime::EditorBridge bridge{};
  bridge.process_event = &process_editor_event_without_capture;
  bridge.wants_capture_keyboard = &wants_no_capture;

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.scancode = static_cast<SDL_Scancode>(engine::core::kKey_B);

  engine::core::begin_input_frame();
  const engine::runtime::InputEventRoute route =
      engine::runtime::process_editor_input_event(&bridge, &event);
  if (route == engine::runtime::InputEventRoute::Gameplay) {
    engine::core::input_process_event(&event);
  }
  engine::core::end_input_frame();

  const bool keyDown = engine::core::is_key_down(engine::core::kKey_B);
  engine::core::shutdown_input();

  if (g_editorProcessedEvents != 1) {
    return 2;
  }
  if (route != engine::runtime::InputEventRoute::Gameplay) {
    return 3;
  }
  if (!keyDown) {
    return 4;
  }

  return 0;
}

/// Verifies quit events are still surfaced after editor processing.
int check_editor_processed_quit_event_routes_to_quit() {
  g_editorProcessedEvents = 0;

  engine::runtime::EditorBridge bridge{};
  bridge.process_event = &process_editor_event_without_capture;

  SDL_Event event{};
  event.type = SDL_EVENT_QUIT;

  const engine::runtime::InputEventRoute route =
      engine::runtime::process_editor_input_event(&bridge, &event);

  if (g_editorProcessedEvents != 1) {
    return 1;
  }
  return (route == engine::runtime::InputEventRoute::QuitRequested) ? 0 : 2;
}

using TestFn = int (*)();

struct TestEntry {
  const char *name;
  TestFn fn;
};

const TestEntry g_tests[] = {
    {"local_direction_rotation", check_local_direction_rotation},
    {"transformed_aabb_half_extents", check_transformed_aabb_half_extents},
    {"construction_destruction", check_construction_destruction},
    {"initialize_failure_clears_editor_world",
     check_initialize_failure_clears_editor_world},
    {"editor_capture_after_event_processing_skips_gameplay_input",
     check_editor_capture_after_event_processing_skips_gameplay_input},
    {"editor_mouse_capture_after_event_processing_skips_gameplay_input",
     check_editor_mouse_capture_after_event_processing_skips_gameplay_input},
    {"uncaptured_editor_event_reaches_gameplay_input",
     check_uncaptured_editor_event_reaches_gameplay_input},
    {"editor_processed_quit_event_routes_to_quit",
     check_editor_processed_quit_event_routes_to_quit},
};

} // namespace

/// Runs this executable or test program.
int main() {
  int failures = 0;
  for (const auto &test : g_tests) {
    const int result = test.fn();
    if (result != 0) {
      ++failures;
    }
  }
  return failures;
}
