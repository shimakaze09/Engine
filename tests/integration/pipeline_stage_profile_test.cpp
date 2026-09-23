// The Stats panel's flame graph had exactly one scope in the whole
// engine -- "engine_frame" -- so it showed one bar and told nobody where
// a frame went. This pins that every pipeline stage now records a scope,
// and that the profiler refuses a scope from any other thread instead of
// racing its plain globals (#416 item 8).

#include "engine/core/native_thread.h"
#include "engine/core/profiler.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"

#include <cstdio>
#include <cstring>
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

// Stages every frame runs, whatever the play state. Deliberately not the
// whole list: the graph stages only run when the frame graph does, and
// tying this test to all nineteen would make it fail for any reordering
// rather than for the thing it checks.
constexpr const char *kAlwaysRunStages[] = {
    "input",      "play-transitions", "timing",   "scripting",
    "assets",     "hot-reload",       "audio",    "animation",
    "render",     "diagnostics",      "frame-cleanup"};

constexpr std::size_t kEntryCapacity = 256U;
engine::core::ProfileEntry g_entries[kEntryCapacity] = {};

bool entries_contain(std::size_t count, const char *name) noexcept {
  for (std::size_t i = 0U; i < count; ++i) {
    if ((g_entries[i].name != nullptr) &&
        (std::strcmp(g_entries[i].name, name) == 0)) {
      return true;
    }
  }
  return false;
}

// Set by the thread below if its scope was recorded, which it must not be.
bool g_foreignScopeAccepted = false;

// A thread of its own rather than a job: the job system helps from the
// calling thread while it waits, so a submitted job can legitimately run
// on the profiler's own thread and its scope be accepted. A directly
// spawned thread is never the owner, which is the case worth pinning.
void foreign_thread_entry(void * /*data*/) noexcept {
  g_foreignScopeAccepted = engine::core::profiler_begin_scope("foreign");
  if (g_foreignScopeAccepted) {
    engine::core::profiler_end_scope();
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
  config.core.workerThreads = 2U;
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

    // Two frames: the profiler double-buffers, so the entries readable
    // now are the previous frame's.
    CHECK(pipeline.execute_frame(), "frame 1");
    CHECK(pipeline.execute_frame(), "frame 2");

    const std::size_t count =
        engine::core::profiler_get_entries(g_entries, kEntryCapacity);

    // The bar that already existed, and the ones that did not.
    CHECK(entries_contain(count, "engine_frame"), "the frame scope is there");
    for (const char *name : kAlwaysRunStages) {
      if (!entries_contain(count, name)) {
        std::fprintf(stderr, "FAIL: no scope recorded for stage \"%s\"\n",
                     name);
        ++g_failures;
      }
    }

    // More than the one scope the panel used to show. Stated as a number
    // so a regression that instrumented only a couple of stages still
    // fails rather than passing on the frame scope alone.
    constexpr std::size_t kMinimumScopes =
        (sizeof(kAlwaysRunStages) / sizeof(kAlwaysRunStages[0])) + 1U;
    if (count < kMinimumScopes) {
      std::fprintf(stderr,
                   "FAIL: %zu scopes recorded, expected at least %zu\n",
                   count, kMinimumScopes);
      ++g_failures;
      result = 4;
    }

    // Another thread's scope must be refused, not recorded: the buffer,
    // the scope stack and the depth are plain globals, and a second
    // thread writing them corrupts the frame for both.
    engine::core::NativeThread foreign{};
    CHECK(foreign.spawn(&foreign_thread_entry, nullptr),
          "a second thread starts");
    foreign.join();
    CHECK(!g_foreignScopeAccepted,
          "a profiler scope opened off the profiler's thread is refused");
    if (g_foreignScopeAccepted) {
      result = 5;
    }

    pipeline.teardown();
  }

  engine::shutdown();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return (result != 0) ? result : 1;
  }

  std::puts("pipeline_stage_profile_test passed");
  return 0;
}
