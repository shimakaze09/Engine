// Verifies vfs test behavior for the Engine test suite.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"

using namespace engine::core;

namespace {

bool test_init_shutdown() noexcept {
  if (!initialize_vfs()) {
    return false;
  }
  shutdown_vfs();
  return true;
}

bool test_mount_unmount() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  if (!mount("data", ".")) {
    shutdown_vfs();
    return false;
  }

  // Unmount known prefix succeeds.
  if (!unmount("data")) {
    shutdown_vfs();
    return false;
  }

  // Unmount unknown prefix fails.
  if (unmount("data")) {
    shutdown_vfs();
    return false;
  }

  shutdown_vfs();
  return true;
}

bool test_path_resolution() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  if (!mount("assets", ".")) {
    shutdown_vfs();
    return false;
  }

  char resolved[512] = {};
  if (!vfs_resolve_os_path("assets/test.txt", resolved, sizeof(resolved))) {
    shutdown_vfs();
    return false;
  }

  // Should be "./test.txt"
  if (std::strcmp(resolved, "./test.txt") != 0) {
    shutdown_vfs();
    return false;
  }

  // Unmounted prefix should fail.
  if (vfs_resolve_os_path("unknown/test.txt", resolved, sizeof(resolved))) {
    shutdown_vfs();
    return false;
  }

  const char *badPaths[] = {
      "assets/../outside.txt",
      "assets/sub/../../outside.txt",
      "assets/./same.txt",
      "assets/sub/./same.txt",
      "assets//absolute-like.txt",
      "assets/C:/outside.txt",
      "assets/sub/D:/outside.txt",
      "assets\\..\\outside.txt",
      "assets\\C:\\outside.txt",
  };
  for (const char *path : badPaths) {
    if (vfs_resolve_os_path(path, resolved, sizeof(resolved))) {
      shutdown_vfs();
      return false;
    }
  }

  shutdown_vfs();
  return true;
}

