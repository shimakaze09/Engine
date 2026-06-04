// Verifies shader system test behavior for the Engine test suite.

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

/// Handles test init shutdown.
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

/// Handles test variant key order independent.
static void test_variant_key_order_independent() {
  using namespace engine::renderer;

  const ShaderDefine definesA[] = {{"HAS_NORMAL_MAP", "1"}, {"SKINNED", "1"}};
  const ShaderDefine definesB[] = {{"SKINNED", "1"}, {"HAS_NORMAL_MAP", "1"}};
  const ShaderVariantDesc descA{"assets/shaders/pbr.vert",
                                "assets/shaders/pbr.frag",
                                definesA,
                                2U};
  const ShaderVariantDesc descB{"assets/shaders/pbr.vert",
                                "assets/shaders/pbr.frag",
                                definesB,
                                2U};

  const ShaderVariantKey keyA = shader_variant_key(descA);
  const ShaderVariantKey keyB = shader_variant_key(descB);
  TEST_ASSERT(keyA.value != 0U);
  TEST_ASSERT(keyA == keyB);
  ++g_passed;
}

/// Handles test variant key distinguishes values.
static void test_variant_key_distinguishes_values() {
  using namespace engine::renderer;

  const ShaderDefine enabled[] = {{"HAS_EMISSIVE", "1"}};
  const ShaderDefine disabled[] = {{"HAS_EMISSIVE", "0"}};
  const ShaderVariantDesc enabledDesc{"assets/shaders/pbr.vert",
                                      "assets/shaders/pbr.frag",
                                      enabled,
                                      1U};
  const ShaderVariantDesc disabledDesc{"assets/shaders/pbr.vert",
                                       "assets/shaders/pbr.frag",
                                       disabled,
                                       1U};

  const ShaderVariantKey enabledKey = shader_variant_key(enabledDesc);
  const ShaderVariantKey disabledKey = shader_variant_key(disabledDesc);
  TEST_ASSERT(enabledKey.value != 0U);
  TEST_ASSERT(disabledKey.value != 0U);
  TEST_ASSERT(!(enabledKey == disabledKey));
  ++g_passed;
}

/// Handles test variant invalid descriptors.
static void test_variant_invalid_descriptors() {
  using namespace engine::renderer;

  TEST_ASSERT(shader_variant_key(ShaderVariantDesc{}).value == 0U);
  TEST_ASSERT(load_shader_variant(ShaderVariantDesc{}) == kInvalidShaderProgram);

  const ShaderVariantDesc missingDefineArray{
      "assets/shaders/pbr.vert", "assets/shaders/pbr.frag", nullptr, 1U};
  TEST_ASSERT(shader_variant_key(missingDefineArray).value == 0U);

  const ShaderDefine badDefine[] = {{nullptr, "1"}};
  const ShaderVariantDesc badDefineDesc{"assets/shaders/pbr.vert",
                                        "assets/shaders/pbr.frag",
                                        badDefine,
                                        1U};
  TEST_ASSERT(shader_variant_key(badDefineDesc).value == 0U);

  TEST_ASSERT(initialize_shader_system());
  TEST_ASSERT(load_shader_variant(missingDefineArray) == kInvalidShaderProgram);
  TEST_ASSERT(load_shader_variant(badDefineDesc) == kInvalidShaderProgram);
  shutdown_shader_system();
  ++g_passed;
}

/// Handles test load without init returns invalid.
static void test_load_without_init_returns_invalid() {
  using namespace engine::renderer;
  // System not initialized — should return invalid.
  const ShaderProgramHandle h = load_shader_program(
      "assets/shaders/default.vert", "assets/shaders/default.frag");
  TEST_ASSERT(h == kInvalidShaderProgram);
  ++g_passed;
}

/// Handles test load null paths.
static void test_load_null_paths() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  TEST_ASSERT(load_shader_program(nullptr, nullptr) == kInvalidShaderProgram);
  TEST_ASSERT(load_shader_program("a.vert", nullptr) == kInvalidShaderProgram);
  TEST_ASSERT(load_shader_program(nullptr, "a.frag") == kInvalidShaderProgram);
  shutdown_shader_system();
  ++g_passed;
}

/// Handles test gpu program invalid handle.
static void test_gpu_program_invalid_handle() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  TEST_ASSERT(shader_gpu_program(kInvalidShaderProgram) == 0U);
  TEST_ASSERT(shader_gpu_program(ShaderProgramHandle{999U}) == 0U);
  shutdown_shader_system();
  ++g_passed;
}

/// Handles test destroy invalid handle.
static void test_destroy_invalid_handle() {
  using namespace engine::renderer;
  TEST_ASSERT(initialize_shader_system());
  // Should not crash.
  destroy_shader_program(kInvalidShaderProgram);
  destroy_shader_program(ShaderProgramHandle{999U});
  shutdown_shader_system();
  ++g_passed;
}

/// Handles test check reload without init.
static void test_check_reload_without_init() {
  using namespace engine::renderer;
  // Should not crash when system is not initialized.
  check_shader_reload();
  ++g_passed;
}

/// Runs this executable or test program.
int main() {
  int before_test_init_shutdown = g_failed;
  RUN_TEST(test_init_shutdown);
  int before_test_variant_key_order_independent = g_failed;
  RUN_TEST(test_variant_key_order_independent);
  int before_test_variant_key_distinguishes_values = g_failed;
  RUN_TEST(test_variant_key_distinguishes_values);
  int before_test_variant_invalid_descriptors = g_failed;
  RUN_TEST(test_variant_invalid_descriptors);
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
