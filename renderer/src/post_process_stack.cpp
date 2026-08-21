// Implements post process stack behavior for the Engine renderer system.

#include "engine/renderer/post_process_stack.h"

#include "engine/core/cvar.h"

namespace engine::renderer {

namespace {

PostProcessStack g_stack{};

struct PassCVarBinding final {
  PostProcessPassId id;
  const char *cvarName;
};

constexpr PassCVarBinding kPassCVars[] = {
    {PostProcessPassId::Bloom, "r_bloom"},
    {PostProcessPassId::SSAO, "r_ssao"},
    {PostProcessPassId::AutoExposure, "r_auto_exposure"},
    {PostProcessPassId::Tonemap, nullptr}, // always enabled
    {PostProcessPassId::FXAA, "r_fxaa"},
};

} // namespace

/// Initializes the stack in its default order: Bloom → SSAO →
/// AutoExposure → Tonemap → FXAA, all enabled.
void initialize_post_process_stack() noexcept {
  g_stack.passCount = static_cast<std::size_t>(PostProcessPassId::Count);
  for (std::size_t i = 0U; i < g_stack.passCount; ++i) {
    g_stack.passes[i].id = static_cast<PostProcessPassId>(i);
    g_stack.passes[i].enabled = true;
  }
}

const PostProcessStack &get_post_process_stack() noexcept { return g_stack; }

/// Returns whether the pass is enabled: its bound CVar when one exists,
/// otherwise always enabled.
bool is_post_process_pass_enabled(PostProcessPassId id) noexcept {
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= static_cast<std::size_t>(PostProcessPassId::Count)) {
    return false;
  }

  for (const auto &binding : kPassCVars) {
    if (binding.id == id && binding.cvarName != nullptr) {
      return core::cvar_get_bool(binding.cvarName, true);
    }
  }

  return true;
}

const char *post_process_pass_name(PostProcessPassId id) noexcept {
  switch (id) {
  case PostProcessPassId::Bloom:
    return "Bloom";
  case PostProcessPassId::SSAO:
    return "SSAO";
  case PostProcessPassId::AutoExposure:
    return "AutoExposure";
  case PostProcessPassId::Tonemap:
    return "Tonemap";
  case PostProcessPassId::FXAA:
    return "FXAA";
  default:
    return "Unknown";
  }
}

} // namespace engine::renderer
