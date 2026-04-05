#include "engine/renderer/command_buffer.h"

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/mesh_loader.h"

namespace engine::renderer {

namespace {

constexpr float kDefaultFovRadians = 1.0471975512F;
constexpr float kNearClip = 0.1F;
constexpr float kFarClip = 100.0F;
constexpr float kClearRed = 0.06F;
constexpr float kClearGreen = 0.08F;
constexpr float kClearBlue = 0.12F;

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif

#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif

#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif

#ifndef GL_TRUE
#define GL_TRUE 1
#endif

#ifndef GL_DEPTH_TEST
#define GL_DEPTH_TEST 0x0B71
#endif

#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif

using GlCreateShaderProc = GLuint(APIENTRYP)(GLenum shaderType);
using GlShaderSourceProc = void(APIENTRYP)(GLuint shader,
                                           GLsizei count,
                                           const char *const *strings,
                                           const GLint *lengths);
using GlCompileShaderProc = void(APIENTRYP)(GLuint shader);
using GlGetShaderivProc = void(APIENTRYP)(GLuint shader,
                                          GLenum pname,
                                          GLint *params);
using GlGetShaderInfoLogProc = void(APIENTRYP)(GLuint shader,
                                               GLsizei bufferSize,
                                               GLsizei *length,
                                               char *infoLog);
using GlDeleteShaderProc = void(APIENTRYP)(GLuint shader);
using GlCreateProgramProc = GLuint(APIENTRYP)(void);
using GlAttachShaderProc = void(APIENTRYP)(GLuint program, GLuint shader);
using GlLinkProgramProc = void(APIENTRYP)(GLuint program);
using GlGetProgramivProc = void(APIENTRYP)(GLuint program,
                                           GLenum pname,
                                           GLint *params);
using GlGetProgramInfoLogProc = void(APIENTRYP)(GLuint program,
                                                GLsizei bufferSize,
                                                GLsizei *length,
                                                char *infoLog);
using GlDeleteProgramProc = void(APIENTRYP)(GLuint program);
using GlUseProgramProc = void(APIENTRYP)(GLuint program);
using GlGetUniformLocationProc = GLint(APIENTRYP)(GLuint program,
                                                  const char *name);
using GlUniformMatrix4fvProc = void(APIENTRYP)(GLint location,
                                               GLsizei count,
                                               GLboolean transpose,
                                               const GLfloat *value);
using GlUniformMatrix3fvProc = void(APIENTRYP)(GLint location,
                                               GLsizei count,
                                               GLboolean transpose,
                                               const GLfloat *value);
using GlUniform1fProc = void(APIENTRYP)(GLint location, GLfloat value);
using GlUniform3fvProc = void(APIENTRYP)(GLint location,
                                         GLsizei count,
                                         const GLfloat *value);
using GlBindVertexArrayProc = void(APIENTRYP)(GLuint array);
using GlDrawArraysProc = void(APIENTRYP)(GLenum mode,
                                         GLint first,
                                         GLsizei count);
using GlDrawElementsProc = void(APIENTRYP)(GLenum mode,
                                           GLsizei count,
                                           GLenum type,
                                           const void *indices);
using GlViewportProc = void(APIENTRYP)(GLint x,
                                       GLint y,
                                       GLsizei width,
                                       GLsizei height);
using GlEnableProc = void(APIENTRYP)(GLenum cap);
using GlClearColorProc = void(APIENTRYP)(GLfloat red,
                                         GLfloat green,
                                         GLfloat blue,
                                         GLfloat alpha);
using GlClearProc = void(APIENTRYP)(GLbitfield mask);

struct GlFunctions final {
  GlCreateShaderProc createShader = nullptr;
  GlShaderSourceProc shaderSource = nullptr;
  GlCompileShaderProc compileShader = nullptr;
  GlGetShaderivProc getShaderiv = nullptr;
  GlGetShaderInfoLogProc getShaderInfoLog = nullptr;
  GlDeleteShaderProc deleteShader = nullptr;
  GlCreateProgramProc createProgram = nullptr;
  GlAttachShaderProc attachShader = nullptr;
  GlLinkProgramProc linkProgram = nullptr;
  GlGetProgramivProc getProgramiv = nullptr;
  GlGetProgramInfoLogProc getProgramInfoLog = nullptr;
  GlDeleteProgramProc deleteProgram = nullptr;
  GlUseProgramProc useProgram = nullptr;
  GlGetUniformLocationProc getUniformLocation = nullptr;
  GlUniformMatrix4fvProc uniformMatrix4fv = nullptr;
  GlUniformMatrix3fvProc uniformMatrix3fv = nullptr;
  GlUniform1fProc uniform1f = nullptr;
  GlUniform3fvProc uniform3fv = nullptr;
  GlBindVertexArrayProc bindVertexArray = nullptr;
  GlDrawArraysProc drawArrays = nullptr;
  GlDrawElementsProc drawElements = nullptr;
  GlViewportProc viewport = nullptr;
  GlEnableProc enable = nullptr;
  GlClearColorProc clearColor = nullptr;
  GlClearProc clear = nullptr;
};

struct GlBackendState final {
  bool initialized = false;
  bool failed = false;
  GLuint program = 0U;
  GLint mvpLocation = -1;
  GLint normalMatrixLocation = -1;
  GLint timeLocation = -1;
  GLint albedoLocation = -1;
  GlFunctions gl{};
};

GlBackendState &backend_state() noexcept {
  static GlBackendState state{};
  return state;
}

template <typename T>
bool load_gl_function(T *outFunction, const char *name) noexcept {
  if ((outFunction == nullptr) || (name == nullptr)) {
    return false;
  }

  *outFunction = reinterpret_cast<T>(core::get_gl_proc_address(name));
  return *outFunction != nullptr;
}

bool load_gl_functions(GlFunctions *outGl) noexcept {
  if (outGl == nullptr) {
    return false;
  }

  return load_gl_function(&outGl->createShader, "glCreateShader")
         && load_gl_function(&outGl->shaderSource, "glShaderSource")
         && load_gl_function(&outGl->compileShader, "glCompileShader")
         && load_gl_function(&outGl->getShaderiv, "glGetShaderiv")
         && load_gl_function(&outGl->getShaderInfoLog, "glGetShaderInfoLog")
         && load_gl_function(&outGl->deleteShader, "glDeleteShader")
         && load_gl_function(&outGl->createProgram, "glCreateProgram")
         && load_gl_function(&outGl->attachShader, "glAttachShader")
         && load_gl_function(&outGl->linkProgram, "glLinkProgram")
         && load_gl_function(&outGl->getProgramiv, "glGetProgramiv")
         && load_gl_function(&outGl->getProgramInfoLog, "glGetProgramInfoLog")
         && load_gl_function(&outGl->deleteProgram, "glDeleteProgram")
         && load_gl_function(&outGl->useProgram, "glUseProgram")
         && load_gl_function(&outGl->getUniformLocation, "glGetUniformLocation")
         && load_gl_function(&outGl->uniformMatrix4fv, "glUniformMatrix4fv")
         && load_gl_function(&outGl->uniformMatrix3fv, "glUniformMatrix3fv")
         && load_gl_function(&outGl->uniform1f, "glUniform1f")
         && load_gl_function(&outGl->uniform3fv, "glUniform3fv")
         && load_gl_function(&outGl->bindVertexArray, "glBindVertexArray")
         && load_gl_function(&outGl->drawArrays, "glDrawArrays")
         && load_gl_function(&outGl->drawElements, "glDrawElements")
         && load_gl_function(&outGl->viewport, "glViewport")
         && load_gl_function(&outGl->enable, "glEnable")
         && load_gl_function(&outGl->clearColor, "glClearColor")
         && load_gl_function(&outGl->clear, "glClear");
}

void log_shader_compile_error(const GlFunctions &gl,
                              GLuint shader,
                              const char *stageLabel) noexcept {
  std::array<char, 1024U> logBuffer{};
  GLsizei written = 0;
  gl.getShaderInfoLog(shader,
                      static_cast<GLsizei>(logBuffer.size()),
                      &written,
                      logBuffer.data());

  char message[1200] = {};
  std::snprintf(message,
                sizeof(message),
                "%s shader compile failed: %s",
                stageLabel,
                logBuffer.data());
  core::log_message(core::LogLevel::Error, "renderer", message);
}

void log_program_link_error(const GlFunctions &gl, GLuint program) noexcept {
  std::array<char, 1024U> logBuffer{};
  GLsizei written = 0;
  gl.getProgramInfoLog(program,
                       static_cast<GLsizei>(logBuffer.size()),
                       &written,
                       logBuffer.data());

  char message[1200] = {};
  std::snprintf(
      message, sizeof(message), "shader link failed: %s", logBuffer.data());
  core::log_message(core::LogLevel::Error, "renderer", message);
}

bool compile_shader(const GlFunctions &gl,
                    GLenum stage,
                    const char *source,
                    const char *stageLabel,
                    GLuint *outShader) noexcept {
  if ((source == nullptr) || (outShader == nullptr)) {
    return false;
  }

  *outShader = 0U;
  const GLuint shader = gl.createShader(stage);
  if (shader == 0U) {
    return false;
  }

  const char *sources[] = {source};
  const GLint sourceLength = static_cast<GLint>(std::strlen(source));
  gl.shaderSource(shader, 1, sources, &sourceLength);
  gl.compileShader(shader);

  GLint compiled = 0;
  gl.getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != static_cast<GLint>(GL_TRUE)) {
    log_shader_compile_error(gl, shader, stageLabel);
    gl.deleteShader(shader);
    return false;
  }

