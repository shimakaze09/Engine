// Verifies strict path ingress for scripting. Length (issue #80): over-long
// script, module, and scene paths are rejected with a diagnostic instead of
// silently truncating into a different (wrong) path — engine.require of a
// path whose 127-char truncation names a real file must not load that file,
// add_script_component refuses over-long paths, load_scene refuses over-long
// paths while preserving a previously queued valid request, and max-length
// paths still work exactly. VFS jail (issue #83): script-supplied paths
// with "..", absolute roots, backslashes, or drive designators are refused
// on every read and write surface (require, add_script_component,
// load_scene/save_scene, save_prefab/instantiate, load_asset_async,
// load_sound/play_music) and no file outside the jail is created or read.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kDriverPath = "path_ingress_driver.lua";
constexpr std::size_t kModulePathMax = 127U;

/// Writes contents to a relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

/// Builds a name of exactly length characters: 'a' padding + given suffix.
std::string padded_name(std::size_t length, const char *suffix) {
  const std::size_t suffixLen = std::strlen(suffix);
  std::string name(length - suffixLen, 'a');
  name += suffix;
  return name;
}

/// True when the file exists (jail-escape write probes must not).
bool file_exists(const char *path) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "rb") != 0) {
    f = nullptr;
  }
#else
  f = std::fopen(path, "rb");
