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
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

namespace {

constexpr float kDefaultFovRadians = 1.0471975512F;
constexpr float kNearClip = 0.1F;
constexpr float kFarClip = 100.0F;
constexpr float kClearRed = 0.18F;
constexpr float kClearGreen = 0.28F;
constexpr float kClearBlue = 0.60F;

CameraState g_activeCamera{};
int g_sceneViewportWidth = 0;
int g_sceneViewportHeight = 0;

struct BackendState final {
  bool initialized = false;
  bool failed = false;

  // Fallback shader (kept for compatibility).
  ShaderProgramHandle defaultShaderHandle{};
  std::uint32_t defaultProgram = 0U;

  // PBR shader.
  ShaderProgramHandle pbrShaderHandle{};
  std::uint32_t pbrProgram = 0U;

  // PBR uniform locations.
  std::int32_t pbrModelLocation = -1;
  std::int32_t pbrMvpLocation = -1;
  std::int32_t pbrNormalMatrixLocation = -1;
  std::int32_t pbrAlbedoLocation = -1;
  std::int32_t pbrRoughnessLocation = -1;
  std::int32_t pbrMetallicLocation = -1;
  std::int32_t pbrTimeLocation = -1;
  std::int32_t pbrCameraPosLocation = -1;
  std::int32_t pbrHasAlbedoTextureLocation = -1;
  std::int32_t pbrAlbedoMapLocation = -1;

  // Directional lights.
  std::int32_t pbrDirLightCountLocation = -1;
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightDir{};
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightColor{};
  std::array<std::int32_t, kMaxDirectionalLights> pbrDirLightIntensity{};

  // Point lights.
  std::int32_t pbrPointLightCountLocation = -1;
  std::array<std::int32_t, kMaxPointLights> pbrPointLightPos{};
  std::array<std::int32_t, kMaxPointLights> pbrPointLightColor{};
  std::array<std::int32_t, kMaxPointLights> pbrPointLightIntensity{};

  // Tonemap shader.
  ShaderProgramHandle tonemapShaderHandle{};
  std::uint32_t tonemapProgram = 0U;
  std::int32_t tonemapSceneColorLocation = -1;
  std::int32_t tonemapExposureLocation = -1;

  // Empty VAO for fullscreen triangle.
  std::uint32_t emptyVao = 0U;

  // Tracked drawable dimensions for pass resource resize.
  int lastWidth = 0;
  int lastHeight = 0;
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

void resolve_pbr_light_uniforms(BackendState &backend,
                                const RenderDevice *dev) noexcept {
  const std::uint32_t prog = backend.pbrProgram;

  backend.pbrDirLightCountLocation =
      dev->uniform_location(prog, "u_dirLightCount");
  for (std::size_t i = 0U; i < kMaxDirectionalLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].direction", i);
    backend.pbrDirLightDir[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].color", i);
    backend.pbrDirLightColor[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_dirLights[%zu].intensity", i);
    backend.pbrDirLightIntensity[i] = dev->uniform_location(prog, name);
    if ((backend.pbrDirLightDir[i] < 0) || (backend.pbrDirLightColor[i] < 0) ||
        (backend.pbrDirLightIntensity[i] < 0)) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing directional light uniforms at "
                        "index — lights will be invisible");
    }
  }

