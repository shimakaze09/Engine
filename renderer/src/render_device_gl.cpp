// OpenGL implementation of the engine-facing render-device contract.
// All GL mechanics live here: VAO/FBO objects, uniform locations,
// texture units, buffer bind targets, and GL enums are private to this
// backend and are addressed from above only through generational
// handles and backend-neutral descriptors (#165).

#include "engine/renderer/render_device.h"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "device_slot_table.h"
#include "engine/core/cvar.h"
#include "render_device_null.h"
#include "gl_texture_upload_layout.h"

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

#ifndef GL_LESS
#define GL_LESS 0x0201
#endif

#ifndef GL_LEQUAL
#define GL_LEQUAL 0x0203
#endif

#ifndef GL_LINES
#define GL_LINES 0x0001
#endif

#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif

#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif

#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif

#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif

#ifndef GL_RED
#define GL_RED 0x1903
#endif

#ifndef GL_RG
#define GL_RG 0x8227
#endif

#ifndef GL_RGB
#define GL_RGB 0x1907
#endif

#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif

#ifndef GL_R16F
#define GL_R16F 0x822D
#endif

#ifndef GL_RG16F
#define GL_RG16F 0x822F
#endif

#ifndef GL_RGB16F
#define GL_RGB16F 0x881B
#endif

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

#ifndef GL_R32F
#define GL_R32F 0x822E
#endif

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif

#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP 0x8513
#endif

#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#endif

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif

#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif

#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R 0x8072
#endif

#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif

#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif

#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif

#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif

#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif

#ifndef GL_REPEAT
#define GL_REPEAT 0x2901
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif

#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif

#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif

#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif

#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif

#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT 0x1902
#endif

#ifndef GL_BLEND
#define GL_BLEND 0x0BE2
#endif

#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA 0x0302
#endif

#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif

#ifndef GL_CULL_FACE
#define GL_CULL_FACE 0x0B44
#endif

#ifndef GL_NONE
#define GL_NONE 0
#endif

#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif

#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif

#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif

#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif

#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif

#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif

#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX 0xFFFFFFFFU
#endif

#ifndef GL_TRIANGLES
#define GL_TRIANGLES 0x0004
#endif

// --- GL proc types ---

using GlCreateShaderProc = GLuint(APIENTRYP)(GLenum);
using GlShaderSourceProc = void(APIENTRYP)(GLuint, GLsizei, const char *const *,
                                           const GLint *);
using GlCompileShaderProc = void(APIENTRYP)(GLuint);
using GlGetShaderivProc = void(APIENTRYP)(GLuint, GLenum, GLint *);
using GlGetShaderInfoLogProc = void(APIENTRYP)(GLuint, GLsizei, GLsizei *,
                                               char *);
using GlDeleteShaderProc = void(APIENTRYP)(GLuint);
using GlCreateProgramProc = GLuint(APIENTRYP)(void);
using GlAttachShaderProc = void(APIENTRYP)(GLuint, GLuint);
using GlLinkProgramProc = void(APIENTRYP)(GLuint);
using GlGetProgramivProc = void(APIENTRYP)(GLuint, GLenum, GLint *);
using GlGetProgramInfoLogProc = void(APIENTRYP)(GLuint, GLsizei, GLsizei *,
                                                char *);
using GlDeleteProgramProc = void(APIENTRYP)(GLuint);
using GlUseProgramProc = void(APIENTRYP)(GLuint);
using GlGetUniformLocationProc = GLint(APIENTRYP)(GLuint, const char *);
using GlUniformMatrix4fvProc = void(APIENTRYP)(GLint, GLsizei, GLboolean,
                                               const GLfloat *);
using GlUniformMatrix3fvProc = void(APIENTRYP)(GLint, GLsizei, GLboolean,
                                               const GLfloat *);
using GlUniform1fProc = void(APIENTRYP)(GLint, GLfloat);
using GlUniform3fvProc = void(APIENTRYP)(GLint, GLsizei, const GLfloat *);
using GlUniform2fvProc = void(APIENTRYP)(GLint, GLsizei, const GLfloat *);
using GlGenVertexArraysProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlBindVertexArrayProc = void(APIENTRYP)(GLuint);
using GlDeleteVertexArraysProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlGenBuffersProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlBindBufferProc = void(APIENTRYP)(GLenum, GLuint);
using GlBufferDataProc = void(APIENTRYP)(GLenum, std::ptrdiff_t, const void *,
                                         GLenum);
using GlDeleteBuffersProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlEnableVertexAttribArrayProc = void(APIENTRYP)(GLuint);
using GlVertexAttribPointerProc = void(APIENTRYP)(GLuint, GLint, GLenum,
                                                  GLboolean, GLsizei,
                                                  const void *);
using GlVertexAttribDivisorProc = void(APIENTRYP)(GLuint, GLuint);
using GlDrawArraysProc = void(APIENTRYP)(GLenum, GLint, GLsizei);
using GlDrawElementsProc = void(APIENTRYP)(GLenum, GLsizei, GLenum,
                                           const void *);
using GlDrawElementsInstancedProc = void(APIENTRYP)(GLenum, GLsizei, GLenum,
                                                    const void *, GLsizei);
using GlViewportProc = void(APIENTRYP)(GLint, GLint, GLsizei, GLsizei);
using GlEnableProc = void(APIENTRYP)(GLenum);
using GlDisableProc = void(APIENTRYP)(GLenum);
using GlClearColorProc = void(APIENTRYP)(GLfloat, GLfloat, GLfloat, GLfloat);
using GlClearProc = void(APIENTRYP)(GLbitfield);
using GlUniform1iProc = void(APIENTRYP)(GLint, GLint);
using GlUniform4fvProc = void(APIENTRYP)(GLint, GLsizei, const GLfloat *);
using GlGetIntegervProc = void(APIENTRYP)(GLenum, GLint *);
using GlPixelStoreiProc = void(APIENTRYP)(GLenum, GLint);
using GlGenTexturesProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlDeleteTexturesProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlBindTextureProc = void(APIENTRYP)(GLenum, GLuint);
using GlActiveTextureProc = void(APIENTRYP)(GLenum);
using GlTexImage2DProc = void(APIENTRYP)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                         GLint, GLenum, GLenum, const void *);
using GlTexParameteriProc = void(APIENTRYP)(GLenum, GLenum, GLint);
using GlGenerateMipmapProc = void(APIENTRYP)(GLenum);
using GlGenFramebuffersProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlDeleteFramebuffersProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlBindFramebufferProc = void(APIENTRYP)(GLenum, GLuint);
using GlFramebufferTexture2DProc = void(APIENTRYP)(GLenum, GLenum, GLenum,
                                                   GLuint, GLint);
using GlCheckFramebufferStatusProc = GLenum(APIENTRYP)(GLenum);
using GlDrawBuffersProc = void(APIENTRYP)(GLsizei, const GLenum *);
using GlReadBufferProc = void(APIENTRYP)(GLenum);
using GlBlitFramebufferProc = void(APIENTRYP)(GLint, GLint, GLint, GLint, GLint,
                                              GLint, GLint, GLint, GLbitfield,
                                              GLenum);
using GlTexSubImage2DProc = void(APIENTRYP)(GLenum, GLint, GLint, GLint,
                                            GLsizei, GLsizei, GLenum, GLenum,
                                            const void *);
using GlBufferSubDataProc = void(APIENTRYP)(GLenum, std::ptrdiff_t,
                                            std::ptrdiff_t, const void *);
