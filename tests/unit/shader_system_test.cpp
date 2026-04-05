#include "engine/renderer/shader_system.h"

#include <cstdio>

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                  \
      ++g_failed;                                                              \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    std::printf("[ RUN  ] %s\n", #fn);                                         \
    fn();                                                                      \
    if (g_failed == before_##fn) {                                             \
      std::printf("[  OK  ] %s\n", #fn);                                       \
    }                                                                          \
  } while (false)

// --------------------------------------------------------------------------
// Tests — no GL context available, so we test the registry bookkeeping only.
// --------------------------------------------------------------------------

static void test_init_shutdown() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  // Double-init is fine.
  TEST_ASSERT(initialize_shader_system());
  shutdown_shader_system();
  // Double-shutdown is fine.
  shutdown_shader_system();
  ++g_passed;
}

static void test_load_without_init_returns_invalid() {
  using namespace engine::renderer;
  // System not initialized — should return invalid.
  const ShaderProgramHandle h = load_shader_program(
      "assets/shaders/default.vert", "assets/shaders/default.frag");
  TEST_ASSERT(h == kInvalidShaderProgram);
  ++g_passed;
}

static void test_load_null_paths() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  TEST_ASSERT(load_shader_program(nullptr, nullptr) == kInvalidShaderProgram);
  TEST_ASSERT(load_shader_program("a.vert", nullptr) == kInvalidShaderProgram);
  TEST_ASSERT(load_shader_program(nullptr, "a.frag") == kInvalidShaderProgram);
  shutdown_shader_system();
  ++g_passed;
}

static void test_gpu_program_invalid_handle() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  TEST_ASSERT(shader_gpu_program(kInvalidShaderProgram) == 0U);
  TEST_ASSERT(shader_gpu_program(ShaderProgramHandle{999U}) == 0U);
  shutdown_shader_system();
  ++g_passed;
}

static void test_destroy_invalid_handle() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  // Should not crash.
  destroy_shader_program(kInvalidShaderProgram);
  destroy_shader_program(ShaderProgramHandle{999U});
  shutdown_shader_system();
  ++g_passed;
}

static void test_check_reload_without_init() {
  using namespace engine::renderer;
  // Should not crash when system is not initialized.
  check_shader_reload();
  ++g_passed;
}

int main() {
  int before_test_init_shutdown = g_failed;
  RUN_TEST(test_init_shutdown);
  int before_test_load_without_init_returns_invalid = g_failed;
  RUN_TEST(test_load_without_init_returns_invalid);
  int before_test_load_null_paths = g_failed;
  RUN_TEST(test_load_null_paths);
  int before_test_gpu_program_invalid_handle = g_failed;
  RUN_TEST(test_gpu_program_invalid_handle);
  int before_test_destroy_invalid_handle = g_failed;
  RUN_TEST(test_destroy_invalid_handle);
  int before_test_check_reload_without_init = g_failed;
  RUN_TEST(test_check_reload_without_init);

  std::printf(
      "\nShader system tests: %d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
