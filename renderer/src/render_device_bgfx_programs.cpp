// Implements the bgfx backend's program path (#138 Phase C): linking
// cooked shader binaries into programs with an introspected,
// name-addressable parameter table, the shader-parameter setters
// (scalar/vec values pad to bgfx's vec4 uniform model), and sampler
// application at submit. Runtime GLSL compilation stays unavailable —
// bgfx programs only exist from the shaderc cook's outputs.

#include "render_device_bgfx_context.h"

#include "engine/core/logging.h"
#include "engine/core/string_util.h"

#include <cstring>

namespace engine::renderer::bgfx_backend {

namespace {

/// Cooked shader binaries open with shaderc's chunk magic; rejecting
/// other bytes here keeps malformed input a logged failure instead of a
/// bgfx-internal assertion.
bool has_shader_magic(const void *data, std::ptrdiff_t size,
                      char kind) noexcept {
  if ((data == nullptr) || (size < 4)) {
    return false;
  }
  const char *bytes = static_cast<const char *>(data);
  return (bytes[0] == kind) && (bytes[1] == 'S') && (bytes[2] == 'H');
}

/// Copies a shader's introspected uniforms into the program's parameter
/// table, deduplicating by name across the vertex/fragment stages.
void collect_shader_params(bgfx::ShaderHandle shader,
                           BgfxProgramRecord *record) noexcept {
  bgfx::UniformHandle uniforms[kMaxProgramParams] = {};
  const std::uint16_t count = bgfx::getShaderUniforms(
      shader, uniforms, static_cast<std::uint16_t>(kMaxProgramParams));
  for (std::uint16_t i = 0U; i < count; ++i) {
    bgfx::UniformInfo info{};
    bgfx::getUniformInfo(uniforms[i], info);
    bool duplicate = false;
    for (std::uint16_t existing = 0U; existing < record->paramCount;
         ++existing) {
      if (std::strncmp(record->params[existing].name, info.name,
                       kMaxParamNameLength) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    if (record->paramCount >= kMaxProgramParams) {
      drop_operation("create_program_binary: parameter table full");
      return;
    }
    BgfxParamRecord &param = record->params[record->paramCount];
    core::copy_string(param.name, kMaxParamNameLength, info.name);
    param.handle = uniforms[i];
    param.type = info.type;
    param.samplerStage = -1;
    ++record->paramCount;
  }
}

/// Parameter behind the token on the currently bound program; nullptr
/// for invalid tokens (defined no-op per the contract).
BgfxParamRecord *resolve_param(ShaderParam param) noexcept {
  if (!param.valid()) {
    return nullptr;
  }
  BgfxProgramRecord *record = current_program_record();
  if ((record == nullptr) ||
      (param.value >= static_cast<std::int32_t>(record->paramCount))) {
    return nullptr;
  }
  return &record->params[param.value];
}

/// Stages a value into the param's pending storage (applied once per
/// submit; last write wins, matching GL uniform semantics).
void stage_param(BgfxParamRecord *param, const float *value,
                 std::size_t floatCount, std::uint16_t num) noexcept {
  if ((param == nullptr) || (value == nullptr) || (floatCount == 0U) ||
      (floatCount > 64U)) {
    return;
  }
  std::memcpy(param->pending, value, floatCount * sizeof(float));
  param->pendingNum = num;
  param->dirty = true;
}

/// Sets a padded vec4 uniform from count source floats (bgfx has no
/// scalar/vec2/vec3 uniform types).
void set_padded_vec4(BgfxParamRecord *param, const float *value,
                     std::int32_t count) noexcept {
  if ((param == nullptr) || (value == nullptr) ||
      (param->type != bgfx::UniformType::Vec4)) {
    return;
  }
  float padded[4] = {};
  for (std::int32_t i = 0; i < count; ++i) {
    padded[i] = value[i];
  }
  stage_param(param, padded, 4U, 1U);
}

} // namespace

BgfxProgramRecord *current_program_record() noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (ctx.currentProgram == 0U) {
    return nullptr;
  }
  return ctx.programs.resolve(ctx.currentProgram);
}

void apply_program_uniforms(BgfxProgramRecord &program) noexcept {
  for (std::uint16_t i = 0U; i < program.paramCount; ++i) {
    BgfxParamRecord &param = program.params[i];
    if (!param.dirty || (param.type == bgfx::UniformType::Sampler)) {
      continue;
    }
    bgfx::setUniform(param.handle, param.pending, param.pendingNum);
    param.dirty = false;
  }
}

void apply_program_samplers(const BgfxProgramRecord &program) noexcept {
  BgfxDeviceContext &ctx = device_context();
  for (std::uint16_t i = 0U; i < program.paramCount; ++i) {
    const BgfxParamRecord &param = program.params[i];
    if ((param.type != bgfx::UniformType::Sampler) ||
        (param.samplerStage < 0)) {
      continue;
    }
    const std::uint32_t bound =
        ctx.boundTextures[static_cast<std::size_t>(param.samplerStage)];
    if (bound == 0U) {
      continue;
    }
    BgfxTextureRecord *texture = ctx.textures.resolve(bound);
    if (texture == nullptr) {
      continue;
    }
    bgfx::setTexture(static_cast<std::uint8_t>(param.samplerStage),
                     param.handle, texture->handle);
  }
}

const char *bgfx_cooked_program_profile() noexcept {
  // spirv is canonical: its binaries embed the uniform table this
  // backend's parameter resolution reads (getShaderUniforms), and it is
  // the Vulkan flavor Phase D targets first. GLSL-family flavors carry
  // no embedded uniforms (bgfx's GL renderer introspects at link), so
  // selecting them requires a different resolution path — recorded for
  // the Phase D renderer-type switch.
  return "spirv";
}

DeviceProgramHandle bgfx_create_program(const char *, const char *) noexcept {
  static bool logged = false;
  if (!logged) {
    logged = true;
    core::log_message(core::LogLevel::Info, "render_device",
                      "bgfx backend: runtime GLSL compilation "
                      "unavailable; programs load from the shaderc cook "
                      "(#138 Phase C, caps.cookedPrograms)");
  }
  return kInvalidDeviceProgram;
}

DeviceProgramHandle bgfx_create_program_binary(
    const void *vertexData, std::ptrdiff_t vertexSize,
    const void *fragmentData, std::ptrdiff_t fragmentSize) noexcept {
  if (!has_shader_magic(vertexData, vertexSize, 'V') ||
      !has_shader_magic(fragmentData, fragmentSize, 'F')) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary: not cooked bgfx shader "
                      "binaries");
    return kInvalidDeviceProgram;
  }
  const bgfx::ShaderHandle vertex = bgfx::createShader(
      bgfx::copy(vertexData, static_cast<std::uint32_t>(vertexSize)));
  if (!bgfx::isValid(vertex)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary: vertex shader rejected");
    return kInvalidDeviceProgram;
  }
  const bgfx::ShaderHandle fragment = bgfx::createShader(
      bgfx::copy(fragmentData, static_cast<std::uint32_t>(fragmentSize)));
  if (!bgfx::isValid(fragment)) {
    bgfx::destroy(vertex);
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary: fragment shader rejected");
    return kInvalidDeviceProgram;
  }