using GlBindBufferBaseProc = void(APIENTRYP)(GLenum, GLuint, GLuint);
using GlGetUniformBlockIndexProc = GLuint(APIENTRYP)(GLuint, const GLchar *);
using GlUniformBlockBindingProc = void(APIENTRYP)(GLuint, GLuint, GLuint);
using GlBlendFuncProc = void(APIENTRYP)(GLenum, GLenum);
using GlDepthMaskProc = void(APIENTRYP)(GLboolean);
using GlDepthFuncProc = void(APIENTRYP)(GLenum);
using GlGenQueriesProc = void(APIENTRYP)(GLsizei, GLuint *);
using GlDeleteQueriesProc = void(APIENTRYP)(GLsizei, const GLuint *);
using GlQueryCounterProc = void(APIENTRYP)(GLuint, GLenum);
using GlGetQueryObjectui64vProc = void(APIENTRYP)(GLuint, GLenum, GLuint64 *);

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
  GlUniform2fvProc uniform2fv = nullptr;
  GlGenVertexArraysProc genVertexArrays = nullptr;
  GlBindVertexArrayProc bindVertexArray = nullptr;
  GlDeleteVertexArraysProc deleteVertexArrays = nullptr;
  GlGenBuffersProc genBuffers = nullptr;
  GlBindBufferProc bindBuffer = nullptr;
  GlBufferDataProc bufferData = nullptr;
  GlBufferSubDataProc bufferSubData = nullptr;
  GlBindBufferBaseProc bindBufferBase = nullptr;
  GlGetUniformBlockIndexProc getUniformBlockIndex = nullptr;
  GlUniformBlockBindingProc uniformBlockBinding = nullptr;
  GlDeleteBuffersProc deleteBuffers = nullptr;
  GlEnableVertexAttribArrayProc enableVertexAttribArray = nullptr;
  GlVertexAttribPointerProc vertexAttribPointer = nullptr;
  GlVertexAttribDivisorProc vertexAttribDivisor = nullptr;
  GlDrawArraysProc drawArrays = nullptr;
  GlDrawElementsProc drawElements = nullptr;
  GlDrawElementsInstancedProc drawElementsInstanced = nullptr;
  GlViewportProc viewport = nullptr;
  GlEnableProc enable = nullptr;
  GlDisableProc disable = nullptr;
  GlClearColorProc clearColor = nullptr;
  GlClearProc clear = nullptr;
  GlUniform1iProc uniform1i = nullptr;
  GlUniform4fvProc uniform4fv = nullptr;
  GlGetIntegervProc getIntegerv = nullptr;
  GlPixelStoreiProc pixelStorei = nullptr;
  GlGenTexturesProc genTextures = nullptr;
  GlDeleteTexturesProc deleteTextures = nullptr;
  GlBindTextureProc bindTexture = nullptr;
  GlActiveTextureProc activeTexture = nullptr;
  GlTexImage2DProc texImage2D = nullptr;
  GlTexParameteriProc texParameteri = nullptr;
  GlGenerateMipmapProc generateMipmap = nullptr;
  GlGenFramebuffersProc genFramebuffers = nullptr;
  GlDeleteFramebuffersProc deleteFramebuffers = nullptr;
  GlBindFramebufferProc bindFramebuffer = nullptr;
  GlFramebufferTexture2DProc framebufferTexture2D = nullptr;
  GlCheckFramebufferStatusProc checkFramebufferStatus = nullptr;
  GlDrawBuffersProc drawBuffers = nullptr;
  GlReadBufferProc readBuffer = nullptr;
  GlBlitFramebufferProc blitFramebuffer = nullptr;
  GlTexSubImage2DProc texSubImage2D = nullptr;
  GlBlendFuncProc blendFunc = nullptr;
  GlDepthMaskProc depthMask = nullptr;
  GlDepthFuncProc depthFunc = nullptr;
  GlGenQueriesProc genQueries = nullptr;
  GlDeleteQueriesProc deleteQueries = nullptr;
  GlQueryCounterProc queryCounter = nullptr;
  GlGetQueryObjectui64vProc getQueryObjectui64v = nullptr;
};

// --- Backend resource records behind the generational handle tables ---

/// GL buffer object plus the target/usage fixed by its BufferDesc.
struct GlBufferRecord final {
  GLuint id = 0U;
  BufferUsage usage = BufferUsage::Vertex;
  BufferAccess access = BufferAccess::Static;
};

/// GL texture object plus the creation shape used for validation and
/// bind-target/attachment resolution.
struct GlTextureRecord final {
  GLuint id = 0U;
  TextureKind kind = TextureKind::Tex2D;
  TextureFormat format = TextureFormat::RGBA8;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t mipLevels = 1;
};

/// Linked GL program object.
struct GlProgramRecord final {
  GLuint id = 0U;
};

/// GL vertex-array object; hasIndex mirrors the GeometryDesc.
struct GlGeometryRecord final {
  GLuint vao = 0U;
  bool hasIndex = false;
};

/// GL framebuffer object.
struct GlTargetRecord final {
  GLuint fbo = 0U;
};

/// GL timestamp query object.
struct GlQueryRecord final {
  GLuint id = 0U;
};

// Capacities track the engine's fixed registries above the device: the
// mesh registry holds up to 4096 meshes (one geometry, two buffers each),
// the texture system 512 slots plus renderer-internal targets, IBL and
// shadow passes need per-face render targets.
constexpr std::size_t kMaxDeviceBuffers = 8704U;
constexpr std::size_t kMaxDeviceTextures = 1024U;
constexpr std::size_t kMaxDevicePrograms = 128U;
constexpr std::size_t kMaxDeviceGeometries = 4352U;
constexpr std::size_t kMaxDeviceTargets = 256U;
constexpr std::size_t kMaxDeviceQueries = 256U;

/// Owns the OpenGL-backed render device state.
struct RenderDeviceContext final {
  bool initialized = false;
  GlTable gl{};
  RenderDevice device{};
  device_slot_detail::DeviceSlotTable<GlBufferRecord, kMaxDeviceBuffers>
      buffers{};
  device_slot_detail::DeviceSlotTable<GlTextureRecord, kMaxDeviceTextures>
      textures{};
  device_slot_detail::DeviceSlotTable<GlProgramRecord, kMaxDevicePrograms>
      programs{};
  device_slot_detail::DeviceSlotTable<GlGeometryRecord, kMaxDeviceGeometries>
      geometries{};
  device_slot_detail::DeviceSlotTable<GlTargetRecord, kMaxDeviceTargets>
      targets{};
  device_slot_detail::DeviceSlotTable<GlQueryRecord, kMaxDeviceQueries>
      queries{};
  DeviceDebugStats stats{};
  GLuint boundVao = 0U;
};

/// Returns the default OpenGL render device context.
RenderDeviceContext &render_device_context() noexcept {
  static RenderDeviceContext context{};
  return context;
}

/// Returns the loaded OpenGL dispatch table.
GlTable &gl_table() noexcept { return render_device_context().gl; }

/// Records one dropped operation; the first few log so the violation is
/// visible without flooding per-frame paths.
void drop_operation(const char *what) noexcept {
  DeviceDebugStats &stats = render_device_context().stats;
  ++stats.droppedOperations;
  if (stats.droppedOperations <= 8U) {
    char msg[160] = {};
    std::snprintf(msg, sizeof(msg),
                  "dropped device operation (invalid or stale argument): %s",
                  what);
    core::log_message(core::LogLevel::Error, "render_device", msg);
  }
}

/// Binds a VAO through the backend cache (draw paths churn geometry).
void bind_vao_cached(GLuint vao) noexcept {
  RenderDeviceContext &ctx = render_device_context();
  if (ctx.boundVao != vao) {
    ctx.gl.bindVertexArray(vao);
    ctx.boundVao = vao;
  }
}

/// Binds a VAO unconditionally and updates the cache (creation paths).
void bind_vao_direct(GLuint vao) noexcept {
  render_device_context().gl.bindVertexArray(vao);
  render_device_context().boundVao = vao;
}

/// Executes a texture upload without leaking pixel-store state.
template <typename Upload>
void with_texture_unpack_alignment(std::int32_t requiredAlignment,
                                   Upload upload) noexcept {
  detail::with_gl_unpack_alignment(
      requiredAlignment,
      [](std::int32_t *outAlignment) noexcept {
        GLint alignment = 4;
        gl_table().getIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        *outAlignment = static_cast<std::int32_t>(alignment);
      },
      [](std::int32_t alignment) noexcept {
        gl_table().pixelStorei(GL_UNPACK_ALIGNMENT,
                               static_cast<GLint>(alignment));
      },
      upload);
}

