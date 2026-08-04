// Verifies cook-stamp recook decisions for the Engine test suite (audit
// H-20, issue #55): a stamp written by the current tool is up to date,
// while a missing stamp, a legacy stamp without a TOOL_VERSION key, a
// stamp from a different tool version, changed source/dependency hashes,
// a missing output manifest, or a missing/altered manifest-listed output
// all force a recook; stale-manifest entries are retired at commit and a
// failed retirement blocks.

#include "packer_shared.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char *kOutputPath = "cook_stamp_test_output.mesh";
constexpr const char *kStampPath = "cook_stamp_test_output.mesh.cookstamp";
constexpr const char *kSidecarPath = "cook_stamp_test_output.skel";
constexpr const char *kStaleDirPath = "cook_stamp_test_output.staledir";

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
  static_cast<void>(std::remove(kSidecarPath));
  std::error_code ignored{};
  std::filesystem::remove_all(kStaleDirPath, ignored);
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

  const std::vector<std::string> outputs{kOutputPath};
  if (!write_cook_stamp(kOutputPath, sourceHash, dependencies, importHash,
                        outputs)) {
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

  // A current-version stamp stripped of its output manifest must recook
  // instead of certifying an unknown output set (issue #55).
  char strippedStamp[512] = {};
  std::snprintf(strippedStamp, sizeof(strippedStamp),
                "SCHEMA 3\n"
                "TOOL_VERSION %u\n"
                "SOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH 99aabbccddeeff00\n"
                "DEP_HASH 0102030405060708 textures/crate.png\n",
                static_cast<unsigned int>(kCookToolVersion));
  if (!write_file(kStampPath, strippedStamp)) {
    remove_files();
    return 411;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash) !=
      true) {
    remove_files();
    return 412;
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

/// EXPECTATION (issue #55): the stamp's output manifest owns the cooked
/// output set — a missing manifest-listed sidecar forces a recook, the
/// verify mode re-hashes committed bytes, remove_stale_outputs retires
/// entries the current cook no longer produces, and a failed retirement
/// reports failure so the caller can block the stamp.
int check_output_manifest_owns_output_set() {
  remove_files();
  if (!write_file(kOutputPath, "cooked-bytes") ||
      !write_file(kSidecarPath, "skeleton-bytes")) {
    remove_files();
    return 501;
  }

  const std::uint64_t sourceHash = 0x1122334455667788ULL;
  const std::uint64_t importHash = 0x99AABBCCDDEEFF00ULL;
  const std::vector<DependencyDigest> dependencies{};
  const std::vector<std::string> outputs{kOutputPath, kSidecarPath};

  if (!write_cook_stamp(kOutputPath, sourceHash, dependencies, importHash,
                        outputs)) {
    remove_files();
    return 502;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash)) {
    remove_files();
    return 503;
  }

  static_cast<void>(std::remove(kSidecarPath));
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash) !=
      true) {
    remove_files();
    return 504;
  }

  if (!write_file(kSidecarPath, "different-skeleton-bytes")) {
    remove_files();
    return 505;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash)) {
    remove_files();
    return 506;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    true) != true) {
    remove_files();
    return 507;
  }

  const std::vector<std::string> withoutSidecar{kOutputPath};
  if (!remove_stale_outputs(kOutputPath, withoutSidecar)) {
    remove_files();
    return 508;
  }
  if (file_exists(kSidecarPath) || !file_exists(kOutputPath)) {
    remove_files();
    return 509;
  }

  std::error_code dirError{};
  std::filesystem::create_directory(kStaleDirPath, dirError);
  const std::string blockerPath = std::string(kStaleDirPath) + "/member";
  if (dirError || !write_file(blockerPath.c_str(), "occupied")) {
    remove_files();
    return 510;
  }
  char undeletableStamp[512] = {};
  std::snprintf(undeletableStamp, sizeof(undeletableStamp),
                "SCHEMA 3\n"
                "TOOL_VERSION %u\n"
                "SOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH 99aabbccddeeff00\n"
                "OUTPUT 0000000000000001 %s\n"
                "OUTPUT 0000000000000002 %s\n",
                static_cast<unsigned int>(kCookToolVersion), kOutputPath,
                kStaleDirPath);
  if (!write_file(kStampPath, undeletableStamp)) {
    remove_files();
    return 511;
  }
  if (remove_stale_outputs(kOutputPath, withoutSidecar) != false) {
    remove_files();
    return 512;
  }

  remove_files();
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  const int toolVersionResult = check_tool_version_gates_recook();
  if (toolVersionResult != 0) {
    return toolVersionResult;
  }
  return check_output_manifest_owns_output_set();
}
