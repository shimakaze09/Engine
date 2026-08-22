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

/// Names bgfx reserves for predefined uniforms; the cook rejects them in
/// engine shaders, and the sidecar walk skips them the way bgfx's own
/// container parse does.
bool is_predefined_uniform_name(const char *name) noexcept {
  static constexpr const char *kPredefined[] = {
      "u_viewRect",    "u_viewTexel", "u_view",          "u_invView",
      "u_proj",        "u_invProj",   "u_viewProj",      "u_invViewProj",
      "u_model",       "u_modelView", "u_modelViewProj", "u_alphaRef4",
  };
  for (const char *candidate : kPredefined) {
    if (std::strcmp(name, candidate) == 0) {
      return true;
    }
  }
  return false;
}

/// Finds or creates the global registry entry for a uniform name. The
/// registry owns its own bgfx handle (created here; bgfx refcounts by
/// name) so entries stay valid after any single program is destroyed.
/// Returns kMaxGlobalUniforms when the registry is full or the name
/// re-registers under a conflicting type (logged, dropped).
std::size_t find_or_create_global_uniform(const char *name,
                                          bgfx::UniformType::Enum type,
                                          std::uint16_t num) noexcept {
  BgfxDeviceContext &ctx = device_context();
  for (std::uint16_t i = 0U; i < ctx.uniformCount; ++i) {
    if (std::strncmp(ctx.uniforms[i].name, name, kMaxParamNameLength) != 0) {
      continue;
    }
    if (ctx.uniforms[i].type != type) {
      drop_operation("uniform registry: type conflict for name");
      return kMaxGlobalUniforms;
    }
    if (num > ctx.uniforms[i].declaredNum) {
      // A later program declares a larger array; recreate the handle at
      // the larger count so uploads are never clamped.
      if (bgfx::isValid(ctx.uniforms[i].handle)) {
        bgfx::destroy(ctx.uniforms[i].handle);
      }
      ctx.uniforms[i].handle = bgfx::createUniform(name, type, num);
      ctx.uniforms[i].declaredNum = num;
    }
    return i;
  }
  if (ctx.uniformCount >= kMaxGlobalUniforms) {
    drop_operation("uniform registry: table full");
    return kMaxGlobalUniforms;
  }
  BgfxGlobalUniform &entry = ctx.uniforms[ctx.uniformCount];
  entry = BgfxGlobalUniform{};
  core::copy_string(entry.name, kMaxParamNameLength, name);
  entry.type = type;
  entry.declaredNum = (num > 0U) ? num : 1U;
  entry.handle = bgfx::createUniform(name, type, entry.declaredNum);
  return ctx.uniformCount++;
}

/// Adds one global-registry index to a program's declared-uniform list
/// (deduplicated; the list only answers shader_param lookups).
void record_program_param(BgfxProgramRecord *record,
                          std::size_t globalIndex) noexcept {
  if (globalIndex >= kMaxGlobalUniforms) {
    return;
  }
  for (std::uint16_t existing = 0U; existing < record->paramCount;
       ++existing) {
    if (record->paramIndices[existing] ==
        static_cast<std::uint16_t>(globalIndex)) {
      return;
    }
  }
  if (record->paramCount >= kMaxProgramParams) {
    drop_operation("create_program_binary: parameter table full");
    return;
  }
  record->paramIndices[record->paramCount] =
      static_cast<std::uint16_t>(globalIndex);
  ++record->paramCount;
}

/// Walks a cooked shader binary's uniform table on the CPU and merges
/// its entries into the program's parameter table (deduplicated by
/// name). This is how the spirv sidecars donate uniform metadata for
/// glsl/essl programs: the sidecar must never reach bgfx::createShader,
/// because the GL backend would queue a real GL compile of the SPIR-V
/// blob and fatal at the next frame.
void collect_sidecar_params(const void *data, std::ptrdiff_t size,
                            BgfxProgramRecord *record) noexcept {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  const auto total = static_cast<std::size_t>(size);
  // Container: magic('x','S','H',version), hashIn u32, hashOut u32
  // (version >= 6), then u16 count of table entries.
  if (total < 4U) {
    return;
  }
  const std::uint8_t version = bytes[3];
  std::size_t off = 4U + 4U + ((version >= 6U) ? 4U : 0U);
  if ((off + 2U) > total) {
    return;
  }
  std::uint16_t count = 0U;
  std::memcpy(&count, bytes + off, sizeof(count));
  off += 2U;
  for (std::uint16_t i = 0U; i < count; ++i) {
    if ((off + 1U) > total) {
      return;
    }
    const std::uint8_t nameSize = bytes[off];
    off += 1U;
    // Entry tail: type u8, num u8, regIndex u16, regCount u16, then
    // texComponent+texDimension (version >= 8) and texFormat u16
    // (version >= 10).
    const std::size_t tailSize =
        6U + ((version >= 8U) ? 2U : 0U) + ((version >= 10U) ? 2U : 0U);
    if ((off + nameSize + tailSize) > total) {
      return;
    }
    char name[kMaxParamNameLength] = {};
    const std::size_t copyLen =
        (nameSize < (kMaxParamNameLength - 1U)) ? nameSize
                                                : (kMaxParamNameLength - 1U);
    std::memcpy(name, bytes + off, copyLen);
    off += nameSize;
    const std::uint8_t rawType = bytes[off];
    const std::uint8_t num = bytes[off + 1U];
    off += tailSize;
    // Low nibble is the bgfx::UniformType; 0x10/0x20/0x40/0x80 are the
    // fragment/sampler/read-only/compare bits. End entries are spirv
    // descriptor-binding records, not settable uniforms.
    const auto baseType =
        static_cast<bgfx::UniformType::Enum>(rawType & 0x0FU);
    if ((baseType == bgfx::UniformType::End) ||
        is_predefined_uniform_name(name)) {
      continue;
    }
    record_program_param(
        record, find_or_create_global_uniform(
                    name, baseType,
                    static_cast<std::uint16_t>((num > 0U) ? num : 1U)));
  }
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
    // The registry creates its own handle for the name: the shader-owned
    // handle dies with the program, the registry's survives it.
    record_program_param(record, find_or_create_global_uniform(
                                     info.name, info.type, info.num));
  }
}