/// Loads the requested resource for proc.
template <typename T> bool load_proc(T *out, const char *name) noexcept {
  *out = reinterpret_cast<T>(core::get_gl_proc_address(name));
  return *out != nullptr;
}

/// Loads the requested resource for all gl functions.
bool load_all_gl_functions() noexcept {
  return load_proc(&gl_table().createShader, "glCreateShader") &&
         load_proc(&gl_table().shaderSource, "glShaderSource") &&
         load_proc(&gl_table().compileShader, "glCompileShader") &&
         load_proc(&gl_table().getShaderiv, "glGetShaderiv") &&
         load_proc(&gl_table().getShaderInfoLog, "glGetShaderInfoLog") &&
         load_proc(&gl_table().deleteShader, "glDeleteShader") &&
         load_proc(&gl_table().createProgram, "glCreateProgram") &&
         load_proc(&gl_table().attachShader, "glAttachShader") &&
         load_proc(&gl_table().linkProgram, "glLinkProgram") &&
         load_proc(&gl_table().getProgramiv, "glGetProgramiv") &&
         load_proc(&gl_table().getProgramInfoLog, "glGetProgramInfoLog") &&
         load_proc(&gl_table().deleteProgram, "glDeleteProgram") &&
         load_proc(&gl_table().useProgram, "glUseProgram") &&
         load_proc(&gl_table().getUniformLocation, "glGetUniformLocation") &&
         load_proc(&gl_table().uniformMatrix4fv, "glUniformMatrix4fv") &&
         load_proc(&gl_table().uniformMatrix3fv, "glUniformMatrix3fv") &&
         load_proc(&gl_table().uniform1f, "glUniform1f") &&
         load_proc(&gl_table().uniform3fv, "glUniform3fv") &&
         load_proc(&gl_table().uniform2fv, "glUniform2fv") &&
         load_proc(&gl_table().genVertexArrays, "glGenVertexArrays") &&
         load_proc(&gl_table().bindVertexArray, "glBindVertexArray") &&
         load_proc(&gl_table().deleteVertexArrays, "glDeleteVertexArrays") &&
         load_proc(&gl_table().genBuffers, "glGenBuffers") &&
         load_proc(&gl_table().bindBuffer, "glBindBuffer") &&
         load_proc(&gl_table().bufferData, "glBufferData") &&
         load_proc(&gl_table().bufferSubData, "glBufferSubData") &&
         load_proc(&gl_table().bindBufferBase, "glBindBufferBase") &&
         load_proc(&gl_table().getUniformBlockIndex,
                   "glGetUniformBlockIndex") &&
         load_proc(&gl_table().uniformBlockBinding, "glUniformBlockBinding") &&
         load_proc(&gl_table().deleteBuffers, "glDeleteBuffers") &&
         load_proc(&gl_table().enableVertexAttribArray,
                   "glEnableVertexAttribArray") &&
         load_proc(&gl_table().vertexAttribPointer, "glVertexAttribPointer") &&
         load_proc(&gl_table().vertexAttribDivisor, "glVertexAttribDivisor") &&
         load_proc(&gl_table().drawArrays, "glDrawArrays") &&
         load_proc(&gl_table().drawElements, "glDrawElements") &&
         load_proc(&gl_table().drawElementsInstanced,
                   "glDrawElementsInstanced") &&
         load_proc(&gl_table().viewport, "glViewport") &&
         load_proc(&gl_table().enable, "glEnable") &&
         load_proc(&gl_table().disable, "glDisable") &&
         load_proc(&gl_table().clearColor, "glClearColor") &&
         load_proc(&gl_table().clear, "glClear") &&
         load_proc(&gl_table().uniform1i, "glUniform1i") &&
         load_proc(&gl_table().uniform4fv, "glUniform4fv") &&
         load_proc(&gl_table().getIntegerv, "glGetIntegerv") &&
         load_proc(&gl_table().pixelStorei, "glPixelStorei") &&
         load_proc(&gl_table().genTextures, "glGenTextures") &&
         load_proc(&gl_table().deleteTextures, "glDeleteTextures") &&
         load_proc(&gl_table().bindTexture, "glBindTexture") &&
         load_proc(&gl_table().activeTexture, "glActiveTexture") &&
         load_proc(&gl_table().texImage2D, "glTexImage2D") &&
         load_proc(&gl_table().texParameteri, "glTexParameteri") &&
         load_proc(&gl_table().generateMipmap, "glGenerateMipmap") &&
         load_proc(&gl_table().genFramebuffers, "glGenFramebuffers") &&
         load_proc(&gl_table().deleteFramebuffers, "glDeleteFramebuffers") &&
         load_proc(&gl_table().bindFramebuffer, "glBindFramebuffer") &&
         load_proc(&gl_table().framebufferTexture2D,
                   "glFramebufferTexture2D") &&
         load_proc(&gl_table().checkFramebufferStatus,
                   "glCheckFramebufferStatus") &&
         load_proc(&gl_table().drawBuffers, "glDrawBuffers") &&
         load_proc(&gl_table().readBuffer, "glReadBuffer") &&
         load_proc(&gl_table().blitFramebuffer, "glBlitFramebuffer") &&
         load_proc(&gl_table().texSubImage2D, "glTexSubImage2D") &&
         load_proc(&gl_table().blendFunc, "glBlendFunc") &&
         load_proc(&gl_table().depthMask, "glDepthMask") &&
         load_proc(&gl_table().depthFunc, "glDepthFunc") &&
         load_proc(&gl_table().genQueries, "glGenQueries") &&
         load_proc(&gl_table().deleteQueries, "glDeleteQueries") &&
         load_proc(&gl_table().queryCounter, "glQueryCounter") &&
         load_proc(&gl_table().getQueryObjectui64v, "glGetQueryObjectui64v");
}

// --- Format and semantic mappings (GL-private) ---

/// GL upload/storage description for one engine texture format.
struct GlFormatInfo final {
  GLint internalFormat = 0;
  GLenum externalFormat = 0U;
  GLenum type = 0U;
  std::int32_t channels = 0;
  std::int32_t bytesPerChannel = 0;
  bool depth = false;
};

/// Maps an engine texture format onto GL storage/upload enums. The U8
/// formats keep the historical unsized internal formats the previous
/// backend used so texture memory layout is unchanged.
GlFormatInfo gl_format_info(TextureFormat format) noexcept {
  switch (format) {
  case TextureFormat::R8:
    return {GL_RED, GL_RED, GL_UNSIGNED_BYTE, 1, 1, false};
  case TextureFormat::RG8:
    return {GL_RG, GL_RG, GL_UNSIGNED_BYTE, 2, 1, false};
  case TextureFormat::RGB8:
    return {GL_RGB, GL_RGB, GL_UNSIGNED_BYTE, 3, 1, false};
  case TextureFormat::RGBA8:
    return {GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, 4, 1, false};
  case TextureFormat::R16F:
    return {GL_R16F, GL_RED, GL_FLOAT, 1, 4, false};
  case TextureFormat::RG16F:
    return {GL_RG16F, GL_RG, GL_FLOAT, 2, 4, false};
  case TextureFormat::RGB16F:
    return {GL_RGB16F, GL_RGB, GL_FLOAT, 3, 4, false};
  case TextureFormat::RGBA16F:
    return {GL_RGBA16F, GL_RGBA, GL_FLOAT, 4, 4, false};
  case TextureFormat::R32F:
    return {static_cast<GLint>(GL_R32F), GL_RED, GL_FLOAT, 1, 4, false};
  case TextureFormat::Depth24:
    return {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, 1, 4, true};
  }
  return {};
}