  BgfxProgramRecord record{};
  // Uniform handles stay owned by the shaders, which live exactly as
  // long as the program (destroyShaders below), so the table's handles
  // are valid for the record's lifetime.
  collect_shader_params(vertex, &record);
  collect_shader_params(fragment, &record);
  record.handle = bgfx::createProgram(vertex, fragment, true);
  if (!bgfx::isValid(record.handle)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary: program link failed");
    return kInvalidDeviceProgram;
  }
  const std::uint32_t value = device_context().programs.allocate(record);
  if (value == 0U) {
    bgfx::destroy(record.handle);
    drop_operation("create_program_binary: table full");
    return kInvalidDeviceProgram;
  }
  return DeviceProgramHandle{value};
}

void bgfx_destroy_program(DeviceProgramHandle program) noexcept {
  if (program.value == 0U) {
    return;
  }
  BgfxDeviceContext &ctx = device_context();
  BgfxProgramRecord *record = ctx.programs.resolve(program.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  if (ctx.currentProgram == program.value) {
    ctx.currentProgram = 0U;
  }
  bgfx::destroy(record->handle);
  ctx.programs.release(program.value);
}

void bgfx_bind_program(DeviceProgramHandle program) noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (program.value == 0U) {
    ctx.currentProgram = 0U;
    return;
  }
  if (ctx.programs.resolve(program.value) == nullptr) {
    drop_operation("bind_program: stale program");
    return;
  }
  ctx.currentProgram = program.value;
}

