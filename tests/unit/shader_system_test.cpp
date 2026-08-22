// Verifies shader system test behavior for the Engine test suite.

#include "engine/renderer/shader_system.h"

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/material.h"
#include "engine/renderer/render_device.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

static int g_passed = 0;
static int g_failed = 0;

namespace engine::renderer {

namespace {

RenderDevice g_fakeDevice{};
std::uint32_t g_nextProgram = 100U;

const char *fake_cooked_profile() noexcept { return "spirv"; }

DeviceProgramHandle fake_create_program_binary(const void *, std::ptrdiff_t,
                                               const void *,
                                               std::ptrdiff_t) noexcept {
  return DeviceProgramHandle{g_nextProgram++};
}

void fake_destroy_program(DeviceProgramHandle) noexcept {}

void configure_fake_render_device() noexcept {
  g_fakeDevice = RenderDevice{};
  g_fakeDevice.caps.cookedPrograms = true;
  g_fakeDevice.cooked_program_profile = &fake_cooked_profile;
  g_fakeDevice.create_program_binary = &fake_create_program_binary;
  g_fakeDevice.destroy_program = &fake_destroy_program;
}

} // namespace

bool initialize_render_device() noexcept { return true; }

void shutdown_render_device() noexcept {}

const RenderDevice *render_device() noexcept { return &g_fakeDevice; }

} // namespace engine::renderer

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

/// Returns whether the selected material shader defines contain a name.
static bool has_selected_define(
    const engine::renderer::MaterialShaderVariantSelection &selection,
    const char *name) {
  for (std::size_t i = 0U; i < selection.defineCount; ++i) {
    if (std::strcmp(selection.defines[i].name, name) == 0) {
      return true;
    }
  }
  return false;
}

static bool write_shader_test_file(const char *path) noexcept {
  // Cooked-binary layout (#296): the loader reads
  // <dir>/bgfx/cooked/<file>.default.<profile>.bin for the fake
  // device's spirv profile; content is opaque to the fake link.
  char cooked[256] = {};
  const char *slash = std::strrchr(path, '/');
  const char *file = (slash != nullptr) ? (slash + 1) : path;
  const std::size_t dirLen =
      (slash != nullptr) ? static_cast<std::size_t>(slash - path) : 0U;
  std::snprintf(cooked, sizeof(cooked), "%.*s/bgfx/cooked/%s.default.spirv.bin",
                static_cast<int>(dirLen), path, file);
  static constexpr const char *kBlob = "cooked-test-blob";
  return engine::core::vfs_write_text(cooked, kBlob, std::strlen(kBlob));
}

static bool setup_shader_files() noexcept {
  if (!engine::core::initialize_logging()) {
    return false;
  }
  if (!engine::core::initialize_vfs()) {
    engine::core::shutdown_logging();
    return false;
  }
  if (!engine::core::mount("shader_test", ".")) {
    engine::core::shutdown_vfs();
    engine::core::shutdown_logging();
    return false;
  }

  std::error_code cookedDirError{};
  std::filesystem::create_directories("bgfx/cooked", cookedDirError);
  if (cookedDirError) {
    engine::core::shutdown_vfs();
    engine::core::shutdown_logging();
    return false;
  }

  if (!write_shader_test_file("shader_test/_shader_a.vert") ||
      !write_shader_test_file("shader_test/_shader_a.frag") ||
      !write_shader_test_file("shader_test/_shader_b.vert") ||
      !write_shader_test_file("shader_test/_shader_b.frag")) {
    engine::core::shutdown_vfs();
    engine::core::shutdown_logging();
    return false;
  }

  return true;
}

static void teardown_shader_files() noexcept {
  std::remove("bgfx/cooked/_shader_a.vert.default.spirv.bin");
  std::remove("bgfx/cooked/_shader_a.frag.default.spirv.bin");
  std::remove("bgfx/cooked/_shader_b.vert.default.spirv.bin");
  std::remove("bgfx/cooked/_shader_b.frag.default.spirv.bin");
  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
}

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

