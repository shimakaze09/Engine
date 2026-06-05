// Verifies engine pipeline test behavior for the Engine test suite.

#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/editor_bridge.h"

namespace {

engine::runtime::World *g_lastEditorWorld = nullptr;

/// Captures editor bridge world binding for pipeline cleanup tests.
void capture_editor_world(engine::runtime::World *world) noexcept {
  g_lastEditorWorld = world;
}

/// Handles check construction destruction.
int check_construction_destruction() {
  engine::EnginePipeline pipeline;
  // Without engine::bootstrap(), initialize() should fail gracefully.
  // We only verify that construction and destruction don't crash.
  static_cast<void>(pipeline.initialize(0U));
  pipeline.teardown();
  return 0;
}

/// Verifies initialize failure does not leave the editor bound to a stale world.
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

using TestFn = int (*)();

/// Stores test entry data used by the engine.
struct TestEntry {
  const char *name;
  TestFn fn;
};

const TestEntry g_tests[] = {
    {"construction_destruction", check_construction_destruction},
    {"initialize_failure_clears_editor_world",
     check_initialize_failure_clears_editor_world},
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
