// Regression for issue #393's production Lua ingress: NaN/Infinity and
// out-of-range camera and shake parameters supplied by a script are
// refused at the CameraManager boundary — the Lua calls return false, no
// entry or shake activates, a valid push still works afterward, and the
// evaluated camera stays finite.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/math/vec3.h"
#include "engine/runtime/camera_manager.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScriptPath = "camera_ingress_lua_test.lua";

/// Writes the scenario script; false on any short write.
bool write_script_file(const char *contents) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kTempScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kTempScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

/// True when some alive entity carries exactly this name (the script
/// reports its verdict by naming a marker entity).
[[nodiscard]] bool marker_exists(engine::runtime::World &world,
                                 const char *name) noexcept {
  bool found = false;
  world.for_each_alive([&](engine::runtime::Entity entity) noexcept {
    engine::runtime::NameComponent nc{};
    if (world.get_name_component(entity, &nc) &&
        (std::strcmp(nc.name, name) == 0)) {
      found = true;
    }
  });
  return found;
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(std::remove(kTempScriptPath));
  static_cast<void>(engine::core::initialize_logging());

  // Each refused call must return false to the script; the one valid push
  // must succeed. The script folds every observation into one verdict.
  const char *script =
      "function on_start()\n"
      "    e = engine.spawn_entity()\n"
      "    local nan = 0.0 / 0.0\n"
      "    local inf = 1.0 / 0.0\n"
      "    local ok = true\n"
      "    if engine.push_camera(e, nan, 0, 0, 1, 0, 0, 1.0) ~= false then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.push_camera(e, 0, 0, 5, inf, 0, 0, 1.0) ~= false then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.push_camera(e, 0, 0, 5, 0, 0, 0, nan) ~= false then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.push_camera(e, 0, 0, 5, 0, 0, 0, 1.0, -2.0) ~= false\n"
      "    then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.camera_shake(nan, 15, 1) ~= false then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.camera_shake(0.1, 15, 0) ~= false then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.camera_shake(-0.1, 15, 1) ~= false then\n"
      "        ok = false\n"
      "    end\n"
      "    if engine.push_camera(e, 0, 0, 5, 0, 0, 0, 1.0) ~= true then\n"
      "        ok = false\n"
      "    end\n"
      "    local m = engine.spawn_entity()\n"
      "    engine.set_name(m, ok and 'ingress_ok' or 'ingress_bad')\n"
      "end\n";
  if (!write_script_file(script)) {
    return 1;
  }

  if (!engine::scripting::initialize_scripting()) {
    return 2;
  }
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    engine::scripting::shutdown_scripting();
    return 3;
  }
  world->end_frame_phase();
  engine::core::ServiceLocator locator{};
  engine::runtime::bind_scripting_runtime(world.get(), locator);

  int failure = 0;
  if (!engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("on_start")) {
    failure = 4;
  }

  if ((failure == 0) && !marker_exists(*world, "ingress_ok")) {
    failure = 5;
  }

  // Exactly the one valid camera survived the refusals, and evaluation
  // stays finite.
  if ((failure == 0) &&
      (world->camera_manager().camera_count() != 1U)) {
    failure = 6;
  }
  if (failure == 0) {
    engine::math::Vec3 position{};
    engine::math::Vec3 target{};
    engine::math::Vec3 up{};
    float fov = 0.0F;
    float nearPlane = 0.0F;
    float farPlane = 0.0F;
    world->camera_manager().evaluate(10.0F, &position, &target, &up, &fov,
                                     &nearPlane, &farPlane);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(target.x) ||
        !std::isfinite(fov) || !std::isfinite(nearPlane) ||
        !std::isfinite(farPlane)) {
      failure = 7;
    }
  }

  engine::runtime::unbind_scripting_runtime(locator);
  engine::scripting::shutdown_scripting();
  static_cast<void>(std::remove(kTempScriptPath));
  if (failure != 0) {
    std::fprintf(stderr, "camera ingress test failed: %d\n", failure);
    return failure;
  }
  std::printf("camera ingress lua tests passed\n");
  return 0;
}