static void test_material_shader_define_selection_default_material() {
  using namespace engine::renderer;

  const Material material{};
  const MaterialShaderVariantSelection selection =
      select_material_shader_defines(material, false);

  TEST_ASSERT(selection.defineCount == 0U);
  ++g_passed;
}

static void test_material_shader_define_selection_feature_flags() {
  using namespace engine::renderer;

  Material material{};
  material.albedoTexture = TextureHandle{1U};
  material.normalTexture = TextureHandle{2U};
  material.emissive = engine::math::Vec3(0.2F, 0.0F, 0.0F);
  material.opacity = 0.5F;

  const MaterialShaderVariantSelection selection =
      select_material_shader_defines(material, true);

  TEST_ASSERT(selection.defineCount == 5U);
  TEST_ASSERT(has_selected_define(selection, "HAS_ALBEDO_TEXTURE"));
  TEST_ASSERT(has_selected_define(selection, "HAS_NORMAL_MAP"));
  TEST_ASSERT(has_selected_define(selection, "HAS_EMISSIVE"));
  TEST_ASSERT(has_selected_define(selection, "MATERIAL_TRANSLUCENT"));
  TEST_ASSERT(has_selected_define(selection, "SKINNED"));

  const ShaderVariantDesc desc{"assets/shaders/pbr.vert",
                               "assets/shaders/pbr.frag",
                               selection.defines,
                               selection.defineCount};
  TEST_ASSERT(shader_variant_key(desc).value != 0U);
  ++g_passed;
}

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
  TEST_ASSERT(shader_device_program(kInvalidShaderProgram) == kInvalidDeviceProgram);
  TEST_ASSERT(shader_device_program(ShaderProgramHandle{999U}) == kInvalidDeviceProgram);
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

static void test_stale_handle_rejected_after_slot_reuse() {
  using namespace engine::renderer;
  TEST_ASSERT(setup_shader_files());
  configure_fake_render_device();
  TEST_ASSERT(initialize_shader_system());

  const ShaderProgramHandle first = load_shader_program(
      "shader_test/_shader_a.vert", "shader_test/_shader_a.frag");
  TEST_ASSERT(first != kInvalidShaderProgram);
  const DeviceProgramHandle firstProgram = shader_device_program(first);
  TEST_ASSERT(firstProgram != kInvalidDeviceProgram);

  destroy_shader_program(first);
  TEST_ASSERT(shader_device_program(first) == kInvalidDeviceProgram);

  const ShaderProgramHandle second = load_shader_program(
      "shader_test/_shader_b.vert", "shader_test/_shader_b.frag");
  TEST_ASSERT(second != kInvalidShaderProgram);
  TEST_ASSERT(second.id == first.id);
  TEST_ASSERT(second.generation != first.generation);
  TEST_ASSERT(shader_device_program(second) != kInvalidDeviceProgram);

  destroy_shader_program(first);
  TEST_ASSERT(shader_device_program(second) != kInvalidDeviceProgram);

  destroy_shader_program(second);
  shutdown_shader_system();
  teardown_shader_files();
  ++g_passed;
}

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
  int before_test_material_shader_define_selection_default_material = g_failed;
  RUN_TEST(test_material_shader_define_selection_default_material);
  int before_test_material_shader_define_selection_feature_flags = g_failed;
  RUN_TEST(test_material_shader_define_selection_feature_flags);
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
  int before_test_stale_handle_rejected_after_slot_reuse = g_failed;
  RUN_TEST(test_stale_handle_rejected_after_slot_reuse);
  int before_test_check_reload_without_init = g_failed;
  RUN_TEST(test_check_reload_without_init);

  std::printf(
      "\nShader system tests: %d passed, %d failed\n", g_passed, g_failed);
  return (g_failed > 0) ? 1 : 0;
}