/// Global-registry entry behind a token; nullptr for invalid tokens
/// (defined no-op per the contract). Tokens are registry indices, so
/// resolution does not depend on which program is currently bound.
BgfxGlobalUniform *resolve_param(ShaderParam param) noexcept {
  if (!param.valid()) {
    return nullptr;
  }
  BgfxDeviceContext &ctx = device_context();
  if (param.value >= static_cast<std::int32_t>(ctx.uniformCount)) {
    return nullptr;
  }
  return &ctx.uniforms[param.value];
}

/// Stages a value into the entry's pending storage and enrolls it in
/// the context's dirty list (applied once per submit; last write wins,
/// matching GL uniform semantics).
void stage_param(BgfxGlobalUniform *param, const float *value,
                 std::size_t floatCount, std::uint16_t num) noexcept {
  if ((param == nullptr) || (value == nullptr) || (floatCount == 0U) ||
      (floatCount > 128U)) {
    return;
  }
  std::memcpy(param->pending, value, floatCount * sizeof(float));
  param->pendingNum = num;
  if (!param->dirty) {
    param->dirty = true;
    BgfxDeviceContext &ctx = device_context();
    ctx.dirtyUniforms[ctx.dirtyUniformCount++] =
        static_cast<std::uint16_t>(param - ctx.uniforms);
  }
}

