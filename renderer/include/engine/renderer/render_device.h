#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

inline constexpr std::uint32_t kShaderStageVertex = 0U;
inline constexpr std::uint32_t kShaderStageFragment = 1U;

struct RenderDevice final {
  // Shader — compile source, return handle (0 on failure, logs errors).
  std::uint32_t (*create_shader)(std::uint32_t stage,
                                 const char *source) noexcept = nullptr;
  void (*destroy_shader)(std::uint32_t shader) noexcept = nullptr;

  // Program — link vertex + fragment shaders (0 on failure, logs errors).
  std::uint32_t (*link_program)(std::uint32_t vertShader,
                                std::uint32_t fragShader) noexcept = nullptr;
  void (*destroy_program)(std::uint32_t program) noexcept = nullptr;
  void (*bind_program)(std::uint32_t program) noexcept = nullptr;

  // Uniform queries and setters.
  std::int32_t (*uniform_location)(std::uint32_t program,
                                   const char *name) noexcept = nullptr;
  void (*set_uniform_mat4)(std::int32_t loc,
                           const float *value) noexcept = nullptr;
  void (*set_uniform_mat3)(std::int32_t loc,
                           const float *value) noexcept = nullptr;
  void (*set_uniform_float)(std::int32_t loc, float value) noexcept = nullptr;
  void (*set_uniform_vec3)(std::int32_t loc,
                           const float *value) noexcept = nullptr;
  void (*set_uniform_vec2)(std::int32_t loc,
                           const float *value) noexcept = nullptr;

  // Vertex arrays.
  std::uint32_t (*create_vertex_array)() noexcept = nullptr;
  void (*destroy_vertex_array)(std::uint32_t vao) noexcept = nullptr;
  void (*bind_vertex_array)(std::uint32_t vao) noexcept = nullptr;

  // Buffers.
  std::uint32_t (*create_buffer)() noexcept = nullptr;
  void (*destroy_buffer)(std::uint32_t buffer) noexcept = nullptr;
  void (*bind_array_buffer)(std::uint32_t buffer) noexcept = nullptr;
  void (*bind_element_buffer)(std::uint32_t buffer) noexcept = nullptr;
  void (*buffer_data_array)(const void *data,
                            std::ptrdiff_t sizeBytes) noexcept = nullptr;
  void (*buffer_data_element)(const void *data,
                              std::ptrdiff_t sizeBytes) noexcept = nullptr;

  // Vertex attributes.
  void (*enable_vertex_attrib)(std::uint32_t index) noexcept = nullptr;
  void (*vertex_attrib_float)(std::uint32_t index, std::int32_t components,
                              std::int32_t stride,
                              const void *offset) noexcept = nullptr;

  // Drawing.
  void (*draw_arrays_triangles)(std::int32_t first,
                                std::int32_t count) noexcept = nullptr;
  void (*draw_elements_triangles_u32)(std::int32_t count) noexcept = nullptr;

  // Uniform — additional types.
  void (*set_uniform_int)(std::int32_t loc,
                          std::int32_t value) noexcept = nullptr;
  void (*set_uniform_vec4)(std::int32_t loc,
                           const float *value) noexcept = nullptr;

  // Textures.
  std::uint32_t (*create_texture_2d)(std::int32_t width, std::int32_t height,
                                     std::int32_t channels,
                                     const void *data) noexcept = nullptr;
  std::uint32_t (*create_texture_2d_hdr)(std::int32_t width,
                                         std::int32_t height,
                                         std::int32_t channels,
                                         const float *data) noexcept = nullptr;
  std::uint32_t (*create_depth_texture)(std::int32_t width,
                                        std::int32_t height) noexcept = nullptr;
  void (*destroy_texture)(std::uint32_t id) noexcept = nullptr;
  void (*bind_texture)(std::int32_t unit, std::uint32_t id) noexcept = nullptr;

  // Framebuffers.
  std::uint32_t (*create_framebuffer)(
      std::uint32_t colorTex, std::uint32_t depthTex) noexcept = nullptr;
  std::uint32_t (*create_framebuffer_mrt)(
      const std::uint32_t *colorTextures, std::int32_t colorCount,
      std::uint32_t depthTex) noexcept = nullptr;
  void (*destroy_framebuffer)(std::uint32_t fbo) noexcept = nullptr;
  void (*bind_framebuffer)(std::uint32_t fbo) noexcept = nullptr;
  bool (*check_framebuffer_complete)() noexcept = nullptr;

  // Textures — single-channel float (R32F).
  std::uint32_t (*create_texture_2d_r32f)(std::int32_t width,
                                          std::int32_t height,
                                          const float *data) noexcept = nullptr;
  void (*update_texture_2d_r32f)(std::uint32_t texId, std::int32_t width,
                                 std::int32_t height,
                                 const float *data) noexcept = nullptr;

  // Blend.
  void (*enable_blending)() noexcept = nullptr;
  void (*disable_blending)() noexcept = nullptr;
  void (*set_blend_func_alpha)() noexcept = nullptr;

  // Face culling.
  void (*enable_face_culling)() noexcept = nullptr;
  void (*disable_face_culling)() noexcept = nullptr;

  // Depth mask.
  void (*set_depth_mask)(bool write) noexcept = nullptr;

  // GPU timestamp queries.
  std::uint32_t (*create_query)() noexcept = nullptr;
  void (*destroy_query)(std::uint32_t query) noexcept = nullptr;
  void (*query_counter_timestamp)(std::uint32_t query) noexcept = nullptr;
  bool (*query_result_available)(std::uint32_t query) noexcept = nullptr;
  std::uint64_t (*query_result_u64)(std::uint32_t query) noexcept = nullptr;

  // State.
  void (*set_viewport)(std::int32_t x, std::int32_t y, std::int32_t w,
                       std::int32_t h) noexcept = nullptr;
  void (*enable_depth_test)() noexcept = nullptr;
  void (*disable_depth_test)() noexcept = nullptr;
  void (*set_clear_color)(float r, float g, float b,
                          float a) noexcept = nullptr;
  void (*clear_color_depth)() noexcept = nullptr;
};

bool initialize_render_device() noexcept;
void shutdown_render_device() noexcept;
const RenderDevice *render_device() noexcept;

} // namespace engine::renderer
