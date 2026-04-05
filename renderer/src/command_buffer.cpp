#include "engine/renderer/command_buffer.h"

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
#include "engine/renderer/camera.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

constexpr float kDefaultFovRadians = 1.0471975512F;
constexpr float kNearClip = 0.1F;
constexpr float kFarClip = 100.0F;
constexpr float kClearRed = 0.06F;
constexpr float kClearGreen = 0.08F;
constexpr float kClearBlue = 0.12F;

CameraState g_activeCamera{};

struct BackendState final {
  bool initialized = false;
  bool failed = false;
  std::uint32_t program = 0U;
  std::int32_t mvpLocation = -1;
  std::int32_t normalMatrixLocation = -1;
  std::int32_t timeLocation = -1;
  std::int32_t albedoLocation = -1;
};

BackendState &backend_state() noexcept {
  static BackendState state{};
  return state;
}

void reset_backend_on_failure() noexcept {
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.failed = true;
}

bool initialize_backend() noexcept {
  BackendState &backend = backend_state();
  if (backend.initialized) {
    return true;
  }

  if (backend.failed) {
    return false;
  }

  if (!initialize_render_device()) {
    core::log_message(core::LogLevel::Error,
                      "renderer",
                      "failed to initialize render device");
    reset_backend_on_failure();
    return false;
  }

  const RenderDevice *dev = render_device();

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

  const std::uint32_t vertexShader =
      dev->create_shader(kShaderStageVertex, kVertexShaderSource);
  const std::uint32_t fragmentShader =
      dev->create_shader(kShaderStageFragment, kFragmentShaderSource);

  if ((vertexShader == 0U) || (fragmentShader == 0U)) {
    if (vertexShader != 0U) {
      dev->destroy_shader(vertexShader);
    }
    if (fragmentShader != 0U) {
      dev->destroy_shader(fragmentShader);
    }
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t program = dev->link_program(vertexShader, fragmentShader);
  dev->destroy_shader(vertexShader);
  dev->destroy_shader(fragmentShader);

  if (program == 0U) {
    reset_backend_on_failure();
    return false;
  }

  backend.program = program;
  backend.mvpLocation = dev->uniform_location(program, "u_mvp");
  backend.normalMatrixLocation =
      dev->uniform_location(program, "u_normalMatrix");
  backend.timeLocation = dev->uniform_location(program, "u_time");
  backend.albedoLocation = dev->uniform_location(program, "u_albedo");
  if ((backend.mvpLocation < 0) || (backend.normalMatrixLocation < 0)
      || (backend.albedoLocation < 0)) {
    core::log_message(core::LogLevel::Error,
                      "renderer",
                      "failed to locate required shader uniforms");
    dev->destroy_program(program);
    reset_backend_on_failure();
    return false;
  }

  backend.initialized = true;
  return true;
}

void destroy_backend_resources(BackendState *backend) noexcept {
  if (backend == nullptr) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((backend->program != 0U) && (dev != nullptr)) {
    dev->destroy_program(backend->program);
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

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();

  int drawableWidth = 1280;
  int drawableHeight = 720;
  core::render_drawable_size(&drawableWidth, &drawableHeight);
  if (drawableWidth <= 0) {
    drawableWidth = 1;
  }
  if (drawableHeight <= 0) {
    drawableHeight = 1;
  }

  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->enable_depth_test();
  dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
  dev->clear_color_depth();

  if (registry == nullptr) {
    return;
  }

  dev->bind_program(backend.program);

  std::uint32_t boundVertexArray = 0U;

  const float aspect =
      static_cast<float>(drawableWidth) / static_cast<float>(drawableHeight);
  const math::Mat4 view = math::look_at(
      g_activeCamera.position, g_activeCamera.target, g_activeCamera.up);
  const float fov = (g_activeCamera.fovRadians > 0.0F)
                        ? g_activeCamera.fovRadians
                        : kDefaultFovRadians;
  const float nearP =
      (g_activeCamera.nearPlane > 0.0F) ? g_activeCamera.nearPlane : kNearClip;
  const float farP =
      (g_activeCamera.farPlane > nearP) ? g_activeCamera.farPlane : kFarClip;
  const math::Mat4 projection = math::perspective(fov, aspect, nearP, farP);
  const math::Mat4 viewProjection = math::mul(projection, view);
  if (backend.timeLocation >= 0) {
    dev->set_uniform_float(backend.timeLocation, timeSeconds);
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
        dev->bind_vertex_array(mesh->vertexArray);
        boundVertexArray = mesh->vertexArray;
      }

      if (backend.albedoLocation >= 0) {
        dev->set_uniform_vec3(backend.albedoLocation,
                              &command.material.albedo.x);
      }

      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);
      dev->set_uniform_mat4(backend.mvpLocation, &mvp.columns[0].x);
      dev->set_uniform_mat3(backend.normalMatrixLocation, normalMatrix);

      if (mesh->indexCount > 0U) {
        dev->draw_elements_triangles_u32(
            static_cast<std::int32_t>(mesh->indexCount));
      } else {
        dev->draw_arrays_triangles(
            0, static_cast<std::int32_t>(mesh->vertexCount));
      }
    }
  }

  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
}

void shutdown_renderer() noexcept {
  BackendState &backend = backend_state();
  if (!backend.initialized && !backend.failed) {
    return;
  }

  if (core::make_render_context_current()) {
    destroy_backend_resources(&backend);
    shutdown_render_device();
    core::release_render_context();
  }

  backend = BackendState{};
}

void set_active_camera(const CameraState &camera) noexcept {
  g_activeCamera = camera;
}

CameraState get_active_camera() noexcept {
  return g_activeCamera;
}

} // namespace engine::renderer