/// Sets a padded vec4 uniform from count source floats (bgfx has no
/// scalar/vec2/vec3 uniform types).
void set_padded_vec4(BgfxGlobalUniform *param, const float *value,
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

void apply_staged_uniforms() noexcept {
  BgfxDeviceContext &ctx = device_context();
  for (std::uint16_t i = 0U; i < ctx.dirtyUniformCount; ++i) {
    BgfxGlobalUniform &param = ctx.uniforms[ctx.dirtyUniforms[i]];
    param.dirty = false;
    // pendingNum 0 marks an entry whose value was applied immediately
    // (oversized array uploads); samplers carry no float payload.
    if ((param.pendingNum == 0U) ||
        (param.type == bgfx::UniformType::Sampler)) {
      continue;
    }
    bgfx::setUniform(param.handle, param.pending, param.pendingNum);
  }
  ctx.dirtyUniformCount = 0U;
}

void apply_program_samplers(const BgfxProgramRecord &program) noexcept {
  BgfxDeviceContext &ctx = device_context();
  for (std::uint16_t i = 0U; i < program.paramCount; ++i) {
    const BgfxGlobalUniform &param = ctx.uniforms[program.paramIndices[i]];
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

void reset_global_uniforms() noexcept {
  BgfxDeviceContext &ctx = device_context();
  for (std::uint16_t i = 0U; i < ctx.uniformCount; ++i) {
    if (bgfx::isValid(ctx.uniforms[i].handle)) {
      bgfx::destroy(ctx.uniforms[i].handle);
    }
    ctx.uniforms[i] = BgfxGlobalUniform{};
  }
  ctx.uniformCount = 0U;
  ctx.dirtyUniformCount = 0U;
}

const char *bgfx_cooked_program_profile() noexcept {
  // Selected by the live renderer. GLSL-family flavors carry no
  // embedded uniform table, so their programs link through the
  // introspected entry with spirv sidecars.
  switch (bgfx::getRendererType()) {
  case bgfx::RendererType::OpenGLES:
    return "essl";
  case bgfx::RendererType::OpenGL:
    return "glsl";
  case bgfx::RendererType::Metal:
    return "metal";
  case bgfx::RendererType::Direct3D11:
  case bgfx::RendererType::Direct3D12:
    return "dx11";
  default:
    return "spirv";
  }
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

DeviceProgramHandle bgfx_create_program_binary_introspected(
    const void *vertexData, std::ptrdiff_t vertexSize,
    const void *fragmentData, std::ptrdiff_t fragmentSize,
    const void *vertexMeta, std::ptrdiff_t vertexMetaSize,
    const void *fragmentMeta, std::ptrdiff_t fragmentMetaSize) noexcept {
  if ((vertexMeta == nullptr) || (fragmentMeta == nullptr)) {
    return bgfx_create_program_binary(vertexData, vertexSize, fragmentData,
                                      fragmentSize);
  }
  if (!has_shader_magic(vertexData, vertexSize, 'V') ||
      !has_shader_magic(fragmentData, fragmentSize, 'F') ||
      !has_shader_magic(vertexMeta, vertexMetaSize, 'V') ||
      !has_shader_magic(fragmentMeta, fragmentMetaSize, 'F')) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary_introspected: not cooked "
                      "bgfx shader binaries");
    return kInvalidDeviceProgram;
  }

  // The sidecars exist only to donate their embedded uniform tables,
  // parsed on the CPU: the names/types/array counts re-create as
  // backend-owned uniforms (stable refs). The sidecar bytes never reach
  // bgfx::createShader — a GL device would queue a real compile of the
  // SPIR-V blob and hit a fatal at the next frame.
  BgfxProgramRecord record{};
  collect_sidecar_params(vertexMeta, vertexMetaSize, &record);
  collect_sidecar_params(fragmentMeta, fragmentMetaSize, &record);

  const bgfx::ShaderHandle vertex = bgfx::createShader(
      bgfx::copy(vertexData, static_cast<std::uint32_t>(vertexSize)));
  if (!bgfx::isValid(vertex)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary_introspected: vertex shader "
                      "rejected");
    return kInvalidDeviceProgram;
  }
  const bgfx::ShaderHandle fragment = bgfx::createShader(
      bgfx::copy(fragmentData, static_cast<std::uint32_t>(fragmentSize)));
  if (!bgfx::isValid(fragment)) {
    bgfx::destroy(vertex);
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary_introspected: fragment "
                      "shader rejected");
    return kInvalidDeviceProgram;
  }
  record.handle = bgfx::createProgram(vertex, fragment, true);
  if (!bgfx::isValid(record.handle)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "create_program_binary_introspected: link failed");
    return kInvalidDeviceProgram;
  }
  const std::uint32_t value = device_context().programs.allocate(record);
  if (value == 0U) {
    bgfx::destroy(record.handle);
    drop_operation("create_program_binary_introspected: table full");
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
  // Uniform handles belong to the context's global registry (device
  // lifetime); only the program object dies here.
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
  const BgfxDeviceContext &ctx = device_context();
  for (std::uint16_t i = 0U; i < record->paramCount; ++i) {
    const std::uint16_t globalIndex = record->paramIndices[i];
    if (std::strncmp(ctx.uniforms[globalIndex].name, name,
                     kMaxParamNameLength) == 0) {
      // Tokens are global-registry indices: setting through them is
      // correct regardless of which program is bound at set time.
      return ShaderParam{static_cast<std::int32_t>(globalIndex)};
    }
  }
  return kInvalidShaderParam;
}

void bgfx_set_param_mat4(ShaderParam param, const float *value) noexcept {
  BgfxGlobalUniform *record = resolve_param(param);
  if ((record == nullptr) || (record->type != bgfx::UniformType::Mat4)) {
    return;
  }
  stage_param(record, value, 16U, 1U);
}

void bgfx_set_param_mat3(ShaderParam param, const float *value) noexcept {
  BgfxGlobalUniform *record = resolve_param(param);
  if ((record == nullptr) || (record->type != bgfx::UniformType::Mat3)) {
    return;
  }
  stage_param(record, value, 9U, 1U);
}

void bgfx_set_param_f32(ShaderParam param, float value) noexcept {
  set_padded_vec4(resolve_param(param), &value, 1);
}

void bgfx_set_param_i32(ShaderParam param, std::int32_t value) noexcept {
  BgfxGlobalUniform *record = resolve_param(param);
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
  BgfxGlobalUniform *record = resolve_param(param);
  if ((record == nullptr) || (count <= 0) || (count > 32) ||
      (record->type != bgfx::UniformType::Vec4)) {
    return;
  }
  stage_param(record, values, static_cast<std::size_t>(count) * 4U,
              static_cast<std::uint16_t>(count));
}

void bgfx_set_param_mat4_array(ShaderParam param, const float *values,
                               std::int32_t count) noexcept {
  BgfxGlobalUniform *record = resolve_param(param);
  if ((record == nullptr) || (values == nullptr) || (count <= 0) ||
      (count > 128) || (record->type != bgfx::UniformType::Mat4)) {
    return;
  }
  // Applied immediately: bone palettes (mat4[128]) exceed the staging
  // cap, and array uploads happen once per pass/draw by design (the
  // once-per-draw bgfx rule holds without staging). Clearing the
  // pending payload keeps a previously staged small value from
  // re-applying over this upload at submit.
  bgfx::setUniform(record->handle, values,
                   static_cast<std::uint16_t>(count));
  record->pendingNum = 0U;
}

bool bgfx_bind_program_uniform_block(DeviceProgramHandle, const char *,
                                     std::uint32_t) noexcept {
  return false;
}

} // namespace engine::renderer::bgfx_backend
