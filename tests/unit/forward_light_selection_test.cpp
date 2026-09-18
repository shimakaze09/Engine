// Verifies the forward path's light selection against a fake render
// device (#565 row 2): the fixed point/spot arrays receive the lights
// nearest the active camera (ties by index), not the first eight by
// index, and the shadow-slot indices the forward shader compares against
// its loop position are remapped to positions in that selection (or -1
// for a light that was not selected).

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shadow_map.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace engine::renderer {

namespace {

constexpr std::int32_t kPointPosRadiusParam = 11;
constexpr std::int32_t kSpotPosRadiusParam = 12;
constexpr std::int32_t kPointCountParam = 13;
constexpr std::int32_t kPointShadowIdxParam = 14;
constexpr std::int32_t kSpotShadowIdxParam = 15;

/// What the fake device records from the light and shadow uploads.
struct Uploads final {
  float pointPosRadius[kForwardMaxPointLights * 4U] = {};
  std::int32_t pointCount = -1;
  float spotPosRadius[kForwardMaxSpotLights * 4U] = {};
  std::int32_t spotCount = -1;
  float pointShadowIdx[4] = {-2.0F, -2.0F, -2.0F, -2.0F};
  float spotShadowIdx[4] = {-2.0F, -2.0F, -2.0F, -2.0F};
};

Uploads g_uploads{};
RenderDevice g_device{};

void fake_set_param_vec4_array(ShaderParam param, const float *values,
                               std::int32_t count) noexcept {
  if (param.value == kPointPosRadiusParam) {
    g_uploads.pointCount = count;
    for (std::int32_t i = 0; i < count * 4; ++i) {
      g_uploads.pointPosRadius[i] = values[i];
    }
  } else if (param.value == kSpotPosRadiusParam) {
    g_uploads.spotCount = count;
    for (std::int32_t i = 0; i < count * 4; ++i) {
      g_uploads.spotPosRadius[i] = values[i];
    }
  }
}
void fake_set_param_vec4(ShaderParam param, const float *values) noexcept {
  if (param.value == kPointShadowIdxParam) {
    for (int i = 0; i < 4; ++i) {
      g_uploads.pointShadowIdx[i] = values[i];
    }
  } else if (param.value == kSpotShadowIdxParam) {
    for (int i = 0; i < 4; ++i) {
      g_uploads.spotShadowIdx[i] = values[i];
    }
  }
}
void fake_set_param_i32(ShaderParam, std::int32_t) noexcept {}
void fake_set_param_f32(ShaderParam, float) noexcept {}
void fake_set_param_vec3(ShaderParam, const float *) noexcept {}
void fake_set_param_mat4(ShaderParam, const float *) noexcept {}
void fake_set_param_mat4_array(ShaderParam, const float *,
                               std::int32_t) noexcept {}
void fake_bind_texture_slot(std::uint32_t, DeviceTextureHandle) noexcept {}

void reset_fake_device() noexcept {
  g_uploads = Uploads{};
  g_device = RenderDevice{};
  g_device.set_param_vec4_array = &fake_set_param_vec4_array;
  g_device.set_param_vec4 = &fake_set_param_vec4;
  g_device.set_param_i32 = &fake_set_param_i32;
  g_device.set_param_f32 = &fake_set_param_f32;
  g_device.set_param_vec3 = &fake_set_param_vec3;
  g_device.set_param_mat4 = &fake_set_param_mat4;
  g_device.set_param_mat4_array = &fake_set_param_mat4_array;
  g_device.bind_texture_slot = &fake_bind_texture_slot;
}

} // namespace

// Link seams: the device the context helpers resolve, and material
// textures, which these uploads never touch.
const RenderDevice *render_device() noexcept { return &g_device; }
DeviceTextureHandle texture_device_handle(TextureHandle) noexcept {
  return kInvalidDeviceTexture;
}

} // namespace engine::renderer

