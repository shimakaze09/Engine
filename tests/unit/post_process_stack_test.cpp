// Verifies post process stack test behavior for the Engine test suite.

#include <cstddef>

#ifdef _MSC_VER
#pragma warning(disable : 4127) // constant conditional (constexpr checks in tests)
#endif

#include "engine/core/cvar.h"
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

// ---------------------------------------------------------------------------
// Test 5: Pass toggles follow their cvars live, and repeated per-frame
// queries scan the cvar table by name zero times after the first.
// ---------------------------------------------------------------------------
int verify_pass_toggles_follow_cvars_without_name_scans() {
  using engine::renderer::PostProcessPassId;
  engine::core::initialize_cvars();
  engine::core::cvar_register_bool("r_bloom", true, "test");
  engine::core::cvar_register_bool("r_fxaa", false, "test");

  if (!engine::renderer::is_post_process_pass_enabled(PostProcessPassId::Bloom))
    return 500;
  if (engine::renderer::is_post_process_pass_enabled(PostProcessPassId::FXAA))
    return 501;
  // No toggle bound: always enabled, and never a lookup.
  if (!engine::renderer::is_post_process_pass_enabled(
          PostProcessPassId::Tonemap))
    return 502;

  const std::size_t lookupsBefore = engine::core::cvar_name_lookup_count();
  for (int i = 0; i < 100; ++i) {
    static_cast<void>(engine::renderer::is_post_process_pass_enabled(
        PostProcessPassId::Bloom));
    static_cast<void>(engine::renderer::is_post_process_pass_enabled(
        PostProcessPassId::FXAA));
    static_cast<void>(engine::renderer::is_post_process_pass_enabled(
        PostProcessPassId::Tonemap));
  }
  if (engine::core::cvar_name_lookup_count() != lookupsBefore)
    return 503;

  // Live tuning: a set by name is seen by the next query.
  static_cast<void>(engine::core::cvar_set_bool("r_bloom", false));
  static_cast<void>(engine::core::cvar_set_bool("r_fxaa", true));
  if (engine::renderer::is_post_process_pass_enabled(PostProcessPassId::Bloom))
    return 504;
  if (!engine::renderer::is_post_process_pass_enabled(PostProcessPassId::FXAA))
    return 505;

  engine::core::shutdown_cvars();
  // Unregistered again: the toggle falls back to enabled.
  if (!engine::renderer::is_post_process_pass_enabled(PostProcessPassId::Bloom))
    return 506;
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = verify_default_stack();
  if (result != 0)
    return result;

  result = verify_pass_toggles_follow_cvars_without_name_scans();
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
