// Regression for #409: a script under an asset root that is not the working
// directory must load and hot-reload through the VFS mount. Chunk paths
// were handed to luaL_loadfile verbatim and the watcher stat'ed the same
// raw path, so "assets/x.lua" only ever worked because the default mount
// happened to coincide with a directory of that name under the cwd. Here
// the "assets" prefix is mounted on a scratch directory outside the
// working directory, which holds no assets/ of its own.

#include "../test_harness.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
#include "engine/scripting/scripting.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

constexpr const char *kVirtualPath = "assets/mount_resolution.lua";

/// Writes the script, then moves its mtime forward so a rewrite is seen
/// as a change without sleeping across a filesystem timestamp tick.
bool write_script(const std::filesystem::path &path, int value) noexcept {
  std::FILE *file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const int written = std::fprintf(
      file,
      "MOUNTED_VALUE = %d\n"
      "function expect_value(v) if MOUNTED_VALUE ~= v then error('stale') "
      "end end\n"
      "function expect_one() expect_value(1) end\n"
      "function expect_two() expect_value(2) end\n",
      value);
  if ((std::fclose(file) != 0) || (written <= 0)) {
    return false;
  }
  std::error_code ec{};
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now() +
                std::chrono::seconds(2 * value),
      ec);
  return !ec;
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;

  char tempDir[512] = {};
  ctx.check(engine::core::platform_get_temp_dir(tempDir, sizeof(tempDir)),
            "temp dir");
  const std::filesystem::path root =
      std::filesystem::path(tempDir) / "engine_script_mount_resolution";
  std::error_code ec{};
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  ctx.check(!ec, "scratch root");
  const std::filesystem::path script = root / "mount_resolution.lua";
  ctx.check(write_script(script, 1), "write script v1");
  ctx.check(!std::filesystem::exists("assets/mount_resolution.lua", ec),
            "no cwd-relative copy exists; only the mount can find it");

  ctx.check(engine::core::initialize_vfs(), "initialize vfs");
  ctx.check(engine::core::mount("assets", root.string().c_str()),
            "mount assets on the scratch root");
  ctx.check(engine::scripting::initialize_scripting(), "initialize scripting");

  ctx.check(engine::scripting::load_script(kVirtualPath),
            "script under the mount loads");
  ctx.check(engine::scripting::call_script_function("expect_one"),
            "loaded chunk came from the mount");

  engine::scripting::watch_script_file(kVirtualPath);
  ctx.check(write_script(script, 2), "write script v2");
  engine::scripting::check_script_reload();
  ctx.check(engine::scripting::call_script_function("expect_two"),
            "hot reload picked up the rewrite through the mount");

  engine::scripting::shutdown_scripting();
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(root, ec);
  return ctx.finish("script_mount_resolution");
}