namespace {

using namespace engine::renderer;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

BackendState g_backend{};

void reset_backend() noexcept {
  g_backend = BackendState{};
  g_backend.pbrPointLightPosRadiusParam = ShaderParam{kPointPosRadiusParam};
  g_backend.pbrSpotLightPosRadiusParam = ShaderParam{kSpotPosRadiusParam};
  g_backend.pbrPointLightCountLocation = ShaderParam{kPointCountParam};
  g_backend.pbrPointShadowLightIdxParam = ShaderParam{kPointShadowIdxParam};
  g_backend.pbrSpotShadowLightIdxParam = ShaderParam{kSpotShadowIdxParam};
  renderer_context().activeCamera.position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
}

/// Twelve point lights: the four lowest indices sit 100 m away, the
/// eight highest 1..8 m away in reverse index order.
SceneLightData make_lights() noexcept {
  SceneLightData lights{};
  lights.pointLightCount = 12U;
  for (std::size_t i = 0U; i < 4U; ++i) {
    lights.pointLights[i].position = engine::math::Vec3(100.0F + static_cast<float>(i), 0.0F, 0.0F);
    lights.pointLights[i].radius = static_cast<float>(i);
  }
  for (std::size_t i = 4U; i < 12U; ++i) {
    // index 4 -> 8 m, index 11 -> 1 m
    lights.pointLights[i].position =
        engine::math::Vec3(0.0F, static_cast<float>(12U - i), 0.0F);
    lights.pointLights[i].radius = static_cast<float>(i);
  }
  lights.spotLightCount = 10U;
  for (std::size_t i = 0U; i < 10U; ++i) {
    // index 0 farthest (10 m) ... index 9 nearest (1 m)
    lights.spotLights[i].position =
        engine::math::Vec3(0.0F, 0.0F, static_cast<float>(10U - i));
    lights.spotLights[i].radius = static_cast<float>(i);
  }
  return lights;
}

/// EXPECTATION: the eight nearest point lights are uploaded, nearest
/// first, identified through the radius each light carries as its index.
void test_nearest_point_lights_uploaded() noexcept {
  reset_backend();
  reset_fake_device();
  const SceneLightData lights = make_lights();
  upload_pbr_lighting_uniforms(g_backend, &g_device, lights);
  CHECK(g_uploads.pointCount == 8, "eight point lights uploaded");
  bool nearestFirst = true;
  for (std::size_t i = 0U; i < 8U; ++i) {
    // Nearest is index 11 (1 m), then 10, ... down to index 4 (8 m).
    const float expectedIndex = static_cast<float>(11U - i);
    if (g_uploads.pointPosRadius[i * 4U + 3U] != expectedIndex) {
      nearestFirst = false;
    }
  }
  CHECK(nearestFirst, "uploaded point lights are the nearest, nearest first");
  CHECK(g_uploads.spotCount == 8, "eight spot lights uploaded");
  CHECK(g_uploads.spotPosRadius[3] == 9.0F, "nearest spot light first");
  CHECK(g_uploads.spotPosRadius[7U * 4U + 3U] == 2.0F,
        "eighth-nearest spot light last; the two farthest are left out");
}

/// EXPECTATION: a shadow slot's scene light index is uploaded as the
/// light's position within the selection, or -1 when unselected.
void test_shadow_slots_remap_to_upload_positions() noexcept {
  reset_backend();
  reset_fake_device();
  g_backend.pointShadowState.slots[0].lightIndex = 10; // 2 m: position 1
  g_backend.pointShadowState.slots[1].lightIndex = 0;  // 100 m: unselected
  g_backend.spotShadowState.slots[0].lightIndex = 9;   // nearest: position 0
  g_backend.spotShadowState.slots[1].lightIndex = 1;   // 9 m: unselected
  const SceneLightData lights = make_lights();
  bind_pbr_shadow_uniforms(g_backend, &g_device, lights, false, true, true);
  CHECK(g_uploads.pointShadowIdx[0] == 1.0F,
        "point slot maps to its upload position");
  CHECK(g_uploads.pointShadowIdx[1] == -1.0F,
        "point slot whose light is not uploaded reads -1");
  CHECK(g_uploads.pointShadowIdx[2] == -1.0F, "unused point slot reads -1");
  CHECK(g_uploads.spotShadowIdx[0] == 0.0F,
        "spot slot maps to its upload position");
  CHECK(g_uploads.spotShadowIdx[1] == -1.0F,
        "spot slot whose light is not uploaded reads -1");
}

/// EXPECTATION: equal distances break ties by index, so the lit set does
/// not depend on creation order.
void test_ties_break_by_index() noexcept {
  reset_backend();
  reset_fake_device();
  SceneLightData lights{};
  lights.pointLightCount = 10U;
  for (std::size_t i = 0U; i < 10U; ++i) {
    lights.pointLights[i].position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
    lights.pointLights[i].radius = static_cast<float>(i);
  }
  upload_pbr_lighting_uniforms(g_backend, &g_device, lights);
  bool ascending = true;
  for (std::size_t i = 0U; i < 8U; ++i) {
    if (g_uploads.pointPosRadius[i * 4U + 3U] != static_cast<float>(i)) {
      ascending = false;
    }
  }
  CHECK(ascending, "equidistant lights upload in index order");
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_nearest_point_lights_uploaded();
  test_shadow_slots_remap_to_upload_positions();
  test_ties_break_by_index();
  if (g_failures != 0) {
    std::fprintf(stderr, "forward_light_selection_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::puts("forward_light_selection_test passed");
  return 0;
}