bool test_read_write_roundtrip() noexcept {
  if (!initialize_logging()) {
    return false;
  }
  if (!initialize_vfs()) {
    shutdown_logging();
    return false;
  }

  if (!mount("tmp", ".")) {
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  const char *testData = "Hello, VFS!";
  const std::size_t testSize = std::strlen(testData);

  if (!vfs_write_text("tmp/_vfs_test_file.txt", testData, testSize)) {
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  char *readBack = nullptr;
  std::size_t readSize = 0U;
  if (!vfs_read_text("tmp/_vfs_test_file.txt", &readBack, &readSize)) {
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  bool ok = (readSize == testSize)
            && (std::memcmp(readBack, testData, testSize) == 0);
  vfs_free(readBack);

  std::remove("_vfs_test_file.txt");

  shutdown_vfs();
  shutdown_logging();
  return ok;
}

bool test_file_exists() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  if (!mount("root", ".")) {
    shutdown_vfs();
    return false;
  }

  const char *data = "x";
  if (!vfs_write_binary("root/_vfs_exist_test.dat", data, 1U)) {
    shutdown_vfs();
    return false;
  }

  if (!vfs_file_exists("root/_vfs_exist_test.dat")) {
    std::remove("_vfs_exist_test.dat");
    shutdown_vfs();
    return false;
  }

  // Non-existent file should return false.
  if (vfs_file_exists("root/_vfs_no_such_file_12345.dat")) {
    std::remove("_vfs_exist_test.dat");
    shutdown_vfs();
    return false;
  }

  std::remove("_vfs_exist_test.dat");
  shutdown_vfs();
  return true;
}

/// vfs_file_size reports a regular file's exact byte count (zero included)
/// from metadata, and refuses a directory, a missing file, an unmounted
/// path, and a null output.
bool test_file_size() noexcept {
  namespace fs = std::filesystem;
  if (!initialize_vfs()) {
    return false;
  }
  if (!mount("root", ".")) {
    shutdown_vfs();
    return false;
  }

  const char *data = "12345";
  bool ok = vfs_write_binary("root/_vfs_size_test.dat", data, 5U) &&
            vfs_write_binary("root/_vfs_size_empty.dat", data, 0U);
  std::error_code ec{};
  fs::remove_all("_vfs_size_dir", ec);
  ec.clear();
  ok = ok && fs::create_directory("_vfs_size_dir", ec) && !ec;

  std::uint64_t size = 99U;
  ok = ok && vfs_file_size("root/_vfs_size_test.dat", &size) && (size == 5U);
  ok = ok && vfs_file_size("root/_vfs_size_empty.dat", &size) && (size == 0U);
  ok = ok && !vfs_file_size("root/_vfs_size_dir", &size);
  ok = ok && !vfs_file_size("root/_vfs_size_missing.dat", &size);
  ok = ok && !vfs_file_size("unmounted/_vfs_size_test.dat", &size);
  ok = ok && !vfs_file_size("root/_vfs_size_test.dat", nullptr);

  fs::remove_all("_vfs_size_dir", ec);
  std::remove("_vfs_size_test.dat");
  std::remove("_vfs_size_empty.dat");
  shutdown_vfs();
  return ok;
}

bool test_mtime() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  if (!mount("root", ".")) {
    shutdown_vfs();
    return false;
  }

  const char *data = "mtime";
  if (!vfs_write_binary("root/_vfs_mtime_test.dat", data, 5U)) {
    shutdown_vfs();
    return false;
  }

  const auto mtime = vfs_file_mtime("root/_vfs_mtime_test.dat");
  std::remove("_vfs_mtime_test.dat");
  shutdown_vfs();

  return mtime > 0;
}

/// Fault injection (audit P2-7): a write whose atomic rename cannot
/// replace its destination (a directory) fails, a pre-existing sibling
/// file written earlier keeps its bytes after a failed overwrite of a
/// directory-shadowed name, and no ".new" temporary survives.
bool test_write_failure_preserves_existing() noexcept {
  namespace fs = std::filesystem;
  if (!initialize_vfs()) {
    return false;
  }
  if (!mount("root", ".")) {
    shutdown_vfs();
    return false;
  }

  const char *previous = "previous-bytes";
  bool ok = vfs_write_binary("root/_vfs_atomic_keep.dat", previous,
                             std::strlen(previous));

  std::error_code ec{};
  fs::remove_all("_vfs_atomic_dir", ec);
  ec.clear();
  ok = ok && fs::create_directory("_vfs_atomic_dir", ec) && !ec;

  ok = ok && !vfs_write_binary("root/_vfs_atomic_dir", "x", 1U);
  ok = ok && fs::is_directory("_vfs_atomic_dir", ec);

  char *readBack = nullptr;
  std::size_t readSize = 0U;
  if (ok && vfs_read_text("root/_vfs_atomic_keep.dat", &readBack, &readSize)) {
    ok = (readSize == std::strlen(previous)) &&
         (std::memcmp(readBack, previous, readSize) == 0);
    vfs_free(readBack);
  } else {
    ok = false;
  }

  std::size_t leftovers = 0U;
  for (const auto &entry : fs::directory_iterator(".", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("_vfs_atomic_dir.new", 0U) == 0U) {
      ++leftovers;
    }
  }

  fs::remove_all("_vfs_atomic_dir", ec);
  std::remove("_vfs_atomic_keep.dat");
  shutdown_vfs();
  return ok && (leftovers == 0U);
}

bool test_longest_prefix_match() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  // Mount two prefixes where one is a sub-prefix of the other.
  if (!mount("assets", "/general")) {
    shutdown_vfs();
    return false;
  }
  if (!mount("assets/textures", "/textures")) {
    shutdown_vfs();
    return false;
  }

  char resolved[512] = {};

  // "assets/textures/foo.png" should match the longer "assets/textures" prefix.
  if (!vfs_resolve_os_path(
          "assets/textures/foo.png", resolved, sizeof(resolved))) {
    shutdown_vfs();
    return false;
  }
  if (std::strcmp(resolved, "/textures/foo.png") != 0) {
    shutdown_vfs();
    return false;
  }

  // "assets/sounds/bar.wav" should match the shorter "assets" prefix.
  if (!vfs_resolve_os_path(
          "assets/sounds/bar.wav", resolved, sizeof(resolved))) {
    shutdown_vfs();
    return false;
  }
  if (std::strcmp(resolved, "/general/sounds/bar.wav") != 0) {
    shutdown_vfs();
    return false;
  }

  shutdown_vfs();
  return true;
}

