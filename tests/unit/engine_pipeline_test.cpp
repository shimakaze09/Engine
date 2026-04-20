#include "engine/runtime/engine_pipeline.h"

namespace {

int check_construction_destruction() {
  engine::EnginePipeline pipeline;
  // Without engine::bootstrap(), initialize() should fail gracefully.
  // We only verify that construction and destruction don't crash.
  static_cast<void>(pipeline.initialize(0U));
  pipeline.teardown();
  return 0;
}

using TestFn = int (*)();

struct TestEntry {
  const char *name;
  TestFn fn;
};

const TestEntry g_tests[] = {
    {"construction_destruction", check_construction_destruction},
};

} // namespace

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
