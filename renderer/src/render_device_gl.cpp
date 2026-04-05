#include "engine/renderer/render_device.h"

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include <array>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::renderer {

namespace {

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

#ifndef GL_TRUE
#define GL_TRUE 1
#endif

#ifndef GL_DEPTH_TEST
#define GL_DEPTH_TEST 0x0B71
#endif

#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif

#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif

#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif

// --- GL proc types ---

using GlCreateShaderProc = GLuint(APIENTRYP)(GLenum);
using GlShaderSourceProc = void(APIENTRYP)(GLuint,
                                           GLsizei,
                                           const char *const *,
                                           const GLint *);
using GlCompileShaderProc = void(APIENTRYP)(GLuint);
using GlGetShaderivProc = void(APIENTRYP)(GLuint, GLenum, GLint *);
using GlGetShaderInfoLogProc = void(APIENTRYP)(GLuint,
                                               GLsizei,
                                               GLsizei *,
                                               char *);
using GlDeleteShaderProc = void(APIENTRYP)(GLuint);
using GlCreateProgramProc = GLuint(APIENTRYP)(void);
using GlAttachShaderProc = void(APIENTRYP)(GLuint, GLuint);
using GlLinkProgramProc = void(APIENTRYP)(GLuint);
using GlGetProgramivProc = void(APIENTRYP)(GLuint, GLenum, GLint *);
using GlGetProgramInfoLogProc = void(APIENTRYP)(GLuint,
                                                GLsizei,
                                                GLsizei *,
                                                char *);
using GlDeleteProgramProc = void(APIENTRYP)(GLuint);
using GlUseProgramProc = void(APIENTRYP)(GLuint);
using GlGetUniformLocationProc = GLint(APIENTRYP)(GLuint, const char *);
using GlUniformMatrix4fvProc = void(APIENTRYP)(GLint,
                                               GLsizei,
                                               GLboolean,
                                               const GLfloat *);
using GlUniformMatrix3fvProc = void(APIENTRYP)(GLint,
                                               GLsizei,
                                               GLboolean,
                                               const GLfloat *);
using GlUniform1fProc = void(APIENTRYP)(GLint, GLfloat);
using GlUniform3fvProc = void(APIENTRYP)(GLint, GLsizei, const GLfloat *);
using GlGenVertexArraysProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlBindVertexArrayProc = void(APIENTRYP)(GLuint);
using GlDeleteVertexArraysProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlGenBuffersProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlBindBufferProc = void(APIENTRYP)(GLenum, GLuint);
using GlBufferDataProc = void(APIENTRYP)(GLenum,
                                         std::ptrdiff_t,
                                         const void *,
                                         GLenum);
using GlDeleteBuffersProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlEnableVertexAttribArrayProc = void(APIENTRYP)(GLuint);
using GlVertexAttribPointerProc =
    void(APIENTRYP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
using GlDrawArraysProc = void(APIENTRYP)(GLenum, GLint, GLsizei);
using GlDrawElementsProc = void(APIENTRYP)(GLenum,
                                           GLsizei,
                                           GLenum,
                                           const void *);
using GlViewportProc = void(APIENTRYP)(GLint, GLint, GLsizei, GLsizei);
using GlEnableProc = void(APIENTRYP)(GLenum);
using GlClearColorProc = void(APIENTRYP)(GLfloat, GLfloat, GLfloat, GLfloat);
using GlClearProc = void(APIENTRYP)(GLbitfield);

// --- Consolidated GL function table ---

struct GlTable final {
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
  GlGenVertexArraysProc genVertexArrays = nullptr;
  GlBindVertexArrayProc bindVertexArray = nullptr;
  GlDeleteVertexArraysProc deleteVertexArrays = nullptr;
  GlGenBuffersProc genBuffers = nullptr;
  GlBindBufferProc bindBuffer = nullptr;
  GlBufferDataProc bufferData = nullptr;
  GlDeleteBuffersProc deleteBuffers = nullptr;
  GlEnableVertexAttribArrayProc enableVertexAttribArray = nullptr;
  GlVertexAttribPointerProc vertexAttribPointer = nullptr;
  GlDrawArraysProc drawArrays = nullptr;
  GlDrawElementsProc drawElements = nullptr;
  GlViewportProc viewport = nullptr;
  GlEnableProc enable = nullptr;
  GlClearColorProc clearColor = nullptr;
  GlClearProc clear = nullptr;
};

bool g_deviceInitialized = false;
GlTable g_gl{};
RenderDevice g_device{};

template <typename T> bool load_proc(T *out, const char *name) noexcept {
  *out = reinterpret_cast<T>(core::get_gl_proc_address(name));
  return *out != nullptr;
}

bool load_all_gl_functions() noexcept {
  return load_proc(&g_gl.createShader, "glCreateShader")
         && load_proc(&g_gl.shaderSource, "glShaderSource")
         && load_proc(&g_gl.compileShader, "glCompileShader")
         && load_proc(&g_gl.getShaderiv, "glGetShaderiv")
         && load_proc(&g_gl.getShaderInfoLog, "glGetShaderInfoLog")
         && load_proc(&g_gl.deleteShader, "glDeleteShader")
         && load_proc(&g_gl.createProgram, "glCreateProgram")
         && load_proc(&g_gl.attachShader, "glAttachShader")
         && load_proc(&g_gl.linkProgram, "glLinkProgram")
         && load_proc(&g_gl.getProgramiv, "glGetProgramiv")
         && load_proc(&g_gl.getProgramInfoLog, "glGetProgramInfoLog")
         && load_proc(&g_gl.deleteProgram, "glDeleteProgram")
         && load_proc(&g_gl.useProgram, "glUseProgram")
         && load_proc(&g_gl.getUniformLocation, "glGetUniformLocation")
         && load_proc(&g_gl.uniformMatrix4fv, "glUniformMatrix4fv")
         && load_proc(&g_gl.uniformMatrix3fv, "glUniformMatrix3fv")
         && load_proc(&g_gl.uniform1f, "glUniform1f")
         && load_proc(&g_gl.uniform3fv, "glUniform3fv")
         && load_proc(&g_gl.genVertexArrays, "glGenVertexArrays")
         && load_proc(&g_gl.bindVertexArray, "glBindVertexArray")
         && load_proc(&g_gl.deleteVertexArrays, "glDeleteVertexArrays")
         && load_proc(&g_gl.genBuffers, "glGenBuffers")
         && load_proc(&g_gl.bindBuffer, "glBindBuffer")
         && load_proc(&g_gl.bufferData, "glBufferData")
         && load_proc(&g_gl.deleteBuffers, "glDeleteBuffers")
         && load_proc(&g_gl.enableVertexAttribArray,
                      "glEnableVertexAttribArray")
         && load_proc(&g_gl.vertexAttribPointer, "glVertexAttribPointer")
         && load_proc(&g_gl.drawArrays, "glDrawArrays")
         && load_proc(&g_gl.drawElements, "glDrawElements")
         && load_proc(&g_gl.viewport, "glViewport")
         && load_proc(&g_gl.enable, "glEnable")
         && load_proc(&g_gl.clearColor, "glClearColor")
         && load_proc(&g_gl.clear, "glClear");
}

// --- Device function implementations ---

std::uint32_t gl_create_shader(std::uint32_t stage,
                               const char *source) noexcept {
  const GLenum glStage =
      (stage == kShaderStageVertex) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
  const GLuint shader = g_gl.createShader(glStage);
  if (shader == 0U) {
    return 0U;
  }

  const char *sources[] = {source};
  const GLint len = static_cast<GLint>(std::strlen(source));
  g_gl.shaderSource(shader, 1, sources, &len);
  g_gl.compileShader(shader);

  GLint compiled = 0;
  g_gl.getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != static_cast<GLint>(GL_TRUE)) {
    std::array<char, 1024U> log{};
    GLsizei written = 0;
    g_gl.getShaderInfoLog(
        shader, static_cast<GLsizei>(log.size()), &written, log.data());
    char msg[1200] = {};
    const char *label = (stage == kShaderStageVertex) ? "vertex" : "fragment";
    std::snprintf(
        msg, sizeof(msg), "%s shader compile failed: %s", label, log.data());
    core::log_message(core::LogLevel::Error, "renderer", msg);
    g_gl.deleteShader(shader);
    return 0U;
  }

  return static_cast<std::uint32_t>(shader);
}

void gl_destroy_shader(std::uint32_t shader) noexcept {
  if (shader != 0U) {
    g_gl.deleteShader(static_cast<GLuint>(shader));
  }
}

std::uint32_t gl_link_program(std::uint32_t vertShader,
                              std::uint32_t fragShader) noexcept {
  const GLuint program = g_gl.createProgram();
  if (program == 0U) {
    return 0U;
  }

  g_gl.attachShader(program, static_cast<GLuint>(vertShader));
  g_gl.attachShader(program, static_cast<GLuint>(fragShader));
  g_gl.linkProgram(program);

  GLint linked = 0;
  g_gl.getProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != static_cast<GLint>(GL_TRUE)) {
    std::array<char, 1024U> log{};
    GLsizei written = 0;
    g_gl.getProgramInfoLog(
        program, static_cast<GLsizei>(log.size()), &written, log.data());
    char msg[1200] = {};
    std::snprintf(msg, sizeof(msg), "shader link failed: %s", log.data());
    core::log_message(core::LogLevel::Error, "renderer", msg);
    g_gl.deleteProgram(program);
    return 0U;
  }

  return static_cast<std::uint32_t>(program);
}

void gl_destroy_program(std::uint32_t program) noexcept {
  if (program != 0U) {
    g_gl.deleteProgram(static_cast<GLuint>(program));
  }
}

void gl_bind_program(std::uint32_t program) noexcept {
  g_gl.useProgram(static_cast<GLuint>(program));
}

std::int32_t gl_uniform_location(std::uint32_t program,
                                 const char *name) noexcept {
  return static_cast<std::int32_t>(
      g_gl.getUniformLocation(static_cast<GLuint>(program), name));
}

void gl_set_uniform_mat4(std::int32_t loc, const float *value) noexcept {
  g_gl.uniformMatrix4fv(static_cast<GLint>(loc), 1, GL_FALSE, value);
}

void gl_set_uniform_mat3(std::int32_t loc, const float *value) noexcept {
  g_gl.uniformMatrix3fv(static_cast<GLint>(loc), 1, GL_FALSE, value);
}

void gl_set_uniform_float(std::int32_t loc, float value) noexcept {
  g_gl.uniform1f(static_cast<GLint>(loc), value);
}

void gl_set_uniform_vec3(std::int32_t loc, const float *value) noexcept {
  g_gl.uniform3fv(static_cast<GLint>(loc), 1, value);
}

std::uint32_t gl_create_vertex_array() noexcept {
  GLuint vao = 0U;
  g_gl.genVertexArrays(1, &vao);
  return static_cast<std::uint32_t>(vao);
}

void gl_destroy_vertex_array(std::uint32_t vao) noexcept {
  if (vao != 0U) {
    const GLuint id = static_cast<GLuint>(vao);
    g_gl.deleteVertexArrays(1, &id);
  }
}

void gl_bind_vertex_array(std::uint32_t vao) noexcept {
  g_gl.bindVertexArray(static_cast<GLuint>(vao));
}

std::uint32_t gl_create_buffer() noexcept {
  GLuint buf = 0U;
  g_gl.genBuffers(1, &buf);
  return static_cast<std::uint32_t>(buf);
}

void gl_destroy_buffer(std::uint32_t buffer) noexcept {
  if (buffer != 0U) {
    const GLuint id = static_cast<GLuint>(buffer);
    g_gl.deleteBuffers(1, &id);
  }
}

void gl_bind_array_buffer(std::uint32_t buffer) noexcept {
  g_gl.bindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
}

void gl_bind_element_buffer(std::uint32_t buffer) noexcept {
  g_gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(buffer));
}