/// Fixed vertex-input location every engine shader declares for a
/// semantic (Color shares the debug-line shaders' location 1; it never
/// coexists with Normal in one layout).
GLuint semantic_location(VertexSemantic semantic) noexcept {
  switch (semantic) {
  case VertexSemantic::Position:
    return 0U;
  case VertexSemantic::Normal:
  case VertexSemantic::Color:
    return 1U;
  case VertexSemantic::TexCoord0:
    return 2U;
  case VertexSemantic::InstanceModel0:
    return 3U;
  case VertexSemantic::InstanceModel1:
    return 4U;
  case VertexSemantic::InstanceModel2:
    return 5U;
  case VertexSemantic::InstanceModel3:
    return 6U;
  case VertexSemantic::InstanceParams:
    return 7U;
  case VertexSemantic::Joints:
    return 8U;
  case VertexSemantic::Weights:
    return 9U;
  }
  return 0U;
}

// Buffer storage is target-agnostic in GL, so uploads go through the
// array-buffer target even for index buffers — the element-array binding
// is VAO state and is only established inside create_geometry. This keeps
// buffer creation/update free of VAO side effects.
GLenum buffer_upload_target(BufferUsage usage) noexcept {
  return (usage == BufferUsage::Uniform) ? GL_UNIFORM_BUFFER
                                         : GL_ARRAY_BUFFER;
}

/// GL usage hint for an access class.
GLenum buffer_usage_hint(BufferAccess access) noexcept {
  return (access == BufferAccess::Stream) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
}

/// Applies a vertex layout's attributes to the bound VAO/array buffer.
void apply_vertex_layout(const VertexLayout &layout,
                         GLuint divisor) noexcept {
  for (std::size_t i = 0U; i < layout.attributeCount; ++i) {
    const VertexAttribute &attr = layout.attributes[i];
    const GLuint location = semantic_location(attr.semantic);
    gl_table().enableVertexAttribArray(location);
    gl_table().vertexAttribPointer(
        location, static_cast<GLint>(attr.componentCount), GL_FLOAT, GL_FALSE,
        static_cast<GLsizei>(layout.strideBytes),
        reinterpret_cast<const void *>(
            static_cast<std::uintptr_t>(attr.offsetBytes)));
    if (divisor != 0U) {
      gl_table().vertexAttribDivisor(location, divisor);
    }
  }
}

/// Validates a vertex layout: bounded attribute count, positive
/// component counts, offsets inside the stride.
bool vertex_layout_valid(const VertexLayout &layout) noexcept {
  if ((layout.attributeCount > kMaxVertexAttributes) ||
      (layout.strideBytes < 0)) {
    return false;
  }
  for (std::size_t i = 0U; i < layout.attributeCount; ++i) {
    const VertexAttribute &attr = layout.attributes[i];
    if ((attr.componentCount < 1) || (attr.componentCount > 4) ||
        (attr.offsetBytes < 0)) {
      return false;
    }
    const std::int32_t attrEnd =
        attr.offsetBytes +
        (attr.componentCount * static_cast<std::int32_t>(sizeof(float)));
    if ((layout.strideBytes != 0) && (attrEnd > layout.strideBytes)) {
      return false;
    }
  }
  return true;
}

// --- Buffers ---

DeviceBufferHandle gl_create_buffer(const BufferDesc &desc) noexcept {
  if (desc.sizeBytes < 0) {
    drop_operation("create_buffer: negative size");
    return kInvalidDeviceBuffer;
  }
  GLuint id = 0U;
  gl_table().genBuffers(1, &id);
  if (id == 0U) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "buffer creation failed");
    return kInvalidDeviceBuffer;
  }
  GlBufferRecord record{};
  record.id = id;
  record.usage = desc.usage;
  record.access = desc.access;

  const GLenum target = buffer_upload_target(desc.usage);
  if ((desc.sizeBytes > 0) || (desc.data != nullptr)) {
    gl_table().bindBuffer(target, id);
    gl_table().bufferData(target, desc.sizeBytes, desc.data,
                          buffer_usage_hint(desc.access));
    gl_table().bindBuffer(target, 0U);
  }

  const std::uint32_t value = render_device_context().buffers.allocate(record);
  if (value == 0U) {
    gl_table().deleteBuffers(1, &id);
    core::log_message(core::LogLevel::Error, "render_device",
                      "buffer handle table exhausted");
    return kInvalidDeviceBuffer;
  }
  return DeviceBufferHandle{value};
}

void gl_buffer_upload(DeviceBufferHandle buffer, const void *data,
                      std::ptrdiff_t sizeBytes, bool respecify) noexcept {
  GlBufferRecord *record =
      render_device_context().buffers.resolve(buffer.value);
  if ((record == nullptr) || (sizeBytes < 0)) {
    drop_operation("update_buffer: stale handle or negative size");
    return;
  }
  const GLenum target = buffer_upload_target(record->usage);
  gl_table().bindBuffer(target, record->id);
  if (respecify) {
    gl_table().bufferData(target, sizeBytes, data,
                          buffer_usage_hint(record->access));
  } else {
    gl_table().bufferSubData(target, 0, sizeBytes, data);
  }
  gl_table().bindBuffer(target, 0U);
}

void gl_update_buffer(DeviceBufferHandle buffer, const void *data,
                      std::ptrdiff_t sizeBytes) noexcept {
  gl_buffer_upload(buffer, data, sizeBytes, true);
}

void gl_update_buffer_range(DeviceBufferHandle buffer, const void *data,
                            std::ptrdiff_t sizeBytes) noexcept {
  gl_buffer_upload(buffer, data, sizeBytes, false);
}

void gl_destroy_buffer(DeviceBufferHandle buffer) noexcept {
  if (buffer.value == 0U) {
    return;
  }
  GlBufferRecord *record =
      render_device_context().buffers.resolve(buffer.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  const GLuint id = record->id;
  render_device_context().buffers.release(buffer.value);
  gl_table().deleteBuffers(1, &id);
}

void gl_bind_uniform_buffer_slot(std::uint32_t slot,
                                 DeviceBufferHandle buffer) noexcept {
  if (buffer.value == 0U) {
    gl_table().bindBufferBase(GL_UNIFORM_BUFFER, slot, 0U);
    return;
  }
  GlBufferRecord *record =
      render_device_context().buffers.resolve(buffer.value);
  if ((record == nullptr) || (record->usage != BufferUsage::Uniform)) {
    drop_operation("bind_uniform_buffer_slot: stale or non-uniform buffer");
    return;
  }
  gl_table().bindBufferBase(GL_UNIFORM_BUFFER, slot, record->id);
}

// --- Textures ---

/// Allocates one mip level for every face of the texture's kind.
void gl_alloc_level(const GlFormatInfo &info, TextureKind kind,
                    std::int32_t level, std::int32_t size,
                    std::int32_t height, const void *pixels,
                    const void *const *facePixels) noexcept {
  if (kind == TextureKind::Cube) {
    for (int face = 0; face < 6; ++face) {
      const void *facePtr =
          (facePixels != nullptr) ? facePixels[face] : nullptr;
      gl_table().texImage2D(
          static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), level,
          info.internalFormat, static_cast<GLsizei>(size),
          static_cast<GLsizei>(size), 0, info.externalFormat, info.type,
          facePtr);
    }
  } else {
    gl_table().texImage2D(GL_TEXTURE_2D, level, info.internalFormat,
                          static_cast<GLsizei>(size),
                          static_cast<GLsizei>(height), 0, info.externalFormat,
                          info.type, pixels);
  }
}

