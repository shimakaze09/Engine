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
/// programs with required uniforms, and the fullscreen empty VAO. On any
/// failure the partial state is unwound and the backend is marked failed.
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

} // namespace engine::renderer
