// GPU regression for issue #1212: on Direct3D 12 a sunlit face in a far
// directional cascade read as shadowed while the cascades were cached, and
// lit as soon as they were rendered every frame. The mip chain suite caught
// it in one run of several; this suite renders the cached cascades afresh
// several times and reads them each time, so a fault in the cached maps
// shows here rather than once in a while there.
//
// Two cubes of one material face a sun shining down the view, one 4 m
// away and one 40 m away, so their front faces lie in the first and the
// third cascade and light alike. Each cycle brightens the sun a little,
// which is part of the cascade cache key, so the cascades render again and
// are cached without any shadow moving; it reads the far face one
// frame after that render and twelve frames after it, then with the
// cascades rendered every frame. All three must match, and match the near
// face. The scene is built after the fixture's first frame, as the mip
// chain suite builds its own, so the first cycle is the cascades' first
// render of it.

#include "../gpu_scene_fixture.h"

#include <cmath>
#include <cstdio>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr int kCycles = 6;

/// Mean brightness of the (2r+1)^2 block centred on (cx, cy).
double block_level(const CapturedFrame &frame, int cx, int cy, int r) noexcept {
  double sum = 0.0;
  int count = 0;
  for (int y = cy - r; y <= cy + r; ++y) {
    for (int x = cx - r; x <= cx + r; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        sum += frame.channel(static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y), c);
        ++count;
      }
    }
  }
  return sum / static_cast<double>(count);
}

/// A cube with a fully rough grey material: no highlight, so the two faces
/// differ by nothing but what reaches them.
Entity add_cube(World &world, const engine::math::Vec3 &position) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const Entity cube = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = engine::content::make_asset_id_from_path("builtin://cube");
  mesh.albedo = engine::math::Vec3(0.8F, 0.8F, 0.8F);
  mesh.roughness = 1.0F;
  return ((cube != kInvalidEntity) && world.add_mesh_component(cube, mesh))
             ? cube
             : kInvalidEntity;
}

/// The sun of cycle `cycle`: straight down the view, a little brighter
/// each cycle. Only its intensity changes, so the cascades render again
/// while every shadow stays where it was and the two front faces stay lit
/// alike.
engine::runtime::LightComponent sun_of_cycle(int cycle) noexcept {
  engine::runtime::LightComponent sun{};
  sun.direction = engine::math::Vec3(0.0F, 0.0F, -1.0F);
  sun.intensity = 2.0F + (0.05F * static_cast<float>(cycle));
  return sun;
}

struct Readings final {
  double near = 0.0;
  double farEarly = 0.0;
  double farLate = 0.0;
  double farRerendered = 0.0;
};

/// Captures `path` and reads both faces from it. False when the device
/// returned no frame.
bool read_faces(engine::EnginePipeline &pipeline, const char *path,
                double *nearOut, double *farOut) noexcept {
  CapturedFrame frame{};
  if (!engine::tests::capture_presented_frame(pipeline, path, &frame)) {
    return false;
  }
  // The near face spans x from about 0.27 to 0.40 of the frame; the far
  // one is a small square at the centre, sampled well inside its edge.
  const int cy = static_cast<int>(frame.height / 2U);
  *nearOut =
      block_level(frame, static_cast<int>((frame.width * 1U) / 3U), cy, 6);
  *farOut = block_level(frame, static_cast<int>(frame.width / 2U), cy, 3);
  return true;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::checked;
  using engine::tests::settle_frames;

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");

  const Entity sun = world.create_scene_object();
  if ((add_cube(world, engine::math::Vec3(-1.2F, 0.0F, -4.0F)) ==
       kInvalidEntity) ||
      (add_cube(world, engine::math::Vec3(0.0F, 0.0F, -40.0F)) ==
       kInvalidEntity) ||
      (sun == kInvalidEntity) ||
      !world.add_light_component(sun, sun_of_cycle(0)) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }

  int result = 0;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    if ((cycle > 0) && (!world.remove_light_component(sun) ||
                        !world.add_light_component(sun, sun_of_cycle(cycle)))) {
      return 11;
    }
    // One frame renders the cascades for this sun and caches them; the
    // capture shows the frame after it, which reuses them.
    Readings readings{};
    double ignored = 0.0;
    if (!settle_frames(pipeline, 1) ||
        !read_faces(pipeline, "shadow_cache_far_early.tga", &ignored,
                    &readings.farEarly)) {
      if (cycle == 0) {
        std::printf("SKIPPED: the device returned no back-buffer readback\n");
        return 0;
      }
      return 12;
    }
    if (!settle_frames(pipeline, 12) ||
        !read_faces(pipeline, "shadow_cache_far_late.tga", &ignored,
                    &readings.farLate)) {
      return 13;
    }
    checked(engine::core::cvar_set_bool("r_shadow_cache", false),
            "r_shadow_cache");
    const bool rerendered =
        settle_frames(pipeline, 4) &&
        read_faces(pipeline, "shadow_cache_far_rerendered.tga", &readings.near,
                   &readings.farRerendered);
    checked(engine::core::cvar_set_bool("r_shadow_cache", true),
            "r_shadow_cache");
    if (!rerendered) {
      return 14;
    }
    std::printf("cycle %d: near %.1f; far %.1f a frame after caching, %.1f "
                "twelve frames on, %.1f rendered every frame\n",
                cycle, readings.near, readings.farEarly, readings.farLate,
                readings.farRerendered);

    // Same material, facing and sun: the faces match to within rounding,
    // two levels of the 8-bit readback, and so does every reading of the
    // far face. A shadowed face reads its ambient level, some 38 against
    // 217 lit in the mip chain suite.
    if (std::fabs(readings.farRerendered - readings.near) > 2.0) {
      std::fprintf(stderr,
                   "FAIL: cycle %d: rendered every frame, the far face "
                   "reads %.1f against the near face's %.1f\n",
                   cycle, readings.farRerendered, readings.near);
      result = 20;
    }
    if (std::fabs(readings.farEarly - readings.farRerendered) > 2.0) {
      std::fprintf(stderr,
                   "FAIL: cycle %d: a frame after caching, the far face "
                   "reads %.1f against %.1f rendered every frame\n",
                   cycle, readings.farEarly, readings.farRerendered);
      result = 21;
    }
    if (std::fabs(readings.farLate - readings.farRerendered) > 2.0) {
      std::fprintf(stderr,
                   "FAIL: cycle %d: twelve frames after caching, the far "
                   "face reads %.1f against %.1f rendered every frame\n",
                   cycle, readings.farLate, readings.farRerendered);
      result = 22;
    }
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("shadow_cache_far_cascade_gpu_test",
                                           &run);
}
