#include <cstddef>

#ifdef _MSC_VER
#pragma warning(disable : 4127) // constant conditional (constexpr checks in tests)
#endif

#include "engine/renderer/post_process_stack.h"

namespace {

// ---------------------------------------------------------------------------
// Test 1: Initialize creates the default 5-pass stack.
// ---------------------------------------------------------------------------
int verify_default_stack() {
  engine::renderer::initialize_post_process_stack();

  const auto &stack = engine::renderer::get_post_process_stack();
  if (stack.passCount !=
      static_cast<std::size_t>(engine::renderer::PostProcessPassId::Count)) {
    return 100;
  }

  // Verify default ordering.
  if (stack.passes[0].id != engine::renderer::PostProcessPassId::Bloom)
    return 101;
  if (stack.passes[1].id != engine::renderer::PostProcessPassId::SSAO)
    return 102;
  if (stack.passes[2].id != engine::renderer::PostProcessPassId::AutoExposure)
    return 103;
  if (stack.passes[3].id != engine::renderer::PostProcessPassId::Tonemap)
    return 104;
  if (stack.passes[4].id != engine::renderer::PostProcessPassId::FXAA)
    return 105;

  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: All passes are enabled by default.
// ---------------------------------------------------------------------------
int verify_all_enabled_by_default() {
  engine::renderer::initialize_post_process_stack();

  const auto &stack = engine::renderer::get_post_process_stack();
  for (std::size_t i = 0U; i < stack.passCount; ++i) {
    if (!stack.passes[i].enabled) {
      return 200;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Pass names are valid (non-null, non-empty).
// ---------------------------------------------------------------------------
int verify_pass_names() {
  for (int i = 0;
       i < static_cast<int>(engine::renderer::PostProcessPassId::Count); ++i) {
    const char *name = engine::renderer::post_process_pass_name(
        static_cast<engine::renderer::PostProcessPassId>(i));
    if (name == nullptr)
      return 300;
    if (name[0] == '\0')
      return 301;
  }

  // Invalid ID should return "Unknown".
  const char *unknown = engine::renderer::post_process_pass_name(
      engine::renderer::PostProcessPassId::Count);
  if (unknown == nullptr)
    return 302;

  return 0;
}

// ---------------------------------------------------------------------------
// Test 4: Stack capacity matches pass count.
// ---------------------------------------------------------------------------
int verify_stack_capacity() {
  const auto &stack = engine::renderer::get_post_process_stack();
  if (stack.kMaxPasses !=
      static_cast<std::size_t>(engine::renderer::PostProcessPassId::Count)) {
    return 400;
  }
  return 0;
}

} // namespace

int main() {
  int result = verify_default_stack();
  if (result != 0)
    return result;

  result = verify_all_enabled_by_default();
  if (result != 0)
    return result;

  result = verify_pass_names();
  if (result != 0)
    return result;

  result = verify_stack_capacity();
  if (result != 0)
    return result;

  return 0;
}
