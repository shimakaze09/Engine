#include <cstddef>
#include <cstdio>
#include <cstring>

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

  // Write.
  if (!vfs_write_text("tmp/_vfs_test_file.txt", testData, testSize)) {
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  // Read back.
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

  // Clean up test file.
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

  // Write a temp file to ensure it exists.
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

} // namespace

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
  return 0;
}
