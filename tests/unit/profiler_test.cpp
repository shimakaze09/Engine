#include <cstdio>
#include <cstring>

#include "engine/core/profiler.h"

using namespace engine::core;

namespace {

bool test_init_shutdown() noexcept {
  if (!initialize_profiler()) {
    return false;
  }
  shutdown_profiler();
  return true;
}

bool test_frame_boundaries() noexcept {
  if (!initialize_profiler()) {
    return false;
  }

  profiler_begin_frame();
  profiler_end_frame();

  const float frameMs = profiler_frame_time_ms();
  if (frameMs < 0.0F) {
    shutdown_profiler();
    return false;
  }

  shutdown_profiler();
  return true;
}

bool test_scope_push_pop() noexcept {
  if (!initialize_profiler()) {
    return false;
  }

  profiler_begin_frame();
  {
    ProfileScope scope("test_scope");
    volatile int dummy = 0;
    for (int i = 0; i < 100; ++i) {
      dummy += i;
    }
  }
  profiler_end_frame();

  ProfileEntry entries[16] = {};
  const std::size_t count = profiler_get_entries(entries, 16);
  if (count != 1U) {
    shutdown_profiler();
    return false;
  }
  if (std::strcmp(entries[0].name, "test_scope") != 0) {
    shutdown_profiler();
    return false;
  }
  if (entries[0].depth != 0U) {
    shutdown_profiler();
    return false;
  }
  if (entries[0].durationMs < 0.0F) {
    shutdown_profiler();
    return false;
  }

  shutdown_profiler();
  return true;
}

bool test_nested_scopes() noexcept {
  if (!initialize_profiler()) {
    return false;
  }

  profiler_begin_frame();
  profiler_begin_scope("outer");
  profiler_begin_scope("inner");
  profiler_end_scope();
  profiler_end_scope();
  profiler_end_frame();

  ProfileEntry entries[16] = {};
  const std::size_t count = profiler_get_entries(entries, 16);
  if (count != 2U) {
    shutdown_profiler();
    return false;
  }
  if (entries[0].depth != 0U) {
    shutdown_profiler();
    return false;
  }
  if (entries[1].depth != 1U) {
    shutdown_profiler();
    return false;
  }
  if (std::strcmp(entries[0].name, "outer") != 0) {
    shutdown_profiler();
    return false;
  }
  if (std::strcmp(entries[1].name, "inner") != 0) {
    shutdown_profiler();
    return false;
  }

  shutdown_profiler();
  return true;
}

bool test_double_buffer() noexcept {
  if (!initialize_profiler()) {
    return false;
  }

  // Frame 1.
  profiler_begin_frame();
  profiler_begin_scope("frame1_scope");
  profiler_end_scope();
  profiler_end_frame();

  // Frame 2 — write buffer is overwritten.
  profiler_begin_frame();
  profiler_begin_scope("frame2_scope_a");
  profiler_end_scope();
  profiler_begin_scope("frame2_scope_b");
  profiler_end_scope();
  profiler_end_frame();

  // Read buffer should have frame 2 entries.
  ProfileEntry entries[16] = {};
  const std::size_t count = profiler_get_entries(entries, 16);
  if (count != 2U) {
    shutdown_profiler();
    return false;
  }
  if (std::strcmp(entries[0].name, "frame2_scope_a") != 0) {
    shutdown_profiler();
    return false;
  }
  if (std::strcmp(entries[1].name, "frame2_scope_b") != 0) {
    shutdown_profiler();
    return false;
  }

  shutdown_profiler();
  return true;
}

bool test_null_out() noexcept {
  if (!initialize_profiler()) {
    return false;
  }

  profiler_begin_frame();
  profiler_end_frame();

  std::size_t count = profiler_get_entries(nullptr, 10);
  if (count != 0U) {
    shutdown_profiler();
    return false;
  }

  ProfileEntry e{};
  count = profiler_get_entries(&e, 0);
  if (count != 0U) {
    shutdown_profiler();
    return false;
  }

  shutdown_profiler();
  return true;
}

} // namespace

int main() {
  int passed = 0;
  int failed = 0;

  auto run = [&](const char *name, bool (*fn)() noexcept) {
    if (fn()) {
      ++passed;
      std::printf("  PASS  %s\n", name);
    } else {
      ++failed;
      std::printf("  FAIL  %s\n", name);
    }
  };

  std::printf("--- profiler tests ---\n");
  run("init_shutdown", &test_init_shutdown);
  run("frame_boundaries", &test_frame_boundaries);
  run("scope_push_pop", &test_scope_push_pop);
  run("nested_scopes", &test_nested_scopes);
  run("double_buffer", &test_double_buffer);
  run("null_out", &test_null_out);

  std::printf("--- %d passed, %d failed ---\n", passed, failed);
  return (failed > 0) ? 1 : 0;
}