  *outShader = shader;
  return true;
}

bool link_program(const GlFunctions &gl,
                  GLuint vertexShader,
                  GLuint fragmentShader,
                  GLuint *outProgram) noexcept {
  if (outProgram == nullptr) {
    return false;
  }

  *outProgram = 0U;
  const GLuint program = gl.createProgram();
  if (program == 0U) {
    return false;
  }

  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);

  GLint linked = 0;
  gl.getProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != static_cast<GLint>(GL_TRUE)) {
    log_program_link_error(gl, program);
    gl.deleteProgram(program);
    return false;
  }

  *outProgram = program;
  return true;
}

void reset_backend_on_failure() noexcept {
  GlBackendState &backend = backend_state();
  backend = GlBackendState{};
  backend.failed = true;
}

bool initialize_backend() noexcept {
  GlBackendState &backend = backend_state();
  if (backend.initialized) {
    return true;
  }

  if (backend.failed) {
    return false;
  }

  if (!load_gl_functions(&backend.gl)) {
    core::log_message(
        core::LogLevel::Error, "renderer", "failed to load OpenGL functions");
    reset_backend_on_failure();
    return false;
  }

  GLuint vertexShader = 0U;
  GLuint fragmentShader = 0U;
  GLuint program = 0U;

  constexpr char kVertexShaderSource[] = R"(#version 450 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
uniform mat4 u_mvp;
uniform mat3 u_normalMatrix;
out vec3 vNormal;

void main() {
  gl_Position = u_mvp * vec4(inPosition, 1.0);
  vNormal = u_normalMatrix * inNormal;
}
)";

  constexpr char kFragmentShaderSource[] = R"(#version 450 core
