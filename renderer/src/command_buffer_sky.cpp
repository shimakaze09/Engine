// Implements skybox and procedural (Preetham, Hosek-Wilkie) sky rendering
// plus the shared skybox cube geometry for the renderer backend.
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#include "command_buffer_sky.h"

#include "command_buffer_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/gpu_profiler.h"
#include "engine/renderer/light_culling.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/pass_resources.h"
#include "engine/renderer/post_process_stack.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

namespace {

constexpr float kSkyboxCubeVertices[] = {
    -1.0F, 1.0F,  -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  -1.0F, -1.0F,
    1.0F,  -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, -1.0F, 1.0F,  -1.0F,

    -1.0F, -1.0F, 1.0F,  -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  -1.0F,
    -1.0F, 1.0F,  -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, -1.0F, 1.0F,

    1.0F,  -1.0F, -1.0F, 1.0F,  -1.0F, 1.0F,  1.0F,  1.0F,  1.0F,
    1.0F,  1.0F,  1.0F,  1.0F,  1.0F,  -1.0F, 1.0F,  -1.0F, -1.0F,

    -1.0F, -1.0F, 1.0F,  -1.0F, 1.0F,  1.0F,  1.0F,  1.0F,  1.0F,
    1.0F,  1.0F,  1.0F,  1.0F,  -1.0F, 1.0F,  -1.0F, -1.0F, 1.0F,

    -1.0F, 1.0F,  -1.0F, 1.0F,  1.0F,  -1.0F, 1.0F,  1.0F,  1.0F,
    1.0F,  1.0F,  1.0F,  -1.0F, 1.0F,  1.0F,  -1.0F, 1.0F,  -1.0F,

    -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, -1.0F,
    1.0F,  -1.0F, -1.0F, -1.0F, -1.0F, 1.0F,  1.0F,  -1.0F, 1.0F,
};
static_assert(sizeof(kSkyboxCubeVertices) / (3U * sizeof(float)) ==
                  static_cast<std::size_t>(kSkyboxVertexCount),
              "kSkyboxVertexCount must match the cube vertex array");

/// Handles cvar string equals.
bool cvar_string_equals(const char *lhs, const char *rhs) noexcept {
  return (lhs != nullptr) && (rhs != nullptr) && (std::strcmp(lhs, rhs) == 0);
}

/// Handles preetham sun direction.
math::Vec3 preetham_sun_direction(const SceneLightData &lights) noexcept {
  if (lights.directionalLightCount > 0U) {
    const math::Vec3 sunDir =
        math::normalize(math::negate(lights.directionalLights[0].direction));
    if (math::length_sq(sunDir) > 0.0F) {
      return sunDir;
    }
  }

  return math::normalize(math::Vec3(0.25F, 0.85F, 0.45F));
}

/// Handles prepare procedural sky draw.
void prepare_procedural_sky_draw(const RenderDevice *dev) noexcept {
  dev->enable_depth_test();
  dev->set_depth_func_less_equal();
  dev->set_depth_mask(false);
  dev->disable_face_culling();
}

/// Handles finish procedural sky draw.
void finish_procedural_sky_draw(const RenderDevice *dev) noexcept {
  dev->bind_vertex_array(0U);
  dev->bind_program(0U);
  dev->set_depth_mask(true);
  dev->set_depth_func_less();
  dev->enable_face_culling();
}

} // namespace

/// Destroys or releases the requested object, handle, or resource for skybox resources.
void destroy_skybox_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.skyboxVertexBuffer != 0U) && (dev != nullptr)) {
    dev->destroy_buffer(backend.skyboxVertexBuffer);
    backend.skyboxVertexBuffer = 0U;
  }
  if ((backend.skyboxVertexArray != 0U) && (dev != nullptr)) {
    dev->destroy_vertex_array(backend.skyboxVertexArray);
    backend.skyboxVertexArray = 0U;
  }
  if (backend.skyboxShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.skyboxShaderHandle);
    backend.skyboxShaderHandle = ShaderProgramHandle{};
  }
  if (backend.preethamSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.preethamSkyShaderHandle);
    backend.preethamSkyShaderHandle = ShaderProgramHandle{};
  }
  if (backend.hosekSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.hosekSkyShaderHandle);
    backend.hosekSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.skyboxProgram = 0U;
  backend.skyboxAvailable = false;
  backend.preethamSkyProgram = 0U;
  backend.preethamSkyAvailable = false;
  backend.hosekSkyProgram = 0U;
  backend.hosekSkyAvailable = false;
}

