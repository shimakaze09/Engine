// Declares the backend initialization stages command_buffer.cpp sequences:
// the hard-fail core (device, shader system, default/PBR/tonemap programs),
// then the soft-fail environment, lighting, and post/overlay groups whose
// availability flags gate their passes.

#pragma once

#include "engine/renderer/shader_system.h"
#include "command_buffer_context.h"

namespace engine::renderer {

/// Loads a shader pair from the configured shader root.
ShaderProgramHandle load_configured_shader_program(
    const char *vertFileName, const char *fragFileName) noexcept;

/// Loads a shader-pair variant from the configured shader root with the
/// given preprocessor defines.
ShaderProgramHandle load_configured_shader_variant(
    const char *vertFileName, const char *fragFileName,
    const ShaderDefine *defines, std::size_t defineCount) noexcept;

/// Hard-fail core: render device, shader system, default/PBR/tonemap
/// programs with required shader params, and the fullscreen attribute-
/// less geometry. On any failure the partial state is unwound and the
/// backend is marked failed.
bool init_backend_core(BackendState &backend) noexcept;

/// Soft-fail sky and IBL programs: cubemap skybox, Preetham, procedural
/// scatter, environment prefilter/irradiance, and the BRDF LUT.
void init_backend_environment(BackendState &backend,
                              const RenderDevice *dev) noexcept;

/// Soft-fail deferred pipeline (cvars, G-Buffer/lighting/debug programs and
/// uniforms) plus the cascade/spot/point shadow depth programs.
void init_backend_lighting(BackendState &backend,
                           const RenderDevice *dev) noexcept;

/// Soft-fail post and overlay programs: FXAA, bloom (with the tonemap
/// integration uniforms), SSAO, debug lines, and auto exposure.
void init_backend_post(BackendState &backend,
                       const RenderDevice *dev) noexcept;

// Per-program resolvers shared by initialization and hot-reload refresh
// (audit H-09): each re-reads the device program from its stored handle
// and re-queries every cached shader param (and uniform-block binding
// where the program uses one). Every queried name is classified REQUIRED
// (looked up through required_param; the pass cannot render meaningfully
// without it) or OPTIONAL (safe shader default or conditionally used;
// plain shader_param). A resolver returns false when any REQUIRED param
// is invalid or a required block is absent, so init leaves the family
// unavailable and refresh downgrades it.

/// Looks up a REQUIRED shader parameter: returns the param and clears *ok
/// when the current link no longer exposes it, so resolvers accumulate one
/// verdict instead of silently caching an invalid param.
inline ShaderParam required_param(bool *ok, const RenderDevice *dev,
                                  DeviceProgramHandle program,
                                  const char *name) noexcept {
  const ShaderParam param = dev->shader_param(program, name);
  if (!param.valid()) {
    *ok = false;
  }
  return param;
}

/// Default fallback program: id only, no cached uniforms.
bool resolve_default_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept;

/// Forward PBR program: all material/camera/fog/light/shadow locations.
bool resolve_pbr_program_state(BackendState &backend,
                               const RenderDevice *dev) noexcept;

/// Tonemap program, including the bloom-integration uniforms.
bool resolve_tonemap_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept;

/// Cubemap skybox program.
bool resolve_skybox_program_state(BackendState &backend,
                                  const RenderDevice *dev) noexcept;

/// Preetham procedural sky program.
bool resolve_preetham_sky_program_state(BackendState &backend,
                                        const RenderDevice *dev) noexcept;

/// Hosek-Wilkie procedural scatter sky program.
bool resolve_hosek_sky_program_state(BackendState &backend,
                                     const RenderDevice *dev) noexcept;

/// Specular environment prefilter program.
bool resolve_environment_prefilter_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept;

/// Diffuse irradiance convolution program.
bool resolve_environment_irradiance_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept;

/// Split-sum BRDF LUT program: id only, no cached uniforms.
bool resolve_environment_brdf_lut_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept;

/// G-Buffer geometry program.
bool resolve_gbuffer_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept;

/// Deferred lighting program: G-Buffer samplers, tiles, fog, IBL, SSAO,
/// and every shadow family.
bool resolve_deferred_light_program_state(BackendState &backend,
                                          const RenderDevice *dev) noexcept;

/// G-Buffer debug visualization program.
bool resolve_gbuffer_debug_program_state(BackendState &backend,
                                         const RenderDevice *dev) noexcept;

/// Cascade shadow depth program.
bool resolve_shadow_depth_program_state(BackendState &backend,
                                        const RenderDevice *dev) noexcept;

/// Point-light cubemap shadow depth program.
bool resolve_shadow_depth_point_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept;

/// Skinned G-Buffer variant; rebinds the BonePalette uniform block.
bool resolve_gbuffer_skinned_program_state(BackendState &backend,
                                           const RenderDevice *dev) noexcept;

/// Skinned shadow depth variant; rebinds the BonePalette uniform block.
bool resolve_shadow_depth_skinned_program_state(
    BackendState &backend, const RenderDevice *dev) noexcept;

/// FXAA program.
bool resolve_fxaa_program_state(BackendState &backend,
                                const RenderDevice *dev) noexcept;

/// The three bloom programs (threshold, downsample, upsample).
bool resolve_bloom_program_state(BackendState &backend,
                                 const RenderDevice *dev) noexcept;

/// SSAO and SSAO blur programs.
bool resolve_ssao_program_state(BackendState &backend,
                                const RenderDevice *dev) noexcept;

/// Depth-tested debug line program.
bool resolve_debug_line_program_state(BackendState &backend,
                                      const RenderDevice *dev) noexcept;

/// Auto-exposure luminance program.
bool resolve_luminance_program_state(BackendState &backend,
                                     const RenderDevice *dev) noexcept;

/// Re-resolves every cached device program, shader param, and uniform-
/// block binding from the shader-system handles after a hot reload, and
/// downgrades availability flags whose program lost a required param.
/// Called by flush_renderer when shader_reload_epoch() moves past
/// BackendState::programCacheEpoch.
void refresh_backend_program_state(BackendState &backend,
                                   const RenderDevice *dev) noexcept;

} // namespace engine::renderer