  backend.pbrPointLightCountLocation =
      dev->uniform_location(prog, "u_pointLightCount");
  for (std::size_t i = 0U; i < kMaxPointLights; ++i) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].position", i);
    backend.pbrPointLightPos[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].color", i);
    backend.pbrPointLightColor[i] = dev->uniform_location(prog, name);
    std::snprintf(name, sizeof(name), "u_pointLights[%zu].intensity", i);
    backend.pbrPointLightIntensity[i] = dev->uniform_location(prog, name);
    if ((backend.pbrPointLightPos[i] < 0) ||
        (backend.pbrPointLightColor[i] < 0) ||
        (backend.pbrPointLightIntensity[i] < 0)) {
      core::log_message(core::LogLevel::Warning, "renderer",
                        "PBR shader missing point light uniforms at "
                        "index — lights will be invisible");
    }
  }
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
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to initialize render device");
    reset_backend_on_failure();
    return false;
  }

  if (!initialize_shader_system()) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to initialize shader system");
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const RenderDevice *dev = render_device();

  // Load default fallback shader.
  const ShaderProgramHandle defaultShaderHandle = load_shader_program(
      "assets/shaders/default.vert", "assets/shaders/default.frag");
  if (defaultShaderHandle == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load default shader program");
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t defaultProgram = shader_gpu_program(defaultShaderHandle);
  if (defaultProgram == 0U) {
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  backend.defaultShaderHandle = defaultShaderHandle;
  backend.defaultProgram = defaultProgram;

  // Load PBR shader.
  const ShaderProgramHandle pbrShaderHandle =
      load_shader_program("assets/shaders/pbr.vert", "assets/shaders/pbr.frag");
  if (pbrShaderHandle == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load PBR shader program");
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t pbrProgram = shader_gpu_program(pbrShaderHandle);
  if (pbrProgram == 0U) {
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  backend.pbrShaderHandle = pbrShaderHandle;
  backend.pbrProgram = pbrProgram;

  // Resolve PBR uniform locations.
  backend.pbrModelLocation = dev->uniform_location(pbrProgram, "u_model");
  backend.pbrMvpLocation = dev->uniform_location(pbrProgram, "u_mvp");
  backend.pbrNormalMatrixLocation =
      dev->uniform_location(pbrProgram, "u_normalMatrix");
  backend.pbrAlbedoLocation = dev->uniform_location(pbrProgram, "u_albedo");
  backend.pbrRoughnessLocation =
      dev->uniform_location(pbrProgram, "u_roughness");
  backend.pbrMetallicLocation = dev->uniform_location(pbrProgram, "u_metallic");
  backend.pbrTimeLocation = dev->uniform_location(pbrProgram, "u_time");
  backend.pbrCameraPosLocation =
      dev->uniform_location(pbrProgram, "u_cameraPos");
  backend.pbrHasAlbedoTextureLocation =
      dev->uniform_location(pbrProgram, "u_hasAlbedoTexture");
  backend.pbrAlbedoMapLocation =
      dev->uniform_location(pbrProgram, "u_albedoMap");

  if ((backend.pbrMvpLocation < 0) || (backend.pbrNormalMatrixLocation < 0) ||
      (backend.pbrAlbedoLocation < 0)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to locate required PBR shader uniforms");
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  resolve_pbr_light_uniforms(backend, dev);

  // Load tonemap shader.
  const ShaderProgramHandle tonemapShaderHandle = load_shader_program(
      "assets/shaders/fullscreen.vert", "assets/shaders/tonemap.frag");
  if (tonemapShaderHandle == kInvalidShaderProgram) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to load tonemap shader program");
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  const std::uint32_t tonemapProgram = shader_gpu_program(tonemapShaderHandle);
  if (tonemapProgram == 0U) {
    destroy_shader_program(tonemapShaderHandle);
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
    reset_backend_on_failure();
    return false;
  }

  backend.tonemapShaderHandle = tonemapShaderHandle;
  backend.tonemapProgram = tonemapProgram;
  backend.tonemapSceneColorLocation =
      dev->uniform_location(tonemapProgram, "u_sceneColor");
  backend.tonemapExposureLocation =
      dev->uniform_location(tonemapProgram, "u_exposure");

  // Empty VAO for fullscreen triangle (required by core profile).
  backend.emptyVao = dev->create_vertex_array();
  if (backend.emptyVao == 0U) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "failed to create empty VAO for fullscreen pass");
    destroy_shader_program(tonemapShaderHandle);
    destroy_shader_program(pbrShaderHandle);
    destroy_shader_program(defaultShaderHandle);
    shutdown_shader_system();
    shutdown_render_device();
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

  shutdown_pass_resources();

  const RenderDevice *dev = render_device();

  if (backend->emptyVao != 0U && dev != nullptr) {
    dev->destroy_vertex_array(backend->emptyVao);
    backend->emptyVao = 0U;
  }

  if (backend->tonemapShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->tonemapShaderHandle);
    backend->tonemapShaderHandle = ShaderProgramHandle{};
  }
  backend->tonemapProgram = 0U;

  if (backend->pbrShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->pbrShaderHandle);
    backend->pbrShaderHandle = ShaderProgramHandle{};
  }
  if (backend->defaultShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend->defaultShaderHandle);
    backend->defaultShaderHandle = ShaderProgramHandle{};
  }
  backend->pbrProgram = 0U;
  backend->defaultProgram = 0U;
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

void CommandBufferBuilder::reset() noexcept { m_commandCount = 0U; }

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

  std::memcpy(m_commands.data() + m_commandCount, other.m_commands.data(),
              sizeof(DrawCommand) * other.m_commandCount);
  m_commandCount += other.m_commandCount;

  return true;
}

