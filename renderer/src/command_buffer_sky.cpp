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

bool cvar_string_equals(const char *lhs, const char *rhs) noexcept {
  return (lhs != nullptr) && (rhs != nullptr) && (std::strcmp(lhs, rhs) == 0);
}

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

// Sky renders at maximum depth, so it needs the less-equal test, must not
// write depth, and draws the cube's inside faces (no culling). Blending
// stays off: both call sites run inside the opaque section of the frame.
void prepare_procedural_sky_draw(const RenderDevice *dev) noexcept {
  dev->apply_render_state(RenderState{DepthTest::LessEqual, false,
                                      BlendMode::Disabled, CullMode::None});
}

void finish_procedural_sky_draw(const RenderDevice *dev) noexcept {
  dev->bind_program(kInvalidDeviceProgram);
  dev->apply_render_state(RenderState{DepthTest::Less, true,
                                      BlendMode::Disabled, CullMode::Back});
}

} // namespace

/// Destroys or releases the requested object, handle, or resource for skybox resources.
void destroy_skybox_resources(BackendState &backend) noexcept {
  const RenderDevice *dev = render_device();
  if ((backend.skyboxGeometry != kInvalidDeviceGeometry) && (dev != nullptr) &&
      (dev->destroy_geometry != nullptr)) {
    dev->destroy_geometry(backend.skyboxGeometry);
  }
  backend.skyboxGeometry = kInvalidDeviceGeometry;
  if ((backend.skyboxVertexBuffer != kInvalidDeviceBuffer) &&
      (dev != nullptr) && (dev->destroy_buffer != nullptr)) {
    dev->destroy_buffer(backend.skyboxVertexBuffer);
  }
  backend.skyboxVertexBuffer = kInvalidDeviceBuffer;
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
  backend.skyboxProgram = kInvalidDeviceProgram;
  backend.skyboxAvailable = false;
  backend.preethamSkyProgram = kInvalidDeviceProgram;
  backend.preethamSkyAvailable = false;
  backend.hosekSkyProgram = kInvalidDeviceProgram;
  backend.hosekSkyAvailable = false;
}

/// Destroys or releases the requested object, handle, or resource for preetham sky resources.
void destroy_preetham_sky_resources(BackendState &backend) noexcept {
  if (backend.preethamSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.preethamSkyShaderHandle);
    backend.preethamSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.preethamSkyProgram = kInvalidDeviceProgram;
  backend.preethamSkyAvailable = false;
  backend.preethamSkyViewLoc = kInvalidShaderParam;
  backend.preethamSkyProjectionLoc = kInvalidShaderParam;
  backend.preethamSkySunDirectionLoc = kInvalidShaderParam;
  backend.preethamSkyTurbidityLoc = kInvalidShaderParam;
}

/// Destroys or releases the requested object, handle, or resource for hosek sky resources.
void destroy_hosek_sky_resources(BackendState &backend) noexcept {
  if (backend.hosekSkyShaderHandle != kInvalidShaderProgram) {
    destroy_shader_program(backend.hosekSkyShaderHandle);
    backend.hosekSkyShaderHandle = ShaderProgramHandle{};
  }
  backend.hosekSkyProgram = kInvalidDeviceProgram;
  backend.hosekSkyAvailable = false;
  backend.hosekSkyViewLoc = kInvalidShaderParam;
  backend.hosekSkyProjectionLoc = kInvalidShaderParam;
  backend.hosekSkySunDirectionLoc = kInvalidShaderParam;
  backend.hosekSkyTurbidityLoc = kInvalidShaderParam;
  backend.hosekSkyGroundAlbedoLoc = kInvalidShaderParam;
}

/// Creates a new object, handle, or resource for skybox geometry.
bool create_skybox_geometry(BackendState &backend,
                            const RenderDevice *dev) noexcept {
  if ((backend.skyboxGeometry != kInvalidDeviceGeometry) &&
      (backend.skyboxVertexBuffer != kInvalidDeviceBuffer)) {
    return true;
  }

  if ((dev == nullptr) || (dev->create_buffer == nullptr) ||
      (dev->create_geometry == nullptr)) {
    return false;
  }

  BufferDesc bufferDesc{};
  bufferDesc.usage = BufferUsage::Vertex;
  bufferDesc.access = BufferAccess::Static;
  bufferDesc.sizeBytes = sizeof(kSkyboxCubeVertices);
  bufferDesc.data = kSkyboxCubeVertices;
  backend.skyboxVertexBuffer = dev->create_buffer(bufferDesc);
  if (backend.skyboxVertexBuffer == kInvalidDeviceBuffer) {
    destroy_skybox_resources(backend);
    return false;
  }

  GeometryDesc geometryDesc{};
  geometryDesc.vertexBuffer = backend.skyboxVertexBuffer;
  geometryDesc.layout.strideBytes =
      static_cast<std::int32_t>(3 * sizeof(float));
  geometryDesc.layout.attributeCount = 1U;
  geometryDesc.layout.attributes[0] = {VertexSemantic::Position, 3, 0};
  backend.skyboxGeometry = dev->create_geometry(geometryDesc);
  if (backend.skyboxGeometry == kInvalidDeviceGeometry) {
    destroy_skybox_resources(backend);
    return false;
  }
  return true;
}

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