in vec3 vNormal;
uniform float u_time;
uniform vec3 u_albedo;
out vec4 outColor;

void main() {
  const vec3 lightDirection = normalize(vec3(0.4, 1.0, 0.6));
  const float diffuse = max(dot(normalize(vNormal), lightDirection), 0.0);
  const float pulse = 0.95 + (0.05 * sin(u_time * 0.5));
  const vec3 albedo = u_albedo;
  outColor = vec4(albedo * (diffuse * 0.9 + 0.1) * pulse, 1.0);
}
)";

  if (!compile_shader(backend.gl,
                      GL_VERTEX_SHADER,
                      kVertexShaderSource,
                      "vertex",
                      &vertexShader)
      || !compile_shader(backend.gl,
                         GL_FRAGMENT_SHADER,
                         kFragmentShaderSource,
                         "fragment",
                         &fragmentShader)
      || !link_program(backend.gl, vertexShader, fragmentShader, &program)) {
    if (vertexShader != 0U) {
      backend.gl.deleteShader(vertexShader);
    }
    if (fragmentShader != 0U) {
      backend.gl.deleteShader(fragmentShader);
    }
    reset_backend_on_failure();
    return false;
  }

  backend.gl.deleteShader(vertexShader);
  backend.gl.deleteShader(fragmentShader);

  backend.program = program;
  backend.mvpLocation = backend.gl.getUniformLocation(backend.program, "u_mvp");
  backend.normalMatrixLocation =
      backend.gl.getUniformLocation(backend.program, "u_normalMatrix");
  backend.timeLocation =
      backend.gl.getUniformLocation(backend.program, "u_time");
  backend.albedoLocation =
      backend.gl.getUniformLocation(backend.program, "u_albedo");
  if ((backend.mvpLocation < 0) || (backend.normalMatrixLocation < 0)
      || (backend.albedoLocation < 0)) {
    core::log_message(core::LogLevel::Error,
                      "renderer",
                      "failed to locate required shader uniforms");
    backend.gl.deleteProgram(backend.program);
    reset_backend_on_failure();
    return false;
  }

  backend.initialized = true;
  return true;
}

