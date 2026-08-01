// Verifies the atomic authored-file write (audit C-05): new files land
// with exact content, overwrites replace atomically, failures never
// destroy the previous valid file, and the sibling temporary never
// survives a completed or failed commit.

#include "engine/core/atomic_file.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace {

constexpr const char *kPath = "atomic_file_test_tmp.json";
constexpr const char *kTempPath = "atomic_file_test_tmp.json.new";

/// Reads the whole file; empty string when missing.
std::string read_all(const char *path) {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return {};
  }
  char buffer[256] = {};
  const std::size_t read = std::fread(buffer, 1U, sizeof(buffer) - 1U, file);
  std::fclose(file);
  return std::string(buffer, read);
}

/// Removes both the destination and any leftover temporary.
void cleanup() {
  static_cast<void>(std::remove(kPath));
  static_cast<void>(std::remove(kTempPath));
}

/// EXPECTATION: a fresh write creates the file with exactly the payload
/// and leaves no temporary behind.
int check_fresh_write() {
  cleanup();
  const char *payload = "{\"first\":1}";
  if (!engine::core::atomic_write_file(kPath, payload,
                                       std::strlen(payload))) {
    return 1;
  }
  if (read_all(kPath) != payload) {
    return 2;
  }
  if (std::filesystem::exists(kTempPath)) {
    return 3;
  }
  return 0;
}

/// EXPECTATION: overwriting replaces the previous content exactly.
int check_overwrite() {
  const char *payload = "{\"second\":2}";
  if (!engine::core::atomic_write_file(kPath, payload,
                                       std::strlen(payload))) {
    return 10;
  }
  if (read_all(kPath) != payload) {
    return 11;
  }
  return 0;
}

/// EXPECTATION: invalid arguments and unwritable temporary locations fail
/// cleanly without touching the destination.
int check_failed_write_preserves_destination() {
  const std::string before = read_all(kPath);
  if (before.empty()) {
    return 20;
  }

  if (engine::core::atomic_write_file(nullptr, "x", 1U) ||
      engine::core::atomic_write_file(kPath, nullptr, 1U) ||
      engine::core::atomic_write_file(kPath, "x", 0U)) {
    return 21;
  }

  const char *missingDirectory =
      "atomic_file_test_missing_dir/atomic_file_test_tmp.json";
  if (engine::core::atomic_write_file(missingDirectory, "x", 1U)) {
    return 22;
  }

  if (read_all(kPath) != before) {
    return 23;
  }
  return 0;
}

/// EXPECTATION: a rename that cannot replace its destination (a
/// directory) fails, leaves the destination untouched, and removes the
/// temporary.
int check_rename_failure_cleans_temporary() {
  const char *directoryTarget = "atomic_file_test_dir_target";
  std::error_code ec{};
  std::filesystem::remove_all(directoryTarget, ec);
  if (!std::filesystem::create_directory(directoryTarget, ec) || ec) {
    return 30;
  }

  const bool wrote =
      engine::core::atomic_write_file(directoryTarget, "x", 1U);
  const bool stillDirectory = std::filesystem::is_directory(directoryTarget);
  const bool tempGone =
      !std::filesystem::exists(std::string(directoryTarget) + ".new");
  std::filesystem::remove_all(directoryTarget, ec);

  if (wrote || !stillDirectory || !tempGone) {
    return 31;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_fresh_write();
  if (result == 0) {
    result = check_overwrite();
  }
  if (result == 0) {
    result = check_failed_write_preserves_destination();
  }
  if (result == 0) {
    result = check_rename_failure_cleans_temporary();
  }
  cleanup();

  if (result != 0) {
    std::fprintf(stderr, "atomic_file_test failed: %d\n", result);
    return result;
  }
  std::printf("atomic_file_test: all tests passed\n");
  return 0;
}