DeviceTextureHandle
active_skybox_device_texture(const BackendState &backend) noexcept {
  if (!backend.skyboxAvailable ||
      (renderer_context().activeSkyboxTexture == kInvalidTextureHandle) ||
      !is_texture_cubemap(renderer_context().activeSkyboxTexture)) {
    return kInvalidDeviceTexture;
  }

  return texture_device_handle(renderer_context().activeSkyboxTexture);
}

void draw_skybox(const BackendState &backend, const RenderDevice *dev,
                 const math::Mat4 &viewMat, const math::Mat4 &projMat,
                 DeviceTextureHandle cubemap,
                 RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || (cubemap == kInvalidDeviceTexture) ||
      (dev->bind_texture_slot == nullptr) ||
      (dev->apply_render_state == nullptr) || (dev->draw == nullptr)) {
    return;
  }

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.skyboxProgram);
  if (backend.skyboxViewLoc.valid()) {
    dev->set_param_mat4(backend.skyboxViewLoc, &viewMat.columns[0].x);
  }
  if (backend.skyboxProjectionLoc.valid()) {
    dev->set_param_mat4(backend.skyboxProjectionLoc, &projMat.columns[0].x);
  }
  if (backend.skyboxTextureLoc.valid()) {
    dev->set_param_i32(backend.skyboxTextureLoc, 0);
  }

  dev->bind_texture_slot(0U, cubemap);
  dev->draw(backend.skyboxGeometry, PrimitiveTopology::Triangles, 0,
            kSkyboxVertexCount);

  dev->bind_texture_slot(0U, kInvalidDeviceTexture);
  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

void draw_preetham_sky(const BackendState &backend, const RenderDevice *dev,
                       const math::Mat4 &viewMat, const math::Mat4 &projMat,
                       const SceneLightData &lights,
                       RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || !backend.preethamSkyAvailable ||
      (dev->apply_render_state == nullptr) || (dev->draw == nullptr)) {
    return;
  }

  const math::Vec3 sunDir = preetham_sun_direction(lights);
  const float turbidity = core::cvar_get_float("r_sky_turbidity", 3.0F);

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.preethamSkyProgram);
  if (backend.preethamSkyViewLoc.valid()) {
    dev->set_param_mat4(backend.preethamSkyViewLoc, &viewMat.columns[0].x);
  }
  if (backend.preethamSkyProjectionLoc.valid()) {
    dev->set_param_mat4(backend.preethamSkyProjectionLoc,
                          &projMat.columns[0].x);
  }
  if (backend.preethamSkySunDirectionLoc.valid()) {
    dev->set_param_vec3(backend.preethamSkySunDirectionLoc, &sunDir.x);
  }
  if (backend.preethamSkyTurbidityLoc.valid()) {
    dev->set_param_f32(backend.preethamSkyTurbidityLoc, turbidity);
  }

  dev->draw(backend.skyboxGeometry, PrimitiveTopology::Triangles, 0,
            kSkyboxVertexCount);

  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

void draw_hosek_sky(const BackendState &backend, const RenderDevice *dev,
                    const math::Mat4 &viewMat, const math::Mat4 &projMat,
                    const SceneLightData &lights,
                    RendererFrameStats &frameStats) noexcept {
  if ((dev == nullptr) || !backend.hosekSkyAvailable ||
      (dev->apply_render_state == nullptr) || (dev->draw == nullptr)) {
    return;
  }

  const math::Vec3 sunDir = preetham_sun_direction(lights);
  const float turbidity = core::cvar_get_float("r_sky_turbidity", 3.0F);
  const float groundAlbedo = core::cvar_get_float("r_sky_ground_albedo", 0.1F);

  prepare_procedural_sky_draw(dev);

  dev->bind_program(backend.hosekSkyProgram);
  if (backend.hosekSkyViewLoc.valid()) {
    dev->set_param_mat4(backend.hosekSkyViewLoc, &viewMat.columns[0].x);
  }
  if (backend.hosekSkyProjectionLoc.valid()) {
    dev->set_param_mat4(backend.hosekSkyProjectionLoc, &projMat.columns[0].x);
  }
  if (backend.hosekSkySunDirectionLoc.valid()) {
    dev->set_param_vec3(backend.hosekSkySunDirectionLoc, &sunDir.x);
  }
  if (backend.hosekSkyTurbidityLoc.valid()) {
    dev->set_param_f32(backend.hosekSkyTurbidityLoc, turbidity);
  }
  if (backend.hosekSkyGroundAlbedoLoc.valid()) {
    dev->set_param_f32(backend.hosekSkyGroundAlbedoLoc, groundAlbedo);
  }

  dev->draw(backend.skyboxGeometry, PrimitiveTopology::Triangles, 0,
            kSkyboxVertexCount);

  finish_procedural_sky_draw(dev);

  ++frameStats.drawCalls;
  frameStats.triangleCount +=
      static_cast<std::uint64_t>(kSkyboxVertexCount) / 3ULL;
}

} // namespace engine::renderer
