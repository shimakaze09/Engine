// Implements the alpha-mask shadow programs' lifecycle: loading the
// MASKED variants of the shadow depth programs, re-resolving them after a
// shader reload, and releasing them.

#include "shadow_masked_programs.h"

#include "command_buffer_context.h"
#include "command_buffer_init_internal.h"

#include "engine/core/logging.h"
#include "engine/renderer/shader_system.h"

#include <cstdio>

namespace engine::renderer {

namespace {

/// Resolves one MASKED program's interface. REQUIRED: the light MVP and
/// the mask, cutoff and UV transform; the point variant's model matrix,
/// light position and far plane; the skinned variant's bone palette. No
/// OPTIONAL uniforms.
bool resolve_masked_program(MaskedShadowProgram &masked,
                            const RenderDevice *dev, bool point,
                            bool skinned) noexcept {
  masked.program = shader_device_program(masked.shaderHandle);
  const DeviceProgramHandle prog = masked.program;
  if (prog == kInvalidDeviceProgram) {
    return false;
  }
  bool ok = true;
  masked.lightMvpLoc = required_param(&ok, dev, prog, "u_lightMVP");
  masked.opacityMaskLoc = required_param(&ok, dev, prog, "s_opacityMask");
  masked.alphaCutoffLoc = required_param(&ok, dev, prog, "u_alphaCutoff");
  masked.uvTilingLoc = required_param(&ok, dev, prog, "u_uvTiling");
  masked.uvOffsetLoc = required_param(&ok, dev, prog, "u_uvOffset");
  if (point) {
    masked.modelLoc = required_param(&ok, dev, prog, "u_modelMatrix");
    masked.lightPosLoc = required_param(&ok, dev, prog, "u_lightPos");
    masked.farPlaneLoc = required_param(&ok, dev, prog, "u_farPlane");
  }
  if (skinned) {
    masked.bonesParam = dev->shader_param(prog, "uBones");
    ok = ok && masked.bonesParam.valid();
  }
  if (!ok) {
    masked.program = kInvalidDeviceProgram;
  }
  return ok;
}

/// Loads and resolves one MASKED program; on failure nothing is kept and
/// `family` names what keeps casting full silhouettes.
void load_masked_program(MaskedShadowProgram &masked, const RenderDevice *dev,
                         const char *vert, const char *frag, bool point,
                         bool skinned, const char *family) noexcept {
  const ShaderDefine defines[] = {{"MASKED", "1"}, {"SKINNED", "1"}};
  masked.shaderHandle =
      load_configured_shader_variant(vert, frag, defines, skinned ? 2U : 1U);
  if (resolve_masked_program(masked, dev, point, skinned)) {
    return;
  }
  if (masked.shaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(masked.shaderHandle);
  }
  masked = MaskedShadowProgram{};
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "alpha-mask shadow shader not available — mask-mode %s "
                "cast full-silhouette shadows",
                family);
  core::log_message(core::LogLevel::Warning, "renderer", message);
}

/// Re-resolves one loaded MASKED program after a shader reload.
void refresh_masked_program(MaskedShadowProgram &masked,
                            const RenderDevice *dev, bool point, bool skinned,
                            const char *family) noexcept {
  if ((masked.shaderHandle == kInvalidShaderProgram) ||
      resolve_masked_program(masked, dev, point, skinned)) {
    return;
  }
  char message[176] = {};
  std::snprintf(message, sizeof(message),
                "alpha-mask shadow variant lost required state on shader "
                "reload — mask-mode %s cast full-silhouette shadows",
                family);
  core::log_message(core::LogLevel::Warning, "renderer", message);
}

void release_masked_program(MaskedShadowProgram &masked) noexcept {
  if (masked.shaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(masked.shaderHandle);
  }
  masked = MaskedShadowProgram{};
}

constexpr const char *kStaticFamily = "materials";
constexpr const char *kSkinnedFamily = "skinned materials";
constexpr const char *kPointFamily = "materials under point lights";

} // namespace

void init_masked_shadow_programs(BackendState &backend,
                                 const RenderDevice *dev) noexcept {
  if (backend.shadowAvailable) {
    load_masked_program(backend.shadowMasked, dev, "shadow_depth.vert",
                        "shadow_depth.frag", false, false, kStaticFamily);
  }
  if (backend.shadowDepthSkinnedProgram != kInvalidDeviceProgram) {
    load_masked_program(backend.shadowSkinnedMasked, dev, "shadow_depth.vert",
                        "shadow_depth.frag", false, true, kSkinnedFamily);
  }
  if (backend.pointShadowAvailable) {
    load_masked_program(backend.shadowPointMasked, dev,
                        "shadow_depth_point.vert", "shadow_depth_point.frag",
                        true, false, kPointFamily);
  }
}

void refresh_masked_shadow_programs(BackendState &backend,
                                    const RenderDevice *dev) noexcept {
  refresh_masked_program(backend.shadowMasked, dev, false, false,
                         kStaticFamily);
  refresh_masked_program(backend.shadowSkinnedMasked, dev, false, true,
                         kSkinnedFamily);
  refresh_masked_program(backend.shadowPointMasked, dev, true, false,
                         kPointFamily);
}

void release_masked_shadow_programs(BackendState &backend) noexcept {
  release_masked_program(backend.shadowMasked);
  release_masked_program(backend.shadowSkinnedMasked);
  release_masked_program(backend.shadowPointMasked);
  backend.lastShadowMaskedBonePalette = 0xFFFFFFFFU;
}

} // namespace engine::renderer