void gl_buffer_data_array(const void *data, std::ptrdiff_t sizeBytes) noexcept {
  g_gl.bufferData(GL_ARRAY_BUFFER, sizeBytes, data, GL_STATIC_DRAW);
}

void gl_buffer_data_element(const void *data,
                            std::ptrdiff_t sizeBytes) noexcept {
  g_gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, sizeBytes, data, GL_STATIC_DRAW);
}

void gl_enable_vertex_attrib(std::uint32_t index) noexcept {
  g_gl.enableVertexAttribArray(static_cast<GLuint>(index));
}

void gl_vertex_attrib_float(std::uint32_t index,
                            std::int32_t components,
                            std::int32_t stride,
                            const void *offset) noexcept {
  g_gl.vertexAttribPointer(static_cast<GLuint>(index),
                           static_cast<GLint>(components),
                           GL_FLOAT,
                           GL_FALSE,
                           static_cast<GLsizei>(stride),
                           offset);
}

void gl_draw_arrays_triangles(std::int32_t first, std::int32_t count) noexcept {
  g_gl.drawArrays(
      GL_TRIANGLES, static_cast<GLint>(first), static_cast<GLsizei>(count));
}

void gl_draw_elements_triangles_u32(std::int32_t count) noexcept {
  g_gl.drawElements(
      GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr);
}

