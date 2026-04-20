#include "engine/renderer/post_process_stack.h"

#include "engine/core/cvar.h"

namespace engine::renderer {

namespace {

PostProcessStack g_stack{};
bool g_initialized = false;

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

void initialize_post_process_stack() noexcept {
  // Default ordering: Bloom → SSAO → AutoExposure → Tonemap → FXAA.
  g_stack.passCount = static_cast<std::size_t>(PostProcessPassId::Count);
  for (std::size_t i = 0U; i < g_stack.passCount; ++i) {
    g_stack.passes[i].id = static_cast<PostProcessPassId>(i);
    g_stack.passes[i].enabled = true;
  }
  g_initialized = true;
}

const PostProcessStack &get_post_process_stack() noexcept { return g_stack; }

bool is_post_process_pass_enabled(PostProcessPassId id) noexcept {
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= static_cast<std::size_t>(PostProcessPassId::Count)) {
    return false;
  }

  // Check CVar if one is bound to this pass.
  for (const auto &binding : kPassCVars) {
    if (binding.id == id && binding.cvarName != nullptr) {
      return core::cvar_get_bool(binding.cvarName, true);
    }
  }

  return true; // No CVar bound → always enabled.
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
