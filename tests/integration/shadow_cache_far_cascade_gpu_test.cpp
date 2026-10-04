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
// are cached without any shadow moving; it reads both faces on the frame
// that renders them, one frame after that render and twelve frames after
// it, then with the cascades rendered every frame. All four must match,
// and the two faces must match each other.
//
// Every failure seen came from the cascades' first render of the scene,
// and that render fell on the frame after the fixture's first: the
// process's second frame, and the one on which the device's swapchain
// reset (r_vsync leaving its boot value of 1 for 0) takes effect. The
// argument chooses the frame the first render lands on, each its own
// ctest entry since each concerns the process's own early frames:
//   after-boot       the frame after the fixture's first, as above
//                    (default);
//   after-boot-unchanged
//                    the same frame with nothing changing on it: the
//                    fixture issues no reset, so r_vsync keeps its boot
//                    value, and the cvars the suite sets are given as boot
//                    values (its ctest entry does);
//   third-frame      the process's third frame, one after after-boot's;
//   settled          a frame well after those, with nothing else changing;
//   after-vsync-on   a settled frame on which a reset turning vsync on
//                    takes effect;
//   after-vsync-off  a settled frame on which a reset turning vsync off
//                    takes effect, as the fixture's does.
#include "../gpu_scene_fixture.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr int kCycles = 6;

/// Where the cascades' first render of the scene lands; see the file
/// comment.
enum class FirstRender {
  AfterBoot,
  ThirdFrame,
  Settled,
  AfterVsyncOn,
  AfterVsyncOff
};

FirstRender g_firstRender = FirstRender::AfterBoot;

/// The argument naming g_firstRender; it prefixes the capture files, since
/// the entries run side by side in one working directory.
const char *g_firstRenderName = "after-boot";

/// Whether the fixture's swapchain reset takes effect on the second frame.
bool g_resetOnSecondFrame = true;

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

/// One capture's reading of the two front faces.
struct Faces final {
  double near = 0.0;
  double far = 0.0;
};

/// The four captures of a cycle.
struct Readings final {
  Faces rendered;   ///< The frame that rendered the cascades.
  Faces early;      ///< A frame after caching.
  Faces late;       ///< Twelve frames after caching.
  Faces rerendered; ///< The cascades rendered every frame.
};

/// Captures the next frame and reads both faces from it; `what` names the
/// capture file. False when the device returned no frame.
bool read_faces(engine::EnginePipeline &pipeline, const char *what,
                Faces *out) noexcept {
  char path[128] = {};
  std::snprintf(path, sizeof(path), "shadow_cache_far_%s_%s.tga",
                g_firstRenderName, what);
  CapturedFrame frame{};
  if (!engine::tests::capture_presented_frame(pipeline, path, &frame)) {
    return false;
  }
  // The near face spans x from about 0.27 to 0.40 of the frame; the far
  // one is a small square at the centre, sampled well inside its edge.
  const int cy = static_cast<int>(frame.height / 2U);
  out->near =
      block_level(frame, static_cast<int>((frame.width * 1U) / 3U), cy, 6);
  out->far = block_level(frame, static_cast<int>(frame.width / 2U), cy, 3);
  return true;
}

/// Runs frames until the next one is where the scene's first cascade
/// render lands. False when a frame failed.
bool reach_first_render_frame(engine::EnginePipeline &pipeline) noexcept {
  using engine::tests::checked;
  using engine::tests::settle_frames;
  // The device resets its swapchain at the end of the frame on which
  // r_vsync changed, so the frame after it is the first to run on the
  // reset swapchain.
  switch (g_firstRender) {
  case FirstRender::AfterBoot:
    return true;
  case FirstRender::ThirdFrame:
    return settle_frames(pipeline, 1);
  case FirstRender::Settled:
    return settle_frames(pipeline, 8);
  case FirstRender::AfterVsyncOn:
    if (!settle_frames(pipeline, 8)) {
      return false;
    }
    checked(engine::core::cvar_set_int("r_vsync", 1), "r_vsync");
    return settle_frames(pipeline, 1);
  case FirstRender::AfterVsyncOff:
    checked(engine::core::cvar_set_int("r_vsync", 1), "r_vsync");
    if (!settle_frames(pipeline, 8)) {
      return false;
    }
    checked(engine::core::cvar_set_int("r_vsync", 0), "r_vsync");
    return settle_frames(pipeline, 1);
  }
  return false;
}