void gl_set_viewport(std::int32_t x,
                     std::int32_t y,
                     std::int32_t w,
                     std::int32_t h) noexcept {
  g_gl.viewport(static_cast<GLint>(x),
                static_cast<GLint>(y),
                static_cast<GLsizei>(w),
                static_cast<GLsizei>(h));
}

void gl_enable_depth_test() noexcept {
  g_gl.enable(GL_DEPTH_TEST);
}

void gl_set_clear_color(float r, float g, float b, float a) noexcept {
  g_gl.clearColor(r, g, b, a);
}

void gl_clear_color_depth() noexcept {
  g_gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

} // namespace

bool initialize_render_device() noexcept {
  if (g_deviceInitialized) {
    return true;
  }

  if (!load_all_gl_functions()) {
    core::log_message(core::LogLevel::Error,
                      "renderer",
                      "failed to load OpenGL functions for render device");
    return false;
  }

  g_device.create_shader = &gl_create_shader;
  g_device.destroy_shader = &gl_destroy_shader;
  g_device.link_program = &gl_link_program;
  g_device.destroy_program = &gl_destroy_program;
  g_device.bind_program = &gl_bind_program;
  g_device.uniform_location = &gl_uniform_location;
  g_device.set_uniform_mat4 = &gl_set_uniform_mat4;
  g_device.set_uniform_mat3 = &gl_set_uniform_mat3;
  g_device.set_uniform_float = &gl_set_uniform_float;
  g_device.set_uniform_vec3 = &gl_set_uniform_vec3;
  g_device.create_vertex_array = &gl_create_vertex_array;
  g_device.destroy_vertex_array = &gl_destroy_vertex_array;
  g_device.bind_vertex_array = &gl_bind_vertex_array;
  g_device.create_buffer = &gl_create_buffer;
  g_device.destroy_buffer = &gl_destroy_buffer;
  g_device.bind_array_buffer = &gl_bind_array_buffer;
  g_device.bind_element_buffer = &gl_bind_element_buffer;
  g_device.buffer_data_array = &gl_buffer_data_array;
  g_device.buffer_data_element = &gl_buffer_data_element;
  g_device.enable_vertex_attrib = &gl_enable_vertex_attrib;
  g_device.vertex_attrib_float = &gl_vertex_attrib_float;
  g_device.draw_arrays_triangles = &gl_draw_arrays_triangles;
  g_device.draw_elements_triangles_u32 = &gl_draw_elements_triangles_u32;
  g_device.set_viewport = &gl_set_viewport;
  g_device.enable_depth_test = &gl_enable_depth_test;
  g_device.set_clear_color = &gl_set_clear_color;
  g_device.clear_color_depth = &gl_clear_color_depth;

  g_deviceInitialized = true;
  return true;
}

void shutdown_render_device() noexcept {
  g_device = RenderDevice{};
  g_gl = GlTable{};
  g_deviceInitialized = false;
}

const RenderDevice *render_device() noexcept {
  if (!g_deviceInitialized) {
    return nullptr;
  }
  return &g_device;
}

} // namespace engine::renderer
