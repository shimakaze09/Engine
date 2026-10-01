// A save that does not load is never overwritten. A corrupt save.json sits
// in a real project data directory, and a script runs the usual pattern,
// `local data = engine.load_data() or {}` and then engine.save_data, through
// the production runtime services. The file must survive byte for byte:
// load_data answers "corrupt" and holds the slot, and save_data is refused.
// engine.discard_save then moves it aside to save.json.discarded-1, and the
// next save writes a fresh save.json. Before, load_data returned the same nil
// as for no save, and the first autosave replaced the only copy of the
// player's progress.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace fs = std::filesystem;

constexpr const char *kProfileRoot = "save_corrupt_kept_profile";
constexpr const char *kProjectRoot = "save_corrupt_kept_project";
constexpr const char *kScriptPath = "save_corrupt_kept_test.lua";
constexpr const char *kCorrupt = "{\"version\":1,\"entries\":[{\"k\":\"coins\"";

constexpr const char *kScript =
    "function autosave()\n"
    "  local data = engine.load_data() or {}\n"
    "  data.coins = 1\n"
    "  engine.save_data(data)\n"
    "end\n"
    "function expect_corrupt()\n"
    "  local data, status = engine.load_data()\n"
    "  if data ~= nil or status ~= 'corrupt' then\n"
    "    error('status ' .. tostring(status))\n"
    "  end\n"
    "end\n"
    "function discard_and_save()\n"
    "  if not engine.discard_save() then error('discard refused') end\n"
    "  if not engine.save_data({coins = 2}) then error('save refused') end\n"
    "  local data, status = engine.load_data()\n"
    "  if status ~= 'ok' or data.coins ~= 2 then error('fresh save lost') end\n"
    "end\n";

/// Points the per-user save directory at a fresh directory of the run's
/// own, as engine_integration_project_save_scope does.
bool redirect_user_profile() {
  std::error_code ec{};
  const fs::path root = fs::absolute(fs::path(kProfileRoot), ec);
  if (ec) {
    return false;
  }
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (ec) {
    return false;
  }
  const std::string text = root.string();
#if defined(_WIN32)
  return _putenv_s("APPDATA", text.c_str()) == 0;
#elif defined(__APPLE__)
  return setenv("HOME", text.c_str(), 1) == 0;
#else
  return setenv("XDG_DATA_HOME", text.c_str(), 1) == 0;
#endif
}

std::string read_file(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

bool write_file(const fs::path &path, const char *text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  return static_cast<bool>(stream);
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  std::error_code ec{};
  fs::create_directories(fs::path(kProjectRoot), ec);
  char directory[1024] = {};
  ctx.check(redirect_user_profile() &&
                engine::core::set_project_data_root(kProjectRoot) &&
                engine::core::project_data_dir(directory, sizeof(directory)),
            "a project data directory of the run's own");
  fs::create_directories(fs::path(directory), ec);
  const fs::path slot = fs::path(directory) / "save.json";
  const fs::path aside = fs::path(directory) / "save.json.discarded-1";
  ctx.check(write_file(slot, kCorrupt), "plant a corrupt save");

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  ctx.check((world != nullptr) && engine::core::initialize_logging() &&
                engine::scripting::initialize_scripting(),
            "initialize");
  if (world == nullptr) {
    return ctx.finish("save_corrupt_kept");
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
  std::ofstream(kScriptPath, std::ios::binary) << kScript;
  ctx.check(engine::scripting::load_script(kScriptPath), "load the script");

  static_cast<void>(engine::scripting::call_script_function("autosave"));
  ctx.check(read_file(slot) == kCorrupt,
            "an autosave after a failed load leaves the corrupt save intact");
  ctx.check(engine::scripting::call_script_function("expect_corrupt"),
            "load_data says the save is corrupt, not absent");
  ctx.check(engine::scripting::call_script_function("discard_and_save") &&
                (read_file(aside) == kCorrupt),
            "discard_save moves the save aside and a fresh save follows");

  engine::scripting::shutdown_scripting();
  engine::core::shutdown_logging();
  engine::core::clear_project_data_root();
  static_cast<void>(std::remove(kScriptPath));
  fs::remove_all(fs::path(kProfileRoot), ec);
  fs::remove_all(fs::path(kProjectRoot), ec);
  return ctx.finish("save_corrupt_kept");
}