void CommandBufferBuilder::sort_by_key() noexcept {
  std::sort(m_commands.begin(),
            m_commands.begin() + static_cast<std::ptrdiff_t>(m_commandCount),
            [](const DrawCommand &lhs, const DrawCommand &rhs) {
              return lhs.sortKey.value < rhs.sortKey.value;
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
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &lights) noexcept {
  if (!initialize_backend()) {
    return;
  }

  BackendState &backend = backend_state();
  const RenderDevice *dev = render_device();

  int drawableWidth = 1280;
  int drawableHeight = 720;
  if ((g_sceneViewportWidth > 0) && (g_sceneViewportHeight > 0)) {
    drawableWidth = g_sceneViewportWidth;
    drawableHeight = g_sceneViewportHeight;
  } else {
    core::render_drawable_size(&drawableWidth, &drawableHeight);
  }
  if (drawableWidth <= 0) {
    drawableWidth = 1;
  }
  if (drawableHeight <= 0) {
    drawableHeight = 1;
  }

  // Initialize or resize pass resources when dimensions change.
  if (backend.lastWidth != drawableWidth ||
      backend.lastHeight != drawableHeight) {
    if (backend.lastWidth == 0 && backend.lastHeight == 0) {
      initialize_pass_resources(drawableWidth, drawableHeight);
    } else {
      resize_pass_resources(drawableWidth, drawableHeight);
    }
    backend.lastWidth = drawableWidth;
    backend.lastHeight = drawableHeight;
  }

  const PassResources &passRes = get_pass_resources();
  const std::uint32_t sceneFbo = pass_resource_framebuffer(passRes.sceneColor);

  // --- Scene pass: render to HDR FBO ---
  dev->bind_framebuffer(sceneFbo);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->enable_depth_test();
  dev->set_clear_color(kClearRed, kClearGreen, kClearBlue, 1.0F);
  dev->clear_color_depth();

  if (registry == nullptr) {
    dev->bind_framebuffer(0U);
    return;
  }

  // Use PBR shader.
  dev->bind_program(backend.pbrProgram);

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

  // Per-frame uniforms.
  if (backend.pbrTimeLocation >= 0) {
    dev->set_uniform_float(backend.pbrTimeLocation, timeSeconds);
  }
  if (backend.pbrCameraPosLocation >= 0) {
    dev->set_uniform_vec3(backend.pbrCameraPosLocation,
                          &g_activeCamera.position.x);
  }

  // Upload directional lights.
  if (backend.pbrDirLightCountLocation >= 0) {
    dev->set_uniform_int(
        backend.pbrDirLightCountLocation,
        static_cast<std::int32_t>(lights.directionalLightCount));
  }
  for (std::size_t i = 0U; i < lights.directionalLightCount; ++i) {
    const auto &dl = lights.directionalLights[i];
    if (backend.pbrDirLightDir[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrDirLightDir[i], &dl.direction.x);
    }
    if (backend.pbrDirLightColor[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrDirLightColor[i], &dl.color.x);
    }
    if (backend.pbrDirLightIntensity[i] >= 0) {
      dev->set_uniform_float(backend.pbrDirLightIntensity[i], dl.intensity);
    }
  }

  // Upload point lights.
  if (backend.pbrPointLightCountLocation >= 0) {
    dev->set_uniform_int(backend.pbrPointLightCountLocation,
                         static_cast<std::int32_t>(lights.pointLightCount));
  }
  for (std::size_t i = 0U; i < lights.pointLightCount; ++i) {
    const auto &pl = lights.pointLights[i];
    if (backend.pbrPointLightPos[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrPointLightPos[i], &pl.position.x);
    }
    if (backend.pbrPointLightColor[i] >= 0) {
      dev->set_uniform_vec3(backend.pbrPointLightColor[i], &pl.color.x);
    }
    if (backend.pbrPointLightIntensity[i] >= 0) {
      dev->set_uniform_float(backend.pbrPointLightIntensity[i], pl.intensity);
    }
  }

  // Set albedo map sampler to texture unit 0.
  if (backend.pbrAlbedoMapLocation >= 0) {
    dev->set_uniform_int(backend.pbrAlbedoMapLocation, 0);
  }

  if ((commandBufferView.count > 0U) && (commandBufferView.data == nullptr)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "draw command view is invalid");
  }

  // Determine opaque / transparent partition.
  // After sort_by_key, all opaque commands (transparent bit = 0) precede
  // transparent ones (transparent bit = 1, MSB of the 64-bit key).
  constexpr std::uint64_t kTransparentBit = 1ULL << 63;

  std::size_t opaqueCount = 0U;
  std::size_t totalCount = 0U;

  if ((commandBufferView.data != nullptr) && (commandBufferView.count > 0U)) {
    totalCount = static_cast<std::size_t>(commandBufferView.count);
    for (std::size_t i = 0U; i < totalCount; ++i) {
      if ((commandBufferView.data[i].sortKey.value & kTransparentBit) != 0U) {
        break;
      }
      opaqueCount = i + 1U;
    }
  }

  // Helper: draw a range of draw commands.
  auto drawRange = [&](std::size_t start, std::size_t end) {
    std::uint32_t boundVertexArray = 0U;
    std::uint32_t boundAlbedoTexture = 0U;

    for (std::size_t i = start; i < end; ++i) {
      const DrawCommand &command = commandBufferView.data[i];
      const GpuMesh *mesh = lookup_gpu_mesh(registry, command.mesh);
      if ((mesh == nullptr) || (mesh->vertexArray == 0U) ||
          (mesh->vertexCount == 0U)) {
        continue;
      }

      if (mesh->vertexArray != boundVertexArray) {
        dev->bind_vertex_array(mesh->vertexArray);
        boundVertexArray = mesh->vertexArray;
      }

      // Material uniforms.
      if (backend.pbrAlbedoLocation >= 0) {
        dev->set_uniform_vec3(backend.pbrAlbedoLocation,
                              &command.material.albedo.x);
      }
      if (backend.pbrRoughnessLocation >= 0) {
        dev->set_uniform_float(backend.pbrRoughnessLocation,
                               command.material.roughness);
      }
      if (backend.pbrMetallicLocation >= 0) {
        dev->set_uniform_float(backend.pbrMetallicLocation,
                               command.material.metallic);
      }

      // Albedo texture binding with state tracking.
      const std::uint32_t albedoGpuId =
          texture_gpu_id(command.material.albedoTexture);
      const bool hasAlbedoTex =
          (command.material.albedoTexture != kInvalidTextureHandle) &&
          (albedoGpuId != 0U);
      if (backend.pbrHasAlbedoTextureLocation >= 0) {
        dev->set_uniform_int(backend.pbrHasAlbedoTextureLocation,
                             hasAlbedoTex ? 1 : 0);
      }
      if (hasAlbedoTex && (albedoGpuId != boundAlbedoTexture)) {
        dev->bind_texture(0, albedoGpuId);
        boundAlbedoTexture = albedoGpuId;
      } else if (!hasAlbedoTex && (boundAlbedoTexture != 0U)) {
        dev->bind_texture(0, 0U);
        boundAlbedoTexture = 0U;
      }

      const math::Mat4 model = compute_model_matrix(command);
      const math::Mat4 mvp = compute_mvp(model, viewProjection);
      float normalMatrix[9] = {};
      extract_normal_matrix(model, normalMatrix);

      if (backend.pbrModelLocation >= 0) {
        dev->set_uniform_mat4(backend.pbrModelLocation, &model.columns[0].x);
      }
      dev->set_uniform_mat4(backend.pbrMvpLocation, &mvp.columns[0].x);
      dev->set_uniform_mat3(backend.pbrNormalMatrixLocation, normalMatrix);

      if (mesh->indexCount > 0U) {
        dev->draw_elements_triangles_u32(
            static_cast<std::int32_t>(mesh->indexCount));
      } else {
        dev->draw_arrays_triangles(
            0, static_cast<std::int32_t>(mesh->vertexCount));
      }
    }
  };

  // Pass 1: Opaque — depth write ON, blending OFF, face culling ON.
  dev->set_depth_mask(true);
  dev->disable_blending();
  dev->enable_face_culling();
  drawRange(0U, opaqueCount);

  // Pass 2: Transparent — depth write OFF, blending ON (alpha), culling OFF.
  if (opaqueCount < totalCount) {
    dev->set_depth_mask(false);
    dev->enable_blending();
    dev->set_blend_func_alpha();
    dev->disable_face_culling();
    drawRange(opaqueCount, totalCount);

    // Restore depth/blend/cull state.
    dev->set_depth_mask(true);
    dev->disable_blending();
    dev->enable_face_culling();
  }

  dev->bind_texture(0, 0U);
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);

  // --- Tonemap pass: HDR scene → LDR final FBO ---
  const std::uint32_t finalFbo = pass_resource_framebuffer(passRes.finalColor);
  dev->bind_framebuffer(finalFbo);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->disable_depth_test();

  dev->bind_program(backend.tonemapProgram);

  const std::uint32_t sceneColorTex =
      pass_resource_gpu_texture(passRes.sceneColor);
  dev->bind_texture(0, sceneColorTex);
  if (backend.tonemapSceneColorLocation >= 0) {
    dev->set_uniform_int(backend.tonemapSceneColorLocation, 0);
  }
  if (backend.tonemapExposureLocation >= 0) {
    dev->set_uniform_float(backend.tonemapExposureLocation, 1.0F);
  }

  dev->bind_vertex_array(backend.emptyVao);
  dev->draw_arrays_triangles(0, 3);

  dev->bind_texture(0, 0U);
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);

  // --- Prepare back buffer for editor overlay (ImGui) ---
  dev->bind_framebuffer(0U);
  dev->set_viewport(0, 0, drawableWidth, drawableHeight);
  dev->set_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
  dev->clear_color_depth();
  dev->enable_depth_test();
}

void shutdown_renderer() noexcept {
  BackendState &backend = backend_state();
  if (!backend.initialized && !backend.failed) {
    return;
  }

  if (core::make_render_context_current()) {
    destroy_backend_resources(&backend);
    shutdown_shader_system();
    shutdown_render_device();
    core::release_render_context();
  }

  backend = BackendState{};
}

void set_active_camera(const CameraState &camera) noexcept {
  g_activeCamera = camera;
}

void set_scene_viewport_size(int width, int height) noexcept {
  g_sceneViewportWidth = (width > 0) ? width : 0;
  g_sceneViewportHeight = (height > 0) ? height : 0;
}

CameraState get_active_camera() noexcept { return g_activeCamera; }

std::uint32_t get_scene_viewport_texture() noexcept {
  const PassResources &passRes = get_pass_resources();
  return pass_resource_gpu_texture(passRes.finalColor);
}

} // namespace engine::renderer