ShaderParam bgfx_shader_param(DeviceProgramHandle program,
                              const char *name) noexcept {
  if ((program.value == 0U) || (name == nullptr)) {
    return kInvalidShaderParam;
  }
  BgfxProgramRecord *record =
      device_context().programs.resolve(program.value);
  if (record == nullptr) {
    drop_operation("shader_param: stale program");
    return kInvalidShaderParam;
  }
  for (std::uint16_t i = 0U; i < record->paramCount; ++i) {
    if (std::strncmp(record->params[i].name, name, kMaxParamNameLength) ==
        0) {
      return ShaderParam{static_cast<std::int32_t>(i)};
    }
  }
  return kInvalidShaderParam;
}

void bgfx_set_param_mat4(ShaderParam param, const float *value) noexcept {
  BgfxParamRecord *record = resolve_param(param);
  if ((record == nullptr) || (record->type != bgfx::UniformType::Mat4)) {
    return;
  }
  stage_param(record, value, 16U, 1U);
}

void bgfx_set_param_mat3(ShaderParam param, const float *value) noexcept {
  BgfxParamRecord *record = resolve_param(param);
  if ((record == nullptr) || (record->type != bgfx::UniformType::Mat3)) {
    return;
  }
  stage_param(record, value, 9U, 1U);
}

void bgfx_set_param_f32(ShaderParam param, float value) noexcept {
  set_padded_vec4(resolve_param(param), &value, 1);
}

void bgfx_set_param_i32(ShaderParam param, std::int32_t value) noexcept {
  BgfxParamRecord *record = resolve_param(param);
  if (record == nullptr) {
    return;
  }
  if (record->type == bgfx::UniformType::Sampler) {
    // GL convention: an integer on a sampler assigns its texture slot.
    if ((value >= 0) &&
        (value < static_cast<std::int32_t>(kMaxTextureSlots))) {
      record->samplerStage = static_cast<std::int8_t>(value);
    }
    return;
  }
  const float asFloat = static_cast<float>(value);
  set_padded_vec4(record, &asFloat, 1);
}

void bgfx_set_param_vec2(ShaderParam param, const float *value) noexcept {
  set_padded_vec4(resolve_param(param), value, 2);
}

void bgfx_set_param_vec3(ShaderParam param, const float *value) noexcept {
  set_padded_vec4(resolve_param(param), value, 3);
}

void bgfx_set_param_vec4(ShaderParam param, const float *value) noexcept {
  set_padded_vec4(resolve_param(param), value, 4);
}

void bgfx_set_param_vec4_array(ShaderParam param, const float *values,
                               std::int32_t count) noexcept {
  BgfxParamRecord *record = resolve_param(param);
  if ((record == nullptr) || (count <= 0) || (count > 16) ||
      (record->type != bgfx::UniformType::Vec4)) {
    return;
  }
  stage_param(record, values, static_cast<std::size_t>(count) * 4U,
              static_cast<std::uint16_t>(count));
}

void bgfx_set_param_mat4_array(ShaderParam param, const float *values,
                               std::int32_t count) noexcept {
  BgfxParamRecord *record = resolve_param(param);
  if ((record == nullptr) || (count <= 0) || (count > 4) ||
      (record->type != bgfx::UniformType::Mat4)) {
    return;
  }
  stage_param(record, values, static_cast<std::size_t>(count) * 16U,
              static_cast<std::uint16_t>(count));
}

bool bgfx_bind_program_uniform_block(DeviceProgramHandle, const char *,
                                     std::uint32_t) noexcept {
  return false;
}

} // namespace engine::renderer::bgfx_backend
