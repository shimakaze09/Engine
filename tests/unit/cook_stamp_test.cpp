// Verifies cook-stamp recook decisions for the Engine test suite (audit
// H-20): a stamp written by the current tool is up to date, while a
// missing stamp, a legacy stamp without a TOOL_VERSION key, a stamp from
// a different tool version, or changed source/dependency hashes all
// force a recook.

#include "packer_shared.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *kOutputPath = "cook_stamp_test_output.mesh";
constexpr const char *kStampPath = "cook_stamp_test_output.mesh.cookstamp";

bool write_file(const char *path, const char *text) {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

void remove_files() {
  static_cast<void>(std::remove(kOutputPath));
  static_cast<void>(std::remove(kStampPath));
}

/// EXPECTATION (audit H-20): the recook decision covers tool-version
/// migration — current-version stamps with matching hashes skip the
/// cook; missing stamps, legacy stamps without TOOL_VERSION, stamps
/// from another tool version, and changed hashes all recook.
int check_tool_version_gates_recook() {
  remove_files();
  if (!write_file(kOutputPath, "cooked-bytes")) {
    return 401;
  }

  const std::uint64_t sourceHash = 0x1122334455667788ULL;
  const std::uint64_t importHash = 0x99AABBCCDDEEFF00ULL;
  std::vector<DependencyDigest> dependencies{};
  DependencyDigest dep{};
  dep.path = "textures/crate.png";
  dep.hash = 0x0102030405060708ULL;
  dependencies.push_back(dep);

  if (should_repack(kOutputPath, sourceHash, dependencies, importHash) !=
      true) {
    remove_files();
    return 402;
  }

  if (!write_cook_stamp(kOutputPath, sourceHash, dependencies, importHash)) {
    remove_files();
    return 403;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash)) {
    remove_files();
    return 404;
  }

  if (should_repack(kOutputPath, sourceHash + 1U, dependencies, importHash) !=
      true) {
    remove_files();
    return 405;
  }
  std::vector<DependencyDigest> changedDependencies = dependencies;
  changedDependencies[0].hash ^= 1ULL;
  if (should_repack(kOutputPath, sourceHash, changedDependencies,
                    importHash) != true) {
    remove_files();
    return 406;
  }

  // A legacy stamp (written before TOOL_VERSION existed) with otherwise
  // matching hashes must recook exactly once.
  if (!write_file(kStampPath,
                  "SCHEMA 2\n"
                  "SOURCE_HASH 1122334455667788\n"
                  "IMPORT_HASH 99aabbccddeeff00\n"
                  "DEP_HASH 0102030405060708 textures/crate.png\n")) {
    remove_files();
    return 407;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash) !=
      true) {
    remove_files();
    return 408;
  }

  // A stamp from a different tool version must recook.
  if (!write_file(kStampPath,
                  "SCHEMA 2\n"
                  "TOOL_VERSION 999\n"
                  "SOURCE_HASH 1122334455667788\n"
                  "IMPORT_HASH 99aabbccddeeff00\n"
                  "DEP_HASH 0102030405060708 textures/crate.png\n")) {
    remove_files();
    return 409;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash) !=
      true) {
    remove_files();
    return 410;
  }

  remove_files();
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() { return check_tool_version_gates_recook(); }