// Regression for issue #113: trailing-slash and backslash prefix forms must
// identify the same mount for remount, unmount, and resolution.
bool test_prefix_form_equivalence() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  // Remounting the trailing-slash form must replace the plain-form entry,
  // not append a shadowed duplicate, so resolution follows the new target.
  if (!mount("assets", "/first") || !mount("assets/", "/second")) {
    shutdown_vfs();
    return false;
  }
  char resolved[512] = {};
  if (!vfs_resolve_os_path("assets/foo.txt", resolved, sizeof(resolved)) ||
      (std::strcmp(resolved, "/second/foo.txt") != 0)) {
    shutdown_vfs();
    return false;
  }
  // Backslash form remounts the same entry too.
  if (!mount("assets\\", "/third") ||
      !vfs_resolve_os_path("assets/foo.txt", resolved, sizeof(resolved)) ||
      (std::strcmp(resolved, "/third/foo.txt") != 0)) {
    shutdown_vfs();
    return false;
  }

  // Every equivalent form unmounts the single entry; the second unmount of
  // any form must report no such mount.
  if (!unmount("assets/")) {
    shutdown_vfs();
    return false;
  }
  if (unmount("assets") || unmount("assets\\")) {
    shutdown_vfs();
    return false;
  }
  if (vfs_resolve_os_path("assets/foo.txt", resolved, sizeof(resolved))) {
    shutdown_vfs();
    return false;
  }

  // A prefix that normalizes to empty ("/" loses its trailing slash) is
  // rejected rather than stored as an unmountable empty entry.
  if (mount("/", "/root") || mount("\\", "/root")) {
    shutdown_vfs();
    return false;
  }

  shutdown_vfs();
  return true;
}

// Regression for issue #113: repeated trailing-slash mount/unmount cycles
// must not consume extra mount-table slots, and nested mounts keep their
// longest-prefix behavior across slash forms.
bool test_prefix_remount_cycles_and_nested() noexcept {
  if (!initialize_vfs()) {
    return false;
  }

  // 64 cycles far exceeds the fixed table capacity, so any leaked slot
  // would surface as a failed mount or a stale resolution target.
  for (int cycle = 0; cycle < 64; ++cycle) {
    const bool slashForm = (cycle % 2) == 0;
    if (!mount(slashForm ? "cycle/" : "cycle", "/cycle")) {
      shutdown_vfs();
      return false;
    }
    if (!unmount(slashForm ? "cycle" : "cycle/")) {
      shutdown_vfs();
      return false;
    }
  }

  // Repeated remounts (no unmount) must reuse the one entry as well.
  for (int cycle = 0; cycle < 64; ++cycle) {
    if (!mount("stack/", "/stack")) {
      shutdown_vfs();
      return false;
    }
  }
  char resolved[512] = {};
  if (!vfs_resolve_os_path("stack/a", resolved, sizeof(resolved)) ||
      (std::strcmp(resolved, "/stack/a") != 0)) {
    shutdown_vfs();
    return false;
  }

  // Nested mounts registered with mixed slash forms still resolve by the
  // longest matching prefix and unmount independently.
  if (!mount("game/", "/game") || !mount("game/audio/", "/audio")) {
    shutdown_vfs();
    return false;
  }
  if (!vfs_resolve_os_path("game/audio/hit.wav", resolved,
                           sizeof(resolved)) ||
      (std::strcmp(resolved, "/audio/hit.wav") != 0)) {
    shutdown_vfs();
    return false;
  }
  if (!unmount("game/audio")) {
    shutdown_vfs();
    return false;
  }
  if (!vfs_resolve_os_path("game/audio/hit.wav", resolved,
                           sizeof(resolved)) ||
      (std::strcmp(resolved, "/game/audio/hit.wav") != 0)) {
    shutdown_vfs();
    return false;
  }
  if (!unmount("game")) {
    shutdown_vfs();
    return false;
  }

  shutdown_vfs();
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!test_init_shutdown()) {
    return 1;
  }
  if (!test_mount_unmount()) {
    return 2;
  }
  if (!test_path_resolution()) {
    return 3;
  }
  if (!test_read_write_roundtrip()) {
    return 4;
  }
  if (!test_file_exists()) {
    return 5;
  }
  if (!test_mtime()) {
    return 6;
  }
  if (!test_longest_prefix_match()) {
    return 7;
  }
  if (!test_write_failure_preserves_existing()) {
    return 8;
  }
  if (!test_prefix_form_equivalence()) {
    return 9;
  }
  if (!test_prefix_remount_cycles_and_nested()) {
    return 10;
  }
  if (!test_file_size()) {
    return 11;
  }
  return 0;
}