void destroy_backend_resources(GlBackendState *backend) noexcept {
  if (backend == nullptr) {
    return;
  }

  // This function releases GL-owned resources only; callers perform the full
  // backend state reset according to shutdown/failure flow.

  if ((backend->program != 0U) && (backend->gl.deleteProgram != nullptr)) {
    backend->gl.deleteProgram(backend->program);
    backend->program = 0U;
  }

  backend->mvpLocation = -1;
  backend->normalMatrixLocation = -1;
  backend->timeLocation = -1;
  backend->albedoLocation = -1;
  backend->initialized = false;
}

math::Mat4 compute_model_matrix(const DrawCommand &command) noexcept {
  return command.modelMatrix;
}

math::Mat4 compute_mvp(const math::Mat4 &model,
                       const math::Mat4 &viewProjection) noexcept {
  return math::mul(viewProjection, model);
}

void extract_normal_matrix(const math::Mat4 &model,
                           float *normalMatrixOut) noexcept {
  if (normalMatrixOut == nullptr) {
    return;
  }

  // Normal matrix = transpose(inverse(M)), correct for non-uniform scale.
  // Falls back to copying the upper-left 3x3 when M is singular.
  math::Mat4 invModel{};
  const math::Mat4 normalSource =
      math::inverse(model, &invModel) ? math::transpose(invModel) : model;

  normalMatrixOut[0] = normalSource.columns[0].x;
  normalMatrixOut[1] = normalSource.columns[0].y;
  normalMatrixOut[2] = normalSource.columns[0].z;

  normalMatrixOut[3] = normalSource.columns[1].x;
  normalMatrixOut[4] = normalSource.columns[1].y;
  normalMatrixOut[5] = normalSource.columns[1].z;

  normalMatrixOut[6] = normalSource.columns[2].x;
  normalMatrixOut[7] = normalSource.columns[2].y;
  normalMatrixOut[8] = normalSource.columns[2].z;
}

} // namespace

void CommandBufferBuilder::reset() noexcept {
  m_commandCount = 0U;
}

bool CommandBufferBuilder::submit(const DrawCommand &command) noexcept {
  if (m_commandCount >= kMaxDrawCommands) {
    return false;
  }

  m_commands[m_commandCount] = command;
  ++m_commandCount;
  return true;
}

bool CommandBufferBuilder::append_from(
    const CommandBufferBuilder &other) noexcept {
  if (other.m_commandCount == 0U) {
    return true;
  }

  if ((m_commandCount + other.m_commandCount) > kMaxDrawCommands) {
    return false;
  }

  std::memcpy(m_commands.data() + m_commandCount,
              other.m_commands.data(),
              sizeof(DrawCommand) * other.m_commandCount);
  m_commandCount += other.m_commandCount;

  return true;
}

void CommandBufferBuilder::sort_by_entity() noexcept {
  std::sort(m_commands.begin(),
            m_commands.begin() + static_cast<std::ptrdiff_t>(m_commandCount),
            [](const DrawCommand &lhs, const DrawCommand &rhs) {
              return lhs.entity < rhs.entity;
            });
}

