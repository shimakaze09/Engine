#include <cstdio>
#include <cstring>

#include "engine/core/debug_draw.h"

using namespace engine::core;

static bool test_init_shutdown() noexcept {
  if (!initialize_debug_draw()) {
    return false;
  }
  shutdown_debug_draw();
  return true;
}

static bool test_draw_line_and_query() noexcept {
  initialize_debug_draw();

  const DebugVec3 a{0.0F, 0.0F, 0.0F};
  const DebugVec3 b{1.0F, 0.0F, 0.0F};
  const DebugColor red{1.0F, 0.0F, 0.0F, 1.0F};

  debug_draw_line(a, b, red, 1U);

  DebugLine lines[8] = {};
  const std::size_t count = debug_draw_get_lines(lines, 8U);
  if (count != 1U) {
    shutdown_debug_draw();
    return false;
  }
  if (lines[0].from.x != 0.0F) {
    shutdown_debug_draw();
    return false;
  }
  if (lines[0].to.x != 1.0F) {
    shutdown_debug_draw();
    return false;
  }
  if (lines[0].lifeFrames != 1U) {
    shutdown_debug_draw();
    return false;
  }

  shutdown_debug_draw();
  return true;
}

static bool test_draw_sphere_and_query() noexcept {
  initialize_debug_draw();

  const DebugVec3 center{2.0F, 3.0F, 4.0F};
  debug_draw_sphere(center, 5.0F);

  DebugSphere spheres[4] = {};
  const std::size_t count = debug_draw_get_spheres(spheres, 4U);
  if (count != 1U) {
    shutdown_debug_draw();
    return false;
  }
  if (spheres[0].center.x != 2.0F) {
    shutdown_debug_draw();
    return false;
  }
  if (spheres[0].radius != 5.0F) {
    shutdown_debug_draw();
    return false;
  }

  shutdown_debug_draw();
  return true;
}

static bool test_draw_text_and_query() noexcept {
  initialize_debug_draw();

  const DebugVec3 pos{0.0F, 1.0F, 0.0F};
  debug_draw_text(pos, "Hello");

  DebugText texts[4] = {};
  const std::size_t count = debug_draw_get_texts(texts, 4U);
  if (count != 1U) {
    shutdown_debug_draw();
    return false;
  }
  if (std::strcmp(texts[0].text, "Hello") != 0) {
    shutdown_debug_draw();
    return false;
  }

  shutdown_debug_draw();
  return true;
}

static bool test_lifetime_expires_after_tick() noexcept {
  initialize_debug_draw();

  // lifeFrames = 1 → expires after one tick
  debug_draw_line({}, {1.0F, 0.0F, 0.0F}, {}, 1U);

  debug_draw_tick();

  DebugLine lines[4] = {};
  const std::size_t count = debug_draw_get_lines(lines, 4U);
  if (count != 0U) {
    shutdown_debug_draw();
    return false;
  }

  shutdown_debug_draw();
  return true;
}

static bool test_lifetime_persists_multiple_frames() noexcept {
  initialize_debug_draw();

  // lifeFrames = 3 → survives 2 ticks, gone after 3rd
  debug_draw_sphere({}, 1.0F, {}, 3U);

  debug_draw_tick();
  debug_draw_tick();

  DebugSphere spheres[4] = {};
  std::size_t count = debug_draw_get_spheres(spheres, 4U);
  if (count != 1U) {
    shutdown_debug_draw();
    return false;
  }

  debug_draw_tick();
  count = debug_draw_get_spheres(spheres, 4U);
  if (count != 0U) {
    shutdown_debug_draw();
    return false;
  }

  shutdown_debug_draw();
  return true;
}

static bool test_null_text_ignored() noexcept {
  initialize_debug_draw();
  debug_draw_text({}, nullptr);
  DebugText texts[4] = {};
  const std::size_t count = debug_draw_get_texts(texts, 4U);
  if (count != 0U) {
    shutdown_debug_draw();
    return false;
  }
  shutdown_debug_draw();
  return true;
}

int main() {
  struct {
    const char *name;
    bool (*fn)() noexcept;
  } tests[] = {
      {"init_shutdown", test_init_shutdown},
      {"draw_line_and_query", test_draw_line_and_query},
      {"draw_sphere_and_query", test_draw_sphere_and_query},
      {"draw_text_and_query", test_draw_text_and_query},
      {"lifetime_expires_after_tick", test_lifetime_expires_after_tick},
      {"lifetime_persists_multiple_frames",
       test_lifetime_persists_multiple_frames},
      {"null_text_ignored", test_null_text_ignored},
  };

  int failures = 0;
  for (auto &t : tests) {
    if (!t.fn()) {
      std::printf("FAIL: %s\n", t.name);
      ++failures;
    } else {
      std::printf("PASS: %s\n", t.name);
    }
  }
  return (failures == 0) ? 0 : 1;
}