DeviceTextureHandle gl_create_texture(const TextureDesc &desc) noexcept {
  const GlFormatInfo info = gl_format_info(desc.format);
  const std::int32_t height =
      (desc.kind == TextureKind::Cube) ? desc.width : desc.height;
  if ((info.channels == 0) || (desc.width <= 0) || (height <= 0) ||
      (desc.mipLevels < 0)) {
    drop_operation("create_texture: invalid descriptor");
    return kInvalidDeviceTexture;
  }
  const bool floatData = (info.type == GL_FLOAT) && !info.depth;
  if ((desc.pixels != nullptr) || (desc.facePixels != nullptr)) {
    const bool wantsF32 = (desc.pixelData == TexelData::F32);
    if (info.depth || (floatData != wantsF32)) {
      drop_operation("create_texture: pixel data mismatches format");
      return kInvalidDeviceTexture;
    }
  }
  detail::GlTextureUploadLayout layout{};
  if (!detail::describe_gl_texture_upload(desc.width, info.channels,
                                          info.bytesPerChannel, floatData,
                                          &layout)) {
    drop_operation("create_texture: unsupported upload layout");
    return kInvalidDeviceTexture;
  }

  GLuint id = 0U;
  gl_table().genTextures(1, &id);
  if (id == 0U) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "texture creation failed");
    return kInvalidDeviceTexture;
  }

  const GLenum target =
      (desc.kind == TextureKind::Cube) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  gl_table().bindTexture(target, id);

  with_texture_unpack_alignment(layout.unpackAlignment, [&]() noexcept {
    if (desc.mipLevels > 1) {
      std::int32_t mipSize = desc.width;
      std::int32_t mipHeight = height;
      for (std::int32_t mip = 0; mip < desc.mipLevels; ++mip) {
        gl_alloc_level(info, desc.kind, mip, mipSize, mipHeight,
                       (mip == 0) ? desc.pixels : nullptr,
                       (mip == 0) ? desc.facePixels : nullptr);
        mipSize = std::max<std::int32_t>(1, mipSize / 2);
        mipHeight = std::max<std::int32_t>(1, mipHeight / 2);
      }
    } else {
      gl_alloc_level(info, desc.kind, 0, desc.width, height, desc.pixels,
                     desc.facePixels);
    }
  });

  const bool generateMips = (desc.mipLevels == 0);
  const bool hasMips = generateMips || (desc.mipLevels > 1);
  GLint minFilter = GL_LINEAR;
  GLint magFilter = GL_LINEAR;
  if (desc.filter == TextureFilter::Nearest) {
    minFilter = GL_NEAREST;
    magFilter = GL_NEAREST;
  } else if ((desc.filter == TextureFilter::LinearMipmap) && hasMips) {
    minFilter = GL_LINEAR_MIPMAP_LINEAR;
  }
  const GLint wrap =
      (desc.wrap == TextureWrap::Repeat) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
  gl_table().texParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
  gl_table().texParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
  gl_table().texParameteri(target, GL_TEXTURE_WRAP_S, wrap);
  gl_table().texParameteri(target, GL_TEXTURE_WRAP_T, wrap);
  if (desc.kind == TextureKind::Cube) {
    gl_table().texParameteri(target, GL_TEXTURE_WRAP_R, wrap);
  }
  if (desc.mipLevels > 1) {
    gl_table().texParameteri(target, GL_TEXTURE_MAX_LEVEL, desc.mipLevels - 1);
  }
  if (generateMips) {
    gl_table().generateMipmap(target);
  }
  gl_table().bindTexture(target, 0U);

  GlTextureRecord record{};
  record.id = id;
  record.kind = desc.kind;
  record.format = desc.format;
  record.width = desc.width;
  record.height = height;
  record.mipLevels = desc.mipLevels;
  const std::uint32_t value = render_device_context().textures.allocate(record);
  if (value == 0U) {
    gl_table().deleteTextures(1, &id);
    core::log_message(core::LogLevel::Error, "render_device",
                      "texture handle table exhausted");
    return kInvalidDeviceTexture;
  }
  return DeviceTextureHandle{value};
}

void gl_update_texture(DeviceTextureHandle texture, const void *pixels,
                       std::int32_t width, std::int32_t height) noexcept {
  GlTextureRecord *record =
      render_device_context().textures.resolve(texture.value);
  if ((record == nullptr) || (record->kind != TextureKind::Tex2D) ||
      (width <= 0) || (height <= 0) || (width > record->width) ||
      (height > record->height)) {
    drop_operation("update_texture: stale handle or oversized shape");
    return;
  }
  const GlFormatInfo info = gl_format_info(record->format);
  detail::GlTextureUploadLayout layout{};
  if (!detail::describe_gl_texture_upload(width, info.channels,
                                          info.bytesPerChannel,
                                          info.type == GL_FLOAT, &layout)) {
    drop_operation("update_texture: unsupported upload layout");
    return;
  }
  gl_table().bindTexture(GL_TEXTURE_2D, record->id);
  with_texture_unpack_alignment(layout.unpackAlignment, [&]() noexcept {
    gl_table().texSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                             static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height), info.externalFormat,
                             info.type, pixels);
  });
  gl_table().bindTexture(GL_TEXTURE_2D, 0U);
}

void gl_destroy_texture(DeviceTextureHandle texture) noexcept {
  if (texture.value == 0U) {
    return;
  }
  GlTextureRecord *record =
      render_device_context().textures.resolve(texture.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  const GLuint id = record->id;
  render_device_context().textures.release(texture.value);
  gl_table().deleteTextures(1, &id);
}

void gl_bind_texture_slot(std::uint32_t slot,
                          DeviceTextureHandle texture) noexcept {
  gl_table().activeTexture(static_cast<GLenum>(GL_TEXTURE0 + slot));
  if (texture.value == 0U) {
    // Clearing a slot detaches both bind targets: the caller intent is
    // "nothing readable here", independent of what was bound before.
    gl_table().bindTexture(GL_TEXTURE_2D, 0U);
    gl_table().bindTexture(GL_TEXTURE_CUBE_MAP, 0U);
    return;
  }
  GlTextureRecord *record =
      render_device_context().textures.resolve(texture.value);
  if (record == nullptr) {
    drop_operation("bind_texture_slot: stale texture handle");
    gl_table().bindTexture(GL_TEXTURE_2D, 0U);
    gl_table().bindTexture(GL_TEXTURE_CUBE_MAP, 0U);
    return;
  }
  const GLenum target =
      (record->kind == TextureKind::Cube) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  gl_table().bindTexture(target, record->id);
}

// --- Programs and shader parameters ---

/// Compiles one GL shader stage; 0 on failure (logged).
GLuint gl_compile_stage(GLenum stage, const char *label,
                        const char *source) noexcept {
  const GLuint shader = gl_table().createShader(stage);
  if (shader == 0U) {
    return 0U;
  }
  const char *sources[] = {source};
  const GLint len = static_cast<GLint>(std::strlen(source));
  gl_table().shaderSource(shader, 1, sources, &len);
  gl_table().compileShader(shader);

  GLint compiled = 0;
  gl_table().getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != static_cast<GLint>(GL_TRUE)) {
    std::array<char, 1024U> log{};
    GLsizei written = 0;
    gl_table().getShaderInfoLog(shader, static_cast<GLsizei>(log.size()),
                                &written, log.data());
    char msg[1200] = {};
    std::snprintf(msg, sizeof(msg), "%s shader compile failed: %s", label,
                  log.data());
    core::log_message(core::LogLevel::Error, "renderer", msg);
    gl_table().deleteShader(shader);
    return 0U;
  }
  return shader;
}

DeviceProgramHandle gl_create_program(const char *vertexSource,
                                      const char *fragmentSource) noexcept {
  if ((vertexSource == nullptr) || (fragmentSource == nullptr)) {
    drop_operation("create_program: null source");
    return kInvalidDeviceProgram;
  }
  const GLuint vert =
      gl_compile_stage(GL_VERTEX_SHADER, "vertex", vertexSource);
  if (vert == 0U) {
    return kInvalidDeviceProgram;
  }
  const GLuint frag =
      gl_compile_stage(GL_FRAGMENT_SHADER, "fragment", fragmentSource);
  if (frag == 0U) {
    gl_table().deleteShader(vert);
    return kInvalidDeviceProgram;
  }

  const GLuint program = gl_table().createProgram();
  bool linked = false;
  if (program != 0U) {
    gl_table().attachShader(program, vert);
    gl_table().attachShader(program, frag);
    gl_table().linkProgram(program);
    GLint status = 0;
    gl_table().getProgramiv(program, GL_LINK_STATUS, &status);
    linked = (status == static_cast<GLint>(GL_TRUE));
    if (!linked) {
      std::array<char, 1024U> log{};
      GLsizei written = 0;
      gl_table().getProgramInfoLog(program, static_cast<GLsizei>(log.size()),
                                   &written, log.data());
      char msg[1200] = {};
      std::snprintf(msg, sizeof(msg), "shader link failed: %s", log.data());
      core::log_message(core::LogLevel::Error, "renderer", msg);
    }
  }
  // The stage objects only feed the link; the program keeps its own copy.
  gl_table().deleteShader(vert);
  gl_table().deleteShader(frag);
  if (!linked) {
    if (program != 0U) {
      gl_table().deleteProgram(program);
    }
    return kInvalidDeviceProgram;
  }

  const std::uint32_t value =
      render_device_context().programs.allocate(GlProgramRecord{program});
  if (value == 0U) {
    gl_table().deleteProgram(program);
    core::log_message(core::LogLevel::Error, "render_device",
                      "program handle table exhausted");
    return kInvalidDeviceProgram;
  }
  return DeviceProgramHandle{value};
}