/// Fails the cycle when `reading` is not `reference` to within two levels
/// of the 8-bit readback.
bool matches(int cycle, const char *face, const char *when, double reading,
             double reference) noexcept {
  if (std::fabs(reading - reference) <= 2.0) {
    return true;
  }
  std::fprintf(stderr,
               "FAIL: cycle %d: %s, the %s face reads %.1f against %.1f "
               "rendered every frame\n",
               cycle, when, face, reading, reference);
  return false;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::checked;
  using engine::tests::settle_frames;

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");
  // The camera is in place before the frames that lead up to the first
  // render, so the view draws on each of them: a view with no camera draws
  // nothing, and the frame a camera first appears on is drawn without it.
  // With nothing to cast, those frames render no cascades.
  if (!engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }
  if (!reach_first_render_frame(pipeline)) {
    return 15;
  }

  const Entity sun = world.create_scene_object();
  if ((add_cube(world, engine::math::Vec3(-1.2F, 0.0F, -4.0F)) ==
       kInvalidEntity) ||
      (add_cube(world, engine::math::Vec3(0.0F, 0.0F, -40.0F)) ==
       kInvalidEntity) ||
      (sun == kInvalidEntity) ||
      !world.add_light_component(sun, sun_of_cycle(0))) {
    return 10;
  }

  int result = 0;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    if ((cycle > 0) && (!world.remove_light_component(sun) ||
                        !world.add_light_component(sun, sun_of_cycle(cycle)))) {
      return 11;
    }
    // The first capture rides on the frame that renders the cascades for
    // this sun and caches them; the next shows a frame that reuses them.
    Readings readings{};
    if (!read_faces(pipeline, "rendered", &readings.rendered)) {
      if (cycle == 0) {
        std::printf("SKIPPED: the device returned no back-buffer readback\n");
        return 0;
      }
      return 12;
    }
    if (!read_faces(pipeline, "early", &readings.early) ||
        !settle_frames(pipeline, 12) ||
        !read_faces(pipeline, "late", &readings.late)) {
      return 13;
    }
    checked(engine::core::cvar_set_bool("r_shadow_cache", false),
            "r_shadow_cache");
    const bool rerendered =
        settle_frames(pipeline, 4) &&
        read_faces(pipeline, "rerendered", &readings.rerendered);
    checked(engine::core::cvar_set_bool("r_shadow_cache", true),
            "r_shadow_cache");
    if (!rerendered) {
      return 14;
    }
    std::printf("cycle %d: near/far %.1f/%.1f on the frame that rendered, "
                "%.1f/%.1f a frame after caching, %.1f/%.1f twelve frames "
                "on, %.1f/%.1f rendered every frame\n",
                cycle, readings.rendered.near, readings.rendered.far,
                readings.early.near, readings.early.far, readings.late.near,
                readings.late.far, readings.rerendered.near,
                readings.rerendered.far);

    // Same material, facing and sun: the faces match to within rounding,
    // and so does every reading of each face. A shadowed face reads its
    // ambient level, some 38 against 217 lit in the mip chain suite.
    const Faces &reference = readings.rerendered;
    bool ok = true;
    if (std::fabs(reference.far - reference.near) > 2.0) {
      std::fprintf(stderr,
                   "FAIL: cycle %d: rendered every frame, the far face "
                   "reads %.1f against the near face's %.1f\n",
                   cycle, reference.far, reference.near);
      ok = false;
    }
    const struct {
      const char *when;
      const Faces &faces;
    } captures[] = {{"on the frame that rendered", readings.rendered},
                    {"a frame after caching", readings.early},
                    {"twelve frames after caching", readings.late}};
    for (const auto &capture : captures) {
      ok = matches(cycle, "near", capture.when, capture.faces.near,
                   reference.near) &&
           ok;
      ok = matches(cycle, "far", capture.when, capture.faces.far,
                   reference.far) &&
           ok;
    }
    if (!ok) {
      result = 20;
    }
  }
  return result;
}

} // namespace

/// Runs the suite with its first render where argv[1] puts it.
int main(int argc, char **argv) {
  if (argc > 1) {
    static constexpr struct {
      const char *name;
      FirstRender frame;
      bool resetOnSecondFrame = true;
    } kFrames[] = {{"after-boot", FirstRender::AfterBoot},
                   {"after-boot-unchanged", FirstRender::AfterBoot, false},
                   {"third-frame", FirstRender::ThirdFrame},
                   {"settled", FirstRender::Settled},
                   {"after-vsync-on", FirstRender::AfterVsyncOn},
                   {"after-vsync-off", FirstRender::AfterVsyncOff}};
    bool known = false;
    for (const auto &frame : kFrames) {
      if (std::strcmp(argv[1], frame.name) == 0) {
        g_firstRender = frame.frame;
        g_firstRenderName = frame.name;
        g_resetOnSecondFrame = frame.resetOnSecondFrame;
        known = true;
      }
    }
    if (!known) {
      std::fprintf(stderr, "FAIL: unknown first-render frame '%s'\n", argv[1]);
      return 8;
    }
  }
  return engine::tests::run_gpu_scene_test("shadow_cache_far_cascade_gpu_test",
                                           &run, nullptr, g_resetOnSecondFrame);
}
