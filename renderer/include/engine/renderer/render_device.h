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
  void (*vertex_attrib_float)(std::uint32_t index,
                              std::int32_t components,
                              std::int32_t stride,
                              const void *offset) noexcept = nullptr;

  // Drawing.
  void (*draw_arrays_triangles)(std::int32_t first,
                                std::int32_t count) noexcept = nullptr;
  void (*draw_elements_triangles_u32)(std::int32_t count) noexcept = nullptr;

  // State.
  void (*set_viewport)(std::int32_t x,
                       std::int32_t y,
                       std::int32_t w,
                       std::int32_t h) noexcept = nullptr;
  void (*enable_depth_test)() noexcept = nullptr;
  void (*set_clear_color)(float r,
                          float g,
                          float b,
                          float a) noexcept = nullptr;
  void (*clear_color_depth)() noexcept = nullptr;
};

bool initialize_render_device() noexcept;
void shutdown_render_device() noexcept;
const RenderDevice *render_device() noexcept;

} // namespace engine::renderer