#endif
  if (f == nullptr) {
    return false;
  }
  std::fclose(f);
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;

  const std::string longModule = padded_name(kModulePathMax + 4U, ".lua");
  const std::string wrongFile = longModule.substr(0U, kModulePathMax);
  const std::string maxModule = padded_name(kModulePathMax, ".lua");
  const std::string longScene = padded_name(600U, ".json");
  const std::string maxScene = padded_name(511U, ".json");

  ctx.check(write_file_at(wrongFile.c_str(), "return { wrong = true }\n"),
            "write truncation-collision file");
  ctx.check(write_file_at(longModule.c_str(), "return { long = true }\n"),
            "write over-long module file");
  ctx.check(write_file_at(maxModule.c_str(), "return { ok = true }\n"),
            "write max-length module file");

  std::string driver;
  driver += "function require_long_rejected()\n";
  driver += "    if engine.require('" + longModule + "') ~= nil then\n";
  driver += "        error('over-long module path loaded a file')\n";
  driver += "    end\n";
  driver += "    if engine.require('" + longModule + "') ~= nil then\n";
  driver += "        error('over-long module path loaded on retry')\n";
  driver += "    end\n";
  driver += "end\n";
  driver += "function require_max_ok()\n";
  driver += "    local mod = engine.require('" + maxModule + "')\n";
  driver += "    if type(mod) ~= 'table' or mod.ok ~= true then\n";
  driver += "        error('max-length module path failed to load')\n";
  driver += "    end\n";
  driver += "end\n";
  driver += "function script_component_long_rejected()\n";
  driver += "    local e = engine.spawn_entity()\n";
  driver += "    if e == nil then error('spawn_entity failed') end\n";
  driver += "    if engine.add_script_component(e, '" + longModule + "')\n";
  driver += "    then error('over-long script path accepted') end\n";
  driver += "end\n";
  driver += "function load_scene_long()\n";
  driver += "    engine.load_scene('" + longScene + "')\n";
  driver += "end\n";
  driver += "function load_scene_max()\n";
  driver += "    engine.load_scene('" + maxScene + "')\n";
  driver += "end\n";
  driver += "function load_scene_valid_then_long()\n";
  driver += "    engine.load_scene('scene_ok.json')\n";
  driver += "    engine.load_scene('" + longScene + "')\n";
  driver += "end\n";
  driver += "function jail_require_rejected()\n";
  driver += "    if engine.require('../jail_probe_mod.lua') ~= nil then\n";
  driver += "        error('parent-relative module path loaded')\n";
  driver += "    end\n";
  driver += "end\n";
  driver += "function jail_component_rejected()\n";
  driver += "    local e = engine.spawn_entity()\n";
  driver += "    if e == nil then error('spawn_entity failed') end\n";
  driver += "    if engine.add_script_component(e, '../escape.lua') then\n";
  driver += "        error('parent-relative script path accepted')\n";
  driver += "    end\n";
  driver += "    if engine.save_prefab(e, '../jail_evil_prefab.json') then\n";
  driver += "        error('parent-relative save_prefab accepted')\n";
  driver += "    end\n";
  driver += "end\n";
  driver += "function jail_scene_rejected()\n";
  driver += "    engine.load_scene('../jail_escape.json')\n";
  driver += "end\n";
  driver += "function jail_scene_absolute_rejected()\n";
  driver += "    engine.load_scene('/tmp/jail_escape.json')\n";
  driver += "end\n";
  driver += "function jail_save_scene_rejected()\n";
  driver += "    if engine.save_scene('../jail_evil_scene.json') then\n";
  driver += "        error('parent-relative save_scene accepted')\n";
  driver += "    end\n";
  driver += "end\n";
  driver += "function jail_misc_rejected()\n";
  driver += "    if engine.instantiate('..\\\\jail.json') ~= nil then\n";
  driver += "        error('backslash prefab path accepted')\n";
  driver += "    end\n";
  driver += "    if engine.load_asset_async('C:/jail.mesh') ~= nil then\n";
  driver += "        error('drive-designator asset path accepted')\n";
  driver += "    end\n";
  driver += "    if engine.load_sound('../jail.wav') ~= 0 then\n";
  driver += "        error('parent-relative sound path accepted')\n";
  driver += "    end\n";
  driver += "    if engine.play_music('../jail.wav') then\n";
  driver += "        error('parent-relative music path accepted')\n";
  driver += "    end\n";
  driver += "end\n";

  ctx.check(write_file_at(kDriverPath, driver.c_str()), "write driver");
  ctx.check(sc::load_script(kDriverPath), "load driver");

  ctx.check(sc::call_script_function("require_long_rejected"),
            "over-long module path is rejected, not truncated onto a file");
  ctx.check(sc::call_script_function("require_max_ok"),
            "exactly-max module path still loads");
  ctx.check(sc::call_script_function("script_component_long_rejected"),
            "over-long add_script_component path is refused");

  sc::clear_pending_scene_op();
  ctx.check(sc::call_script_function("load_scene_long"), "queue long scene");
  ctx.check(!sc::has_pending_scene_op(),
            "over-long scene path leaves no pending scene op");

  ctx.check(sc::call_script_function("load_scene_max"), "queue max scene");
  ctx.check(sc::has_pending_scene_op() &&
                (std::strcmp(sc::get_pending_scene_path(), maxScene.c_str()) ==
                 0),
            "511-character scene path is queued exactly");
  sc::clear_pending_scene_op();

  ctx.check(sc::call_script_function("load_scene_valid_then_long"),
            "queue valid then long scene");
  ctx.check(sc::has_pending_scene_op() &&
                (std::strcmp(sc::get_pending_scene_path(), "scene_ok.json") ==
                 0),
            "rejected long path preserves the previously queued request");
  sc::clear_pending_scene_op();

  ctx.check(write_file_at("../jail_probe_mod.lua", "return { esc = true }\n"),
            "write jail escape probe module");
  ctx.check(sc::call_script_function("jail_require_rejected"),
            "parent-relative module path is jailed");
  ctx.check(sc::call_script_function("jail_component_rejected"),
            "parent-relative script/prefab paths are jailed");
  ctx.check(!file_exists("../jail_evil_prefab.json"),
            "no prefab file is written outside the jail");

  ctx.check(sc::call_script_function("jail_scene_rejected"), "jail scene");
  ctx.check(!sc::has_pending_scene_op(),
            "parent-relative scene path leaves no pending scene op");
  ctx.check(sc::call_script_function("jail_scene_absolute_rejected"),
            "jail absolute scene");
  ctx.check(!sc::has_pending_scene_op(),
            "absolute scene path leaves no pending scene op");
  ctx.check(sc::call_script_function("jail_save_scene_rejected"),
            "parent-relative save_scene is jailed");
  ctx.check(!file_exists("../jail_evil_scene.json"),
            "no scene file is written outside the jail");
  ctx.check(sc::call_script_function("jail_misc_rejected"),
            "backslash/drive/parent paths are jailed on asset and audio");

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  static_cast<void>(std::remove(kDriverPath));
  static_cast<void>(std::remove(wrongFile.c_str()));
  static_cast<void>(std::remove(longModule.c_str()));
  static_cast<void>(std::remove(maxModule.c_str()));
  static_cast<void>(std::remove("../jail_probe_mod.lua"));
  static_cast<void>(std::remove("../jail_evil_prefab.json"));
  static_cast<void>(std::remove("../jail_evil_scene.json"));
  return ctx.finish("script_path_ingress");
}