void gl_destroy_program(DeviceProgramHandle program) noexcept {
  if (program.value == 0U) {
    return;
  }
  GlProgramRecord *record =
      render_device_context().programs.resolve(program.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  const GLuint id = record->id;
  render_device_context().programs.release(program.value);
  gl_table().deleteProgram(id);
}

void gl_bind_program(DeviceProgramHandle program) noexcept {
  if (program.value == 0U) {
    gl_table().useProgram(0U);
    return;
  }
  GlProgramRecord *record =
      render_device_context().programs.resolve(program.value);
  if (record == nullptr) {
    drop_operation("bind_program: stale program handle");
    gl_table().useProgram(0U);
    return;
  }
  gl_table().useProgram(record->id);
}

ShaderParam gl_shader_param(DeviceProgramHandle program,
                            const char *name) noexcept {
  GlProgramRecord *record =
      render_device_context().programs.resolve(program.value);
  if ((record == nullptr) || (name == nullptr)) {
    return kInvalidShaderParam;
  }
  return ShaderParam{
      static_cast<std::int32_t>(gl_table().getUniformLocation(record->id,
                                                              name))};
}

void gl_set_param_mat4(ShaderParam param, const float *value) noexcept {
  if (param.valid()) {
    gl_table().uniformMatrix4fv(static_cast<GLint>(param.value), 1, GL_FALSE,
                                value);
  }
}

void gl_set_param_mat3(ShaderParam param, const float *value) noexcept {
  if (param.valid()) {
    gl_table().uniformMatrix3fv(static_cast<GLint>(param.value), 1, GL_FALSE,
                                value);
  }
}

void gl_set_param_f32(ShaderParam param, float value) noexcept {
  if (param.valid()) {
    gl_table().uniform1f(static_cast<GLint>(param.value), value);
  }
}

void gl_set_param_i32(ShaderParam param, std::int32_t value) noexcept {
  if (param.valid()) {
    gl_table().uniform1i(static_cast<GLint>(param.value),
                         static_cast<GLint>(value));
  }
}

void gl_set_param_vec2(ShaderParam param, const float *value) noexcept {
  if (param.valid()) {
    gl_table().uniform2fv(static_cast<GLint>(param.value), 1, value);
  }
}

void gl_set_param_vec3(ShaderParam param, const float *value) noexcept {
  if (param.valid()) {
    gl_table().uniform3fv(static_cast<GLint>(param.value), 1, value);
  }
}

void gl_set_param_vec4(ShaderParam param, const float *value) noexcept {
  if (param.valid()) {
    gl_table().uniform4fv(static_cast<GLint>(param.value), 1, value);
  }
}

bool gl_bind_program_uniform_block(DeviceProgramHandle program,
                                   const char *blockName,
                                   std::uint32_t slot) noexcept {
  GlProgramRecord *record =
      render_device_context().programs.resolve(program.value);
  if ((record == nullptr) || (blockName == nullptr)) {
    return false;
  }
  const GLuint blockIndex =
      gl_table().getUniformBlockIndex(record->id, blockName);
  if (blockIndex == GL_INVALID_INDEX) {
    return false;
  }
  gl_table().uniformBlockBinding(record->id, blockIndex,
                                 static_cast<GLuint>(slot));
  return true;
}

// --- Geometry ---

DeviceGeometryHandle gl_create_geometry(const GeometryDesc &desc) noexcept {
  RenderDeviceContext &ctx = render_device_context();
  if (!vertex_layout_valid(desc.layout)) {
    drop_operation("create_geometry: invalid vertex layout");
    return kInvalidDeviceGeometry;
  }
  GlBufferRecord *vertexRecord = nullptr;
  if (desc.vertexBuffer.value != 0U) {
    vertexRecord = ctx.buffers.resolve(desc.vertexBuffer.value);
    if ((vertexRecord == nullptr) ||
        (vertexRecord->usage != BufferUsage::Vertex)) {
      drop_operation("create_geometry: stale or non-vertex buffer");
      return kInvalidDeviceGeometry;
    }
  }
  GlBufferRecord *indexRecord = nullptr;
  if (desc.indexBuffer.value != 0U) {
    indexRecord = ctx.buffers.resolve(desc.indexBuffer.value);
    if ((indexRecord == nullptr) ||
        (indexRecord->usage != BufferUsage::Index)) {
      drop_operation("create_geometry: stale or non-index buffer");
      return kInvalidDeviceGeometry;
    }
  }

  GLuint vao = 0U;
  gl_table().genVertexArrays(1, &vao);
  if (vao == 0U) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "geometry creation failed");
    return kInvalidDeviceGeometry;
  }
  bind_vao_direct(vao);
  if (vertexRecord != nullptr) {
    gl_table().bindBuffer(GL_ARRAY_BUFFER, vertexRecord->id);
    apply_vertex_layout(desc.layout, 0U);
    gl_table().bindBuffer(GL_ARRAY_BUFFER, 0U);
  }
  if (indexRecord != nullptr) {
    // Captured by the VAO: this element binding is the geometry's index
    // stream, so it must stay bound while the VAO is current.
    gl_table().bindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexRecord->id);
  }
  bind_vao_direct(0U);

  GlGeometryRecord record{};
  record.vao = vao;
  record.hasIndex = (indexRecord != nullptr);
  const std::uint32_t value = ctx.geometries.allocate(record);
  if (value == 0U) {
    gl_table().deleteVertexArrays(1, &vao);
    core::log_message(core::LogLevel::Error, "render_device",
                      "geometry handle table exhausted");
    return kInvalidDeviceGeometry;
  }
  return DeviceGeometryHandle{value};
}

void gl_destroy_geometry(DeviceGeometryHandle geometry) noexcept {
  if (geometry.value == 0U) {
    return;
  }
  RenderDeviceContext &ctx = render_device_context();
  GlGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  const GLuint vao = record->vao;
  ctx.geometries.release(geometry.value);
  if (ctx.boundVao == vao) {
    bind_vao_direct(0U);
  }
  gl_table().deleteVertexArrays(1, &vao);
}

bool gl_set_geometry_instance_stream(DeviceGeometryHandle geometry,
                                     DeviceBufferHandle buffer,
                                     const VertexLayout &layout) noexcept {
  RenderDeviceContext &ctx = render_device_context();
  GlGeometryRecord *geoRecord = ctx.geometries.resolve(geometry.value);
  GlBufferRecord *bufRecord = ctx.buffers.resolve(buffer.value);
  if ((geoRecord == nullptr) || (bufRecord == nullptr) ||
      (bufRecord->usage != BufferUsage::Vertex) ||
      !vertex_layout_valid(layout)) {
    drop_operation("set_geometry_instance_stream: invalid arguments");
    return false;
  }
  bind_vao_direct(geoRecord->vao);
  gl_table().bindBuffer(GL_ARRAY_BUFFER, bufRecord->id);
  apply_vertex_layout(layout, 1U);
  gl_table().bindBuffer(GL_ARRAY_BUFFER, 0U);
  return true;
}

// --- Draws ---

void gl_draw(DeviceGeometryHandle geometry, PrimitiveTopology topology,
             std::int32_t firstVertex, std::int32_t vertexCount) noexcept {
  GlGeometryRecord *record =
      render_device_context().geometries.resolve(geometry.value);
  if (record == nullptr) {
    drop_operation("draw: stale geometry handle");
    return;
  }
  bind_vao_cached(record->vao);
  const GLenum mode =
      (topology == PrimitiveTopology::Lines) ? GL_LINES : GL_TRIANGLES;
  gl_table().drawArrays(mode, static_cast<GLint>(firstVertex),
                        static_cast<GLsizei>(vertexCount));
}