std::size_t CommandBufferBuilder::command_count() const noexcept {
  return m_commandCount;
}

CommandBufferView CommandBufferBuilder::view() const noexcept {
  CommandBufferView commandBufferView{};
  commandBufferView.data = m_commands.data();
  commandBufferView.count = static_cast<std::uint32_t>(m_commandCount);
  return commandBufferView;
}

void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry,
                    float timeSeconds) noexcept {
  if (!initialize_backend()) {
    return;
  }

  GlBackendState &backend = backend_state();

  int drawableWidth = 1280;
  int drawableHeight = 720;
  core::render_drawable_size(&drawableWidth, &drawableHeight);
  if (drawableWidth <= 0) {
    drawableWidth = 1;
  }
  if (drawableHeight <= 0) {
    drawableHeight = 1;
  }

  backend.gl.viewport(0, 0, drawableWidth, drawableHeight);
  backend.gl.enable(GL_DEPTH_TEST);
  backend.gl.clearColor(kClearRed, kClearGreen, kClearBlue, 1.0F);
  backend.gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (registry == nullptr) {
    return;
  }

  backend.gl.useProgram(backend.program);

  std::uint32_t boundVertexArray = 0U;

  const float aspect =
      static_cast<float>(drawableWidth) / static_cast<float>(drawableHeight);
  const math::Vec3 eye(
      std::sin(timeSeconds) * 3.0F, 1.5F, std::cos(timeSeconds) * 3.0F);
  const math::Mat4 view = math::look_at(
      eye, math::Vec3(0.0F, 0.0F, 0.0F), math::Vec3(0.0F, 1.0F, 0.0F));
  const math::Mat4 projection =
      math::perspective(kDefaultFovRadians, aspect, kNearClip, kFarClip);
  const math::Mat4 viewProjection = math::mul(projection, view);
  if (backend.timeLocation >= 0) {
    backend.gl.uniform1f(backend.timeLocation, timeSeconds);
  }

  if ((commandBufferView.count > 0U) && (commandBufferView.data == nullptr)) {
    core::log_message(
        core::LogLevel::Error, "renderer", "draw command view is invalid");
  }

  if ((commandBufferView.data != nullptr) && (commandBufferView.count > 0U)) {
    const std::size_t drawCount =
        static_cast<std::size_t>(commandBufferView.count);
    for (std::size_t i = 0U; i < drawCount; ++i) {
      const DrawCommand &command = commandBufferView.data[i];
      const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
      if ((mesh == nullptr) || (mesh->vertexArray == 0U)
          || (mesh->vertexCount == 0U)) {
        continue;
      }

      if (mesh->vertexArray != boundVertexArray) {
        backend.gl.bindVertexArray(mesh->vertexArray);
        boundVertexArray = mesh->vertexArray;
      }

      if (backend.albedoLocation >= 0) {
        backend.gl.uniform3fv(
            backend.albedoLocation, 1, &command.material.albedo.x);
      }

      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);
      backend.gl.uniformMatrix4fv(
          backend.mvpLocation, 1, GL_FALSE, &mvp.columns[0].x);
      backend.gl.uniformMatrix3fv(
          backend.normalMatrixLocation, 1, GL_FALSE, normalMatrix);

      if (mesh->indexCount > 0U) {
        backend.gl.drawElements(GL_TRIANGLES,
                                static_cast<GLsizei>(mesh->indexCount),
                                GL_UNSIGNED_INT,
                                nullptr);
      } else {
        backend.gl.drawArrays(
            GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertexCount));
      }
    }
  }

  backend.gl.bindVertexArray(0U);
  backend.gl.useProgram(0U);
}

void shutdown_renderer() noexcept {
  GlBackendState &backend = backend_state();
  if (!backend.initialized && !backend.failed) {
    return;
  }

  if (core::make_render_context_current()) {
    destroy_backend_resources(&backend);
    core::release_render_context();
  }

  backend = GlBackendState{};
}

} // namespace engine::renderer