/// Destroys or releases the requested object, handle, or resource for preetham sky resources.
void destroy_preetham_sky_resources(BackendState &backend) noexcept {
  if (backend.preethamSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.preethamSkyShaderHandle);
    backend.preethamSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.preethamSkyProgram = 0U;
  backend.preethamSkyAvailable = false;
  backend.preethamSkyViewLoc = -1;
  backend.preethamSkyProjectionLoc = -1;
  backend.preethamSkySunDirectionLoc = -1;
  backend.preethamSkyTurbidityLoc = -1;
}

/// Destroys or releases the requested object, handle, or resource for hosek sky resources.
void destroy_hosek_sky_resources(BackendState &backend) noexcept {
  if (backend.hosekSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.hosekSkyShaderHandle);
    backend.hosekSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.hosekSkyProgram = 0U;
  backend.hosekSkyAvailable = false;
  backend.hosekSkyViewLoc = -1;
  backend.hosekSkyProjectionLoc = -1;
  backend.hosekSkySunDirectionLoc = -1;
  backend.hosekSkyTurbidityLoc = -1;
  backend.hosekSkyGroundAlbedoLoc = -1;
}

/// Creates a new object, handle, or resource for skybox geometry.
bool create_skybox_geometry(BackendState &backend,
                            const RenderDevice *dev) noexcept {
  if ((backend.skyboxVertexArray != 0U) && (backend.skyboxVertexBuffer != 0U)) {
    return true;
  }

  if ((dev == nullptr) || (dev->create_vertex_array == nullptr) ||
      (dev->create_buffer == nullptr) || (dev->bind_vertex_array == nullptr) ||
      (dev->bind_array_buffer == nullptr) ||
      (dev->buffer_data_array == nullptr) ||
      (dev->enable_vertex_attrib == nullptr) ||
      (dev->vertex_attrib_float == nullptr)) {
    return false;
  }

  backend.skyboxVertexArray = dev->create_vertex_array();
  backend.skyboxVertexBuffer = dev->create_buffer();
  if ((backend.skyboxVertexArray == 0U) || (backend.skyboxVertexBuffer == 0U)) {
    destroy_skybox_resources(backend);
    return false;
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->bind_array_buffer(backend.skyboxVertexBuffer);
  dev->buffer_data_array(kSkyboxCubeVertices, sizeof(kSkyboxCubeVertices));
  dev->enable_vertex_attrib(0U);
  dev->vertex_attrib_float(0U, 3, static_cast<std::int32_t>(3 * sizeof(float)),
                           nullptr);
  dev->bind_array_buffer(0U);
  dev->bind_vertex_array(0U);
  return true;
}

/// Handles selected sky model.
SkyModel selected_sky_model() noexcept {
  const char *model = core::cvar_get_string("r_sky_model", "hosek");
  if (cvar_string_equals(model, "cubemap")) {
    return SkyModel::Cubemap;
  }
  if (cvar_string_equals(model, "preetham")) {
    return SkyModel::Preetham;
  }
  if (cvar_string_equals(model, "none")) {
    return SkyModel::None;
  }
  return SkyModel::Hosek;
}

/// Handles active skybox gpu texture.
std::uint32_t active_skybox_gpu_texture(const BackendState &backend) noexcept {
  if (!backend.skyboxAvailable ||
      (renderer_context().activeSkyboxTexture == kInvalidTextureHandle) ||
      !is_texture_cubemap(renderer_context().activeSkyboxTexture)) {
    return 0U;
  }

  return texture_gpu_id(renderer_context().activeSkyboxTexture);
}

/// Handles draw skybox.
void draw_skybox(const BackendState &backend, const RenderDevice *dev,
                 const math::Mat4 &viewMat, const math::Mat4 &projMat,
                 std::uint32_t cubemapGpuId,
                 RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || (cubemapGpuId == 0U) ||
      (dev->bind_texture_cubemap == nullptr) ||
      (dev->set_depth_func_less_equal == nullptr) ||
      (dev->set_depth_func_less == nullptr)) {
    return;
  }

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.skyboxProgram);
  if (backend.skyboxViewLoc >= 0) {
    dev->set_uniform_mat4(backend.skyboxViewLoc, &viewMat.columns[0].x);
  }
  if (backend.skyboxProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.skyboxProjectionLoc, &projMat.columns[0].x);
  }
  if (backend.skyboxTextureLoc >= 0) {
    dev->set_uniform_int(backend.skyboxTextureLoc, 0);
  }

  dev->bind_texture_cubemap(0, cubemapGpuId);
  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->draw_arrays_triangles(0, kSkyboxVertexCount);

  dev->bind_texture_cubemap(0, 0U);
  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