void gl_draw_indexed(DeviceGeometryHandle geometry,
                     std::int32_t indexCount) noexcept {
  GlGeometryRecord *record =
      render_device_context().geometries.resolve(geometry.value);
  if ((record == nullptr) || !record->hasIndex) {
    drop_operation("draw_indexed: stale or non-indexed geometry");
    return;
  }
  bind_vao_cached(record->vao);
  gl_table().drawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount),
                          GL_UNSIGNED_INT, nullptr);
}

void gl_draw_indexed_instanced(DeviceGeometryHandle geometry,
                               std::int32_t indexCount,
                               std::int32_t instanceCount) noexcept {
  GlGeometryRecord *record =
      render_device_context().geometries.resolve(geometry.value);
  if ((record == nullptr) || !record->hasIndex) {
    drop_operation("draw_indexed_instanced: stale or non-indexed geometry");
    return;
  }
  bind_vao_cached(record->vao);
  gl_table().drawElementsInstanced(GL_TRIANGLES,
                                   static_cast<GLsizei>(indexCount),
                                   GL_UNSIGNED_INT, nullptr,
                                   static_cast<GLsizei>(instanceCount));
}

// --- Render targets ---

/// Resolves an attachment to its GL texture target/id; false when the
/// attachment references a stale texture or an out-of-range face/mip.
bool resolve_attachment(const RenderTargetAttachment &attachment,
                        bool wantDepthFormat, GLenum *outTarget,
                        GLuint *outId) noexcept {
  GlTextureRecord *record =
      render_device_context().textures.resolve(attachment.texture.value);
  if (record == nullptr) {
    return false;
  }
  const GlFormatInfo info = gl_format_info(record->format);
  if (info.depth != wantDepthFormat) {
    return false;
  }
  if (record->kind == TextureKind::Cube) {
    const int face = static_cast<int>(attachment.face);
    if ((face < 0) || (face > 5)) {
      return false;
    }
    *outTarget = static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face);
  } else {
    if (attachment.face != CubeFace::None) {
      return false;
    }
    *outTarget = GL_TEXTURE_2D;
  }
  const std::int32_t maxMip =
      (record->mipLevels > 1) ? (record->mipLevels - 1) : 0;
  if ((attachment.mipLevel < 0) ||
      ((record->mipLevels != 0) && (attachment.mipLevel > maxMip))) {
    return false;
  }
  *outId = record->id;
  return true;
}

RenderTargetHandle
gl_create_render_target(const RenderTargetDesc &desc) noexcept {
  if ((desc.colorCount > kMaxColorAttachments) ||
      ((desc.colorCount == 0U) && (desc.depth.texture.value == 0U))) {
    drop_operation("create_render_target: invalid attachment set");
    return RenderTargetHandle{};
  }

  GLuint fbo = 0U;
  gl_table().genFramebuffers(1, &fbo);
  if (fbo == 0U) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "render target creation failed");
    return RenderTargetHandle{};
  }
  gl_table().bindFramebuffer(GL_FRAMEBUFFER, fbo);

  bool attachmentsValid = true;
  constexpr GLenum kAttachments[] = {GL_COLOR_ATTACHMENT0,
                                     GL_COLOR_ATTACHMENT0 + 1U,
                                     GL_COLOR_ATTACHMENT0 + 2U,
                                     GL_COLOR_ATTACHMENT0 + 3U};
  for (std::size_t i = 0U; i < desc.colorCount; ++i) {
    GLenum texTarget = 0U;
    GLuint texId = 0U;
    if (!resolve_attachment(desc.colors[i], false, &texTarget, &texId)) {
      attachmentsValid = false;
      break;
    }
    gl_table().framebufferTexture2D(GL_FRAMEBUFFER, kAttachments[i], texTarget,
                                    texId,
                                    static_cast<GLint>(desc.colors[i].mipLevel));
  }
  if (attachmentsValid && (desc.depth.texture.value != 0U)) {
    GLenum texTarget = 0U;
    GLuint texId = 0U;
    if (!resolve_attachment(desc.depth, true, &texTarget, &texId)) {
      attachmentsValid = false;
    } else {
      gl_table().framebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      texTarget, texId,
                                      static_cast<GLint>(desc.depth.mipLevel));
    }
  }

  if (attachmentsValid) {
    if (desc.colorCount == 0U) {
      // Depth-only targets must disable the color draw/read buffers, or
      // strict drivers report the framebuffer incomplete because
      // attachment 0 has no image (audit H-12).
      const GLenum none = GL_NONE;
      gl_table().drawBuffers(1, &none);
      gl_table().readBuffer(GL_NONE);
    } else {
      gl_table().drawBuffers(static_cast<GLsizei>(desc.colorCount),
                             kAttachments);
      gl_table().readBuffer(GL_COLOR_ATTACHMENT0);
    }
  }

  const bool complete =
      attachmentsValid && (gl_table().checkFramebufferStatus(GL_FRAMEBUFFER) ==
                           GL_FRAMEBUFFER_COMPLETE);
  gl_table().bindFramebuffer(GL_FRAMEBUFFER, 0U);
  if (!complete) {
    gl_table().deleteFramebuffers(1, &fbo);
    core::log_message(core::LogLevel::Error, "render_device",
                      "render target incomplete at creation — rejected");
    return RenderTargetHandle{};
  }

  const std::uint32_t value =
      render_device_context().targets.allocate(GlTargetRecord{fbo});
  if (value == 0U) {
    gl_table().deleteFramebuffers(1, &fbo);
    core::log_message(core::LogLevel::Error, "render_device",
                      "render target handle table exhausted");
    return RenderTargetHandle{};
  }
  return RenderTargetHandle{value};
}

