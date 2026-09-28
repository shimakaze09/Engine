// Regression for #765 on a real render device: a reflection probe lights
// the views inside its box. A mirror-like sphere sits under the default
// procedural sky with no sky light and no direct light, so without a probe
// nothing gives it image-based light and it reads one flat ambient colour
// top to bottom. A probe placed between the camera and the sphere, its box
// around the camera, captures the sky above and the ground below; the
// sphere's upper half then reflects the blue sky and its lower half the
// grey ground. Checked on the deferred and the forward path. Blue on top
// and grey below also shows the probe's cube is the right way up: a
// capture flipped vertically would put the ground on top.

#include "../gpu_scene_fixture.h"

#include "engine/content/asset_identity.h"
#include "engine/renderer/command_buffer.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// Mean of each channel over a 9x9 block of the frame.
struct BlockColor final {
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;

  /// Mean brightness over the three channels.
  double level() const noexcept { return (red + green + blue) / 3.0; }
  /// How much bluer than red: high for sky, near zero for grey.
  double blueness() const noexcept { return blue - red; }
};

/// The block centred on (cx, cy).
BlockColor block_color(const CapturedFrame &frame, int cx, int cy) noexcept {
  std::array<double, 3> sums{};
  int count = 0;
  for (int y = cy - 4; y <= cy + 4; ++y) {
    for (int x = cx - 4; x <= cx + 4; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        sums[c] += frame.channel(static_cast<std::uint32_t>(x),
                                 static_cast<std::uint32_t>(y), c);
      }
      ++count;
    }
  }
  // The capture keeps the TGA's byte order: blue, green, red.
  BlockColor color{};
  color.blue = sums[0] / static_cast<double>(count);
  color.green = sums[1] / static_cast<double>(count);
  color.red = sums[2] / static_cast<double>(count);
  return color;
}

struct SphereLevels final {
  BlockColor upper{};
  BlockColor lower{};
};

/// The sphere's colour a tenth of the frame above and below its centre,
/// well inside its outline (it spans about six tenths of the frame's
/// height). Rows run top to bottom, so "upper" is the smaller row.
bool sphere_levels(engine::EnginePipeline &pipeline, const char *capture,
                   SphereLevels *out) noexcept {
  CapturedFrame frame{};
  if (!engine::tests::settle_frames(pipeline, 20) ||
      !engine::tests::capture_presented_frame(pipeline, capture, &frame)) {
    return false;
  }
  const int cx = static_cast<int>(frame.width / 2U);
  const int cy = static_cast<int>(frame.height / 2U);
  const int offset = static_cast<int>(frame.height / 10U);
  out->upper = block_color(frame, cx, cy - offset);
  out->lower = block_color(frame, cx, cy + offset);
  return true;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::checked;

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_string("r_sky_model", "hosek"), "r_sky_model");

  engine::runtime::Transform transform{};
  transform.scale = engine::math::Vec3(6.0F, 6.0F, 6.0F);
  const Entity sphere = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId =
      engine::content::make_asset_id_from_path("builtin://sphere");
  mesh.albedo = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  mesh.metallic = 1.0F;
  mesh.roughness = 0.05F;
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.0F;
  if ((sphere == kInvalidEntity) || !world.add_mesh_component(sphere, mesh) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 9.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  SphereLevels without{};
  if (!sphere_levels(pipeline, "reflection_probe_none.tga", &without)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  engine::runtime::Transform probeAt{};
  probeAt.position = engine::math::Vec3(0.0F, 0.0F, 6.0F);
  const Entity probeEntity = world.create_scene_object(probeAt);
  engine::runtime::ReflectionProbeComponent probe{};
  probe.boxExtents = engine::math::Vec3(10.0F, 10.0F, 10.0F);
  probe.radius = 50.0F;
  SphereLevels deferred{};
  SphereLevels forward{};
  if ((probeEntity == kInvalidEntity) ||
      !world.add_reflection_probe_component(probeEntity, probe) ||
      !sphere_levels(pipeline, "reflection_probe_deferred.tga", &deferred) ||
      !engine::core::cvar_set_bool("r_deferred", false) ||
      !sphere_levels(pipeline, "reflection_probe_forward.tga", &forward)) {
    return 11;
  }
  engine::renderer::ReflectionProbeStatus status{};
  if (!engine::renderer::get_reflection_probe_status(0U, &status) ||
      !status.baked ||
      (engine::renderer::active_reflection_probe(
           engine::renderer::RenderViewId::Game) != 0)) {
    std::fprintf(stderr, "FAIL: the probe was not baked or not chosen\n");
    return 12;
  }

  const struct {
    const char *path;
    SphereLevels levels;
  } rows[] = {{"none", without}, {"deferred", deferred}, {"forward", forward}};
  for (const auto &row : rows) {
    std::printf("%s: upper level %.1f blueness %.1f, lower level %.1f "
                "blueness %.1f\n",
                row.path, row.levels.upper.level(), row.levels.upper.blueness(),
                row.levels.lower.level(), row.levels.lower.blueness());
  }

  // Without image-based light the sphere is one constant ambient colour:
  // the two blocks match to a level of rounding.
  if ((std::fabs(without.upper.level() - without.lower.level()) > 1.0) ||
      (std::fabs(without.upper.blueness() - without.lower.blueness()) > 1.0)) {
    std::fprintf(stderr, "FAIL: the unlit sphere is not uniform\n");
    return 13;
  }
  // With the probe, the reflected sky over the reflected ground. Thirty-two
  // levels of blueness is far inside the gap between the procedural sky and
  // its grey ground, and far above any tint a uniformly lit surface shows.
  // The lit sphere must also be at least sixteen levels brighter than the
  // flat ambient it had without a probe.
  for (std::size_t i = 1U; i < 3U; ++i) {
    const auto &row = rows[i];
    if (row.levels.upper.blueness() < row.levels.lower.blueness() + 32.0) {
      std::fprintf(stderr,
                   "FAIL: on the %s path the sphere does not reflect the sky "
                   "over the ground\n",
                   row.path);
      return 14;
    }
    if (row.levels.upper.level() < without.upper.level() + 16.0) {
      std::fprintf(stderr,
                   "FAIL: on the %s path the probe did not light the sphere\n",
                   row.path);
      return 15;
    }
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: assets\n");
    return 1;
  }
  return engine::tests::run_gpu_scene_test("reflection_probe_gpu_test", &run);
}