/// Handles draw preetham sky.
void draw_preetham_sky(const BackendState &backend, const RenderDevice *dev,
                       const math::Mat4 &viewMat, const math::Mat4 &projMat,
                       const SceneLightData &lights,
                       RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || !backend.preethamSkyAvailable ||
      (dev->set_depth_func_less_equal == nullptr) ||
      (dev->set_depth_func_less == nullptr)) {
    return;
  }

  const math::Vec3 sunDir = preetham_sun_direction(lights);
  const float turbidity = core::cvar_get_float("r_sky_turbidity", 3.0F);

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.preethamSkyProgram);
  if (backend.preethamSkyViewLoc >= 0) {
    dev->set_uniform_mat4(backend.preethamSkyViewLoc, &viewMat.columns[0].x);
  }
  if (backend.preethamSkyProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.preethamSkyProjectionLoc,
                          &projMat.columns[0].x);
  }
  if (backend.preethamSkySunDirectionLoc >= 0) {
    dev->set_uniform_vec3(backend.preethamSkySunDirectionLoc, &sunDir.x);
  }
  if (backend.preethamSkyTurbidityLoc >= 0) {
    dev->set_uniform_float(backend.preethamSkyTurbidityLoc, turbidity);
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->draw_arrays_triangles(0, kSkyboxVertexCount);

  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

/// Handles draw hosek sky.
void draw_hosek_sky(const BackendState &backend, const RenderDevice *dev,
                    const math::Mat4 &viewMat, const math::Mat4 &projMat,
                    const SceneLightData &lights,
                    RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || !backend.hosekSkyAvailable ||
      (dev->set_depth_func_less_equal == nullptr) ||
      (dev->set_depth_func_less == nullptr)) {
    return;
  }

  const math::Vec3 sunDir = preetham_sun_direction(lights);
  const float turbidity = core::cvar_get_float("r_sky_turbidity", 3.0F);
  const float groundAlbedo = core::cvar_get_float("r_sky_ground_albedo", 0.1F);

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.hosekSkyProgram);
  if (backend.hosekSkyViewLoc >= 0) {
    dev->set_uniform_mat4(backend.hosekSkyViewLoc, &viewMat.columns[0].x);
  }
  if (backend.hosekSkyProjectionLoc >= 0) {
    dev->set_uniform_mat4(backend.hosekSkyProjectionLoc, &projMat.columns[0].x);
  }
  if (backend.hosekSkySunDirectionLoc >= 0) {
    dev->set_uniform_vec3(backend.hosekSkySunDirectionLoc, &sunDir.x);
  }
  if (backend.hosekSkyTurbidityLoc >= 0) {
    dev->set_uniform_float(backend.hosekSkyTurbidityLoc, turbidity);
  }
  if (backend.hosekSkyGroundAlbedoLoc >= 0) {
    dev->set_uniform_float(backend.hosekSkyGroundAlbedoLoc, groundAlbedo);
  }

  dev->bind_vertex_array(backend.skyboxVertexArray);
  dev->draw_arrays_triangles(0, kSkyboxVertexCount);

  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

} // namespace engine::renderer