void gl_destroy_render_target(RenderTargetHandle target) noexcept {
  if (target.value == 0U) {
    return;
  }
  GlTargetRecord *record =
      render_device_context().targets.resolve(target.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  const GLuint fbo = record->fbo;
  render_device_context().targets.release(target.value);
  gl_table().deleteFramebuffers(1, &fbo);
}

void gl_bind_render_target(RenderTargetHandle target) noexcept {
  if (target.value == 0U) {
    gl_table().bindFramebuffer(GL_FRAMEBUFFER, 0U);
    return;
  }
  GlTargetRecord *record =
      render_device_context().targets.resolve(target.value);
  if (record == nullptr) {
    drop_operation("bind_render_target: stale target handle");
    gl_table().bindFramebuffer(GL_FRAMEBUFFER, 0U);
    return;
  }
  gl_table().bindFramebuffer(GL_FRAMEBUFFER, record->fbo);
}

void gl_copy_depth(RenderTargetHandle source, RenderTargetHandle destination,
                   std::int32_t width, std::int32_t height) noexcept {
  GlTargetRecord *src = render_device_context().targets.resolve(source.value);
  GlTargetRecord *dst =
      render_device_context().targets.resolve(destination.value);
  if ((src == nullptr) || (dst == nullptr) || (width <= 0) || (height <= 0)) {
    drop_operation("copy_depth: stale target or invalid size");
    return;
  }
  gl_table().bindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
  gl_table().bindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
  gl_table().blitFramebuffer(0, 0, width, height, 0, 0, width, height,
                             GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  gl_table().bindFramebuffer(GL_READ_FRAMEBUFFER, 0U);
  gl_table().bindFramebuffer(GL_DRAW_FRAMEBUFFER, 0U);
}

// --- State, viewport, clear ---

void gl_apply_render_state(const RenderState &state) noexcept {
  if (state.depthTest == DepthTest::Disabled) {
    gl_table().disable(GL_DEPTH_TEST);
  } else {
    gl_table().enable(GL_DEPTH_TEST);
    gl_table().depthFunc(
        (state.depthTest == DepthTest::LessEqual) ? GL_LEQUAL : GL_LESS);
  }
  gl_table().depthMask(state.depthWrite ? GL_TRUE
                                        : static_cast<GLboolean>(GL_FALSE));
  if (state.blend == BlendMode::Alpha) {
    gl_table().enable(GL_BLEND);
    gl_table().blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    gl_table().disable(GL_BLEND);
  }
  if (state.cull == CullMode::Back) {
    gl_table().enable(GL_CULL_FACE);
  } else {
    gl_table().disable(GL_CULL_FACE);
  }
}

void gl_set_viewport(std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h) noexcept {
  gl_table().viewport(static_cast<GLint>(x), static_cast<GLint>(y),
                      static_cast<GLsizei>(w), static_cast<GLsizei>(h));
}

void gl_clear(ClearFlags flags, float r, float g, float b, float a) noexcept {
  GLbitfield mask = 0U;
  const auto bits = static_cast<std::uint8_t>(flags);
  if ((bits & static_cast<std::uint8_t>(ClearFlags::Color)) != 0U) {
    gl_table().clearColor(r, g, b, a);
    mask |= GL_COLOR_BUFFER_BIT;
  }
  if ((bits & static_cast<std::uint8_t>(ClearFlags::Depth)) != 0U) {
    mask |= GL_DEPTH_BUFFER_BIT;
  }
  if (mask != 0U) {
    gl_table().clear(mask);
  }
}

// --- Timestamp queries ---

DeviceQueryHandle gl_create_timestamp_query() noexcept {
  GLuint id = 0U;
  gl_table().genQueries(1, &id);
  if (id == 0U) {
    return kInvalidDeviceQuery;
  }
  const std::uint32_t value =
      render_device_context().queries.allocate(GlQueryRecord{id});
  if (value == 0U) {
    gl_table().deleteQueries(1, &id);
    return kInvalidDeviceQuery;
  }
  return DeviceQueryHandle{value};
}

void gl_destroy_timestamp_query(DeviceQueryHandle query) noexcept {
  if (query.value == 0U) {
    return;
  }
  GlQueryRecord *record =
      render_device_context().queries.resolve(query.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  const GLuint id = record->id;
  render_device_context().queries.release(query.value);
  gl_table().deleteQueries(1, &id);
}

void gl_write_timestamp(DeviceQueryHandle query) noexcept {
  GlQueryRecord *record =
      render_device_context().queries.resolve(query.value);
  if (record == nullptr) {
    return;
  }
  gl_table().queryCounter(record->id, GL_TIMESTAMP);
}

bool gl_timestamp_ready(DeviceQueryHandle query) noexcept {
  GlQueryRecord *record =
      render_device_context().queries.resolve(query.value);
  if (record == nullptr) {
    return false;
  }
  GLuint64 available = 0U;
  gl_table().getQueryObjectui64v(record->id, GL_QUERY_RESULT_AVAILABLE,
                                 &available);
  return available != 0U;
}

std::uint64_t gl_timestamp_value(DeviceQueryHandle query) noexcept {
  GlQueryRecord *record =
      render_device_context().queries.resolve(query.value);
  if (record == nullptr) {
    return 0U;
  }
  GLuint64 value = 0U;
  gl_table().getQueryObjectui64v(record->id, GL_QUERY_RESULT, &value);
  return static_cast<std::uint64_t>(value);
}

// --- Diagnostics ---

std::uint64_t gl_native_texture_id(DeviceTextureHandle texture) noexcept {
  GlTextureRecord *record =
      render_device_context().textures.resolve(texture.value);
  return (record != nullptr) ? static_cast<std::uint64_t>(record->id) : 0U;
}

DeviceDebugStats gl_debug_stats() noexcept {
  return render_device_context().stats;
}

} // namespace

/// Initializes the owning system for render device.
bool initialize_render_device() noexcept {
  RenderDeviceContext &ctx = render_device_context();
  if (ctx.initialized) {
    return true;
  }

  // #196: the null backend lets pipeline initialization and the frame
  // stages run where no GL exists (headless CI lanes); it shares this
  // context's lifecycle, and shutdown_render_device is already GL-free.
  if (core::cvar_get_bool("r_null_device", false)) {
    fill_null_render_device(&ctx.device);
    ctx.initialized = true;
    core::log_message(core::LogLevel::Info, "renderer",
                      "render device: null backend (r_null_device)");
    return true;
  }

  if (!load_all_gl_functions()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load OpenGL functions for render device");
    return false;
  }

  RenderDevice &device = ctx.device;
  // load_all_gl_functions is all-or-nothing, so every optional feature
  // the contract models is present on a loaded GL 4.5 context.
  device.caps.instancing = true;
  device.caps.uniformBlocks = true;
  device.caps.timestampQueries = true;

  device.create_buffer = &gl_create_buffer;
  device.update_buffer = &gl_update_buffer;
  device.update_buffer_range = &gl_update_buffer_range;
  device.destroy_buffer = &gl_destroy_buffer;
  device.bind_uniform_buffer_slot = &gl_bind_uniform_buffer_slot;
  device.create_texture = &gl_create_texture;
  device.update_texture = &gl_update_texture;
  device.destroy_texture = &gl_destroy_texture;
  device.bind_texture_slot = &gl_bind_texture_slot;
  device.create_program = &gl_create_program;
  device.destroy_program = &gl_destroy_program;
  device.bind_program = &gl_bind_program;
  device.shader_param = &gl_shader_param;
  device.set_param_mat4 = &gl_set_param_mat4;
  device.set_param_mat3 = &gl_set_param_mat3;
  device.set_param_f32 = &gl_set_param_f32;
  device.set_param_i32 = &gl_set_param_i32;
  device.set_param_vec2 = &gl_set_param_vec2;
  device.set_param_vec3 = &gl_set_param_vec3;
  device.set_param_vec4 = &gl_set_param_vec4;
  device.bind_program_uniform_block = &gl_bind_program_uniform_block;
  device.create_geometry = &gl_create_geometry;
  device.destroy_geometry = &gl_destroy_geometry;
  device.set_geometry_instance_stream = &gl_set_geometry_instance_stream;
  device.draw = &gl_draw;
  device.draw_indexed = &gl_draw_indexed;
  device.draw_indexed_instanced = &gl_draw_indexed_instanced;
  device.create_render_target = &gl_create_render_target;
  device.destroy_render_target = &gl_destroy_render_target;
  device.bind_render_target = &gl_bind_render_target;
  device.copy_depth = &gl_copy_depth;
  device.apply_render_state = &gl_apply_render_state;
  device.set_viewport = &gl_set_viewport;
  device.clear = &gl_clear;
  device.create_timestamp_query = &gl_create_timestamp_query;
  device.destroy_timestamp_query = &gl_destroy_timestamp_query;
  device.write_timestamp = &gl_write_timestamp;
  device.timestamp_ready = &gl_timestamp_ready;
  device.timestamp_value = &gl_timestamp_value;
  device.native_texture_id = &gl_native_texture_id;
  device.debug_stats = &gl_debug_stats;

  ctx.initialized = true;
  return true;
}

/// Shuts down the owning system for render device.
void shutdown_render_device() noexcept {
  RenderDeviceContext &ctx = render_device_context();
  // Invalidate every outstanding handle; owning systems destroy their GL
  // objects before this point (shutdown_renderer ordering), and the GL
  // context itself is torn down by the platform layer afterwards.
  ctx.buffers.clear();
  ctx.textures.clear();
  ctx.programs.clear();
  ctx.geometries.clear();
  ctx.targets.clear();
  ctx.queries.clear();
  ctx.stats = DeviceDebugStats{};
  ctx.boundVao = 0U;
  ctx.device = RenderDevice{};
  ctx.gl = GlTable{};
  ctx.initialized = false;
}

const RenderDevice *render_device() noexcept {
  if (!render_device_context().initialized) {
    return nullptr;
  }
  return &render_device_context().device;
}

bool render_backend_owns_swapchain() noexcept { return false; }

void present_render_device() noexcept { core::swap_render_buffers(); }

} // namespace engine::renderer
