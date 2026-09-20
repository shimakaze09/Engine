// Verifies cook-stamp recook decisions for the Engine test suite (audit
// H-20, issue #55): a stamp written by the current tool is up to date,
// while a missing stamp, a legacy stamp without a TOOL_VERSION key, a
// stamp from a different tool version, changed source/dependency hashes,
// a missing output manifest, a platform-tag mismatch (issue #81), or a
// missing/altered manifest-listed output all force a recook;
// stale-manifest entries are retired at commit and a failed retirement
// blocks; a failed thumbnail regeneration retires the previous
// generation's thumbnail and checksum instead of re-certifying them
// (audit #211); the cook key folds the mesh cook's logic revision and a
// newer stamp schema recooks (#424); paths are recorded relative to the
// stamp, retirement never leaves the stamp's directory, and an overlong
// path refuses the stamp (#527).

#include "packer_shared.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr const char *kPlatform = "TestPlat";
constexpr const char *kOutputPath = "cook_stamp_test_output.mesh";
constexpr const char *kStampPath = "cook_stamp_test_output.mesh.cookstamp";
constexpr const char *kSidecarPath = "cook_stamp_test_output.skel";
constexpr const char *kStaleDirPath = "cook_stamp_test_output.staledir";
constexpr const char *kNestedDir = "cook_stamp_test_dir";
constexpr const char *kVictimPath = "cook_stamp_test_victim.txt";
constexpr const char *kDepPath = "cook_stamp_test_dep.bin";

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
  static_cast<void>(std::remove(kVictimPath));
  static_cast<void>(std::remove(kDepPath));
  std::error_code ignored{};
  std::filesystem::remove_all(kStaleDirPath, ignored);
  std::filesystem::remove_all(kNestedDir, ignored);
}

/// Reads a whole text file; empty on failure.
std::string read_text(const char *path) {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return std::string{};
  }
  std::string text{};
  char buffer[512] = {};
  std::size_t got = 0U;
  while ((got = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    text.append(buffer, got);
  }
  std::fclose(file);
  return text;
}

/// EXPECTATION (#527): a stamp records its outputs and dependencies
/// relative to its own directory, so the same stamp certifies the same
/// files from any working directory; on base the invocation paths were
/// recorded verbatim and a different working directory saw every
/// output as missing.
int check_stamp_paths_are_relative_to_the_stamp() {
  remove_files();
  std::error_code ec{};
  std::filesystem::create_directories(kNestedDir, ec);
  const std::string output = std::string(kNestedDir) + "/rel.mesh";
  const std::string sidecar = std::string(kNestedDir) + "/rel.skel";
  if (ec || !write_file(output.c_str(), "cooked") ||
      !write_file(sidecar.c_str(), "skeleton") ||
      !write_file(kDepPath, "dependency")) {
    remove_files();
    return 801;
  }
  std::vector<DependencyDigest> dependencies{};
  if (!build_dependency_digests({kDepPath}, &dependencies)) {
    remove_files();
    return 802;
  }
  const std::uint64_t sourceHash = 0x1122334455667788ULL;
  const std::uint64_t importHash = 0x99AABBCCDDEEFF00ULL;
  const std::vector<std::string> outputs{output, sidecar};
  if (!write_cook_stamp(output.c_str(), nullptr,
                   sourceHash, dependencies, importHash,
                        kPlatform, outputs)) {
    remove_files();
    return 803;
  }
  const std::string stamp = read_text((output + ".cookstamp").c_str());
  if ((stamp.find(" rel.mesh\n") == std::string::npos) ||
      (stamp.find(" rel.skel\n") == std::string::npos) ||
      (stamp.find(" ../cook_stamp_test_dep.bin\n") == std::string::npos) ||
      (stamp.find("cook_stamp_test_dir/rel.mesh") != std::string::npos)) {
    std::fprintf(stderr, "stamp paths are not stamp-relative:\n%s",
                 stamp.c_str());
    remove_files();
    return 804;
  }
  if (should_repack(output.c_str(), sourceHash, dependencies, importHash,
                    kPlatform, true)) {
    remove_files();
    return 805;
  }

  // The same stamp, read from a different working directory through
  // absolute paths, still certifies the same files.
  const std::filesystem::path home = std::filesystem::current_path(ec);
  const std::filesystem::path absOutput =
      std::filesystem::absolute(output, ec);
  const std::filesystem::path absDep = std::filesystem::absolute(kDepPath, ec);
  std::filesystem::current_path(std::filesystem::path(kNestedDir), ec);
  if (ec) {
    remove_files();
    return 806;
  }
  std::vector<DependencyDigest> absDependencies = dependencies;
  absDependencies[0].path = absDep.string();
  const bool repackElsewhere =
      should_repack(absOutput.string().c_str(), sourceHash, absDependencies,
                    importHash, kPlatform, true);
  const std::vector<std::string> onlyMesh{absOutput.string()};
  const bool retired =
      remove_stale_outputs(absOutput.string().c_str(), onlyMesh);
  std::filesystem::current_path(home, ec);
  if (repackElsewhere) {
    std::fprintf(stderr, "stamp did not certify its outputs from another "
                         "working directory\n");
    remove_files();
    return 807;
  }
  if (!retired || file_exists(sidecar.c_str()) || !file_exists(output.c_str())) {
    remove_files();
    return 808;
  }
  remove_files();
  return 0;
}

/// EXPECTATION (#527): a manifest entry that leaves the stamp's
/// directory, or any entry of a legacy (pre-schema-4) manifest, is never
/// removed; the escaping stamp is corrupt and recooks. On base
/// remove_stale_outputs deleted whatever path the stamp named.
int check_manifest_never_removes_outside_the_stamp_directory() {
  remove_files();
  std::error_code ec{};
  std::filesystem::create_directories(kNestedDir, ec);
  const std::string output = std::string(kNestedDir) + "/esc.mesh";
  const std::string stampPath = output + ".cookstamp";
  if (ec || !write_file(output.c_str(), "cooked") ||
      !write_file(kVictimPath, "not yours")) {
    remove_files();
    return 811;
  }
  char text[512] = {};
  std::snprintf(text, sizeof(text),
                "SCHEMA %u\nTOOL_VERSION %u\nSOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH 99aabbccddeeff00\nPLATFORM TestPlat\n"
                "OUTPUT 0000000000000001 ../cook_stamp_test_victim.txt\n"
                "OUTPUT 0000000000000002 esc.mesh\n",
                static_cast<unsigned int>(kCookStampSchema),
                static_cast<unsigned int>(kCookToolVersion));
  if (!write_file(stampPath.c_str(), text)) {
    remove_files();
    return 812;
  }
  const std::vector<std::string> current{output};
  if (!remove_stale_outputs(output.c_str(), current) ||
      !file_exists(kVictimPath)) {
    std::fprintf(stderr, "an escaping manifest entry was removed\n");
    remove_files();
    return 813;
  }
  const std::vector<DependencyDigest> noDependencies{};
  if (!should_repack(output.c_str(), 0x1122334455667788ULL, noDependencies,
                     0x99AABBCCDDEEFF00ULL, kPlatform)) {
    remove_files();
    return 814; // an escaping manifest is corrupt, so it recooks
  }

  // Legacy manifest: its paths carry no containment, so nothing is retired.
  std::snprintf(text, sizeof(text),
                "SCHEMA 3\nTOOL_VERSION 3\nSOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH 99aabbccddeeff00\nPLATFORM TestPlat\n"
                "OUTPUT 0000000000000001 %s\n"
                "OUTPUT 0000000000000002 %s\n",
                kVictimPath, kOutputPath);
  if (!write_file(kOutputPath, "cooked") || !write_file(kStampPath, text)) {
    remove_files();
    return 815;
  }
  const std::vector<std::string> onlyOutput{kOutputPath};
  if (!remove_stale_outputs(kOutputPath, onlyOutput) ||
      !file_exists(kVictimPath)) {
    std::fprintf(stderr, "a legacy manifest entry was removed\n");
    remove_files();
    return 816;
  }
  remove_files();
  return 0;
}

/// EXPECTATION (#527): an output path that would not fit a stamp line
/// refuses the stamp; on base the line was truncated and recorded, so a
/// prefix-named file was certified and later retired instead.
int check_overlong_output_path_refuses_the_stamp() {
  remove_files();
  std::error_code ec{};
  std::filesystem::path deep(kNestedDir);
  const std::string segment(200U, 'p');
  for (int level = 0; level < 5; ++level) {
    deep /= segment;
  }
  errno = 0;
  std::filesystem::create_directories(deep, ec);
  const std::string output = std::string(kNestedDir) + "/owner.mesh";
  const std::string longOutput = (deep / "long.mesh").string();
  if (ec || !write_file(output.c_str(), "cooked") ||
      !write_file(longOutput.c_str(), "far away")) {
    remove_files();
    // A filesystem that refuses the path itself — Windows past MAX_PATH
    // (260), macOS past PATH_MAX (1024) — cannot hold the file this case
    // needs. Calling the writer anyway would prove nothing: it would
    // refuse the stamp for the missing output, not for the line length.
#ifdef _WIN32
    const bool refusedByFilesystem = true;
#else
    const bool refusedByFilesystem =
        (ec == std::errc::filename_too_long) || (errno == ENAMETOOLONG);
#endif
    if (refusedByFilesystem) {
      std::printf("cook_stamp_test: overlong-output case not run: this "
                  "filesystem refuses the %zu-byte path it needs\n",
                  longOutput.size());
      return 0;
    }
    return 821;
  }
  const std::vector<DependencyDigest> noDependencies{};
  const std::vector<std::string> outputs{output, longOutput};
  const bool written =
      write_cook_stamp(output.c_str(), nullptr,
                   0x1122334455667788ULL, noDependencies,
                       0x99AABBCCDDEEFF00ULL, kPlatform, outputs);
  if (written || file_exists((output + ".cookstamp").c_str())) {
    std::fprintf(stderr, "an overlong output path was stamped\n");
    remove_files();
    return 822;
  }
  remove_files();
  return 0;
}

/// EXPECTATION (#527): the containment rule refuses every spelling that
/// leaves the stamp's directory, the Windows ones included. The rule is a
/// pure string function, so these forms are checked on every platform
/// rather than only where they are dangerous: a stamp written on one
/// machine is read on another.
int check_containment_rule_refuses_escaping_forms() {
  using engine::content::cook_stamp_path_is_contained;
  const char *contained[] = {
      "rel.mesh",
      "sub/rel.mesh",
      "sub\\rel.mesh",
      ".thumbnails/rel.png",
      // A name that merely starts with dots is a name, not a parent step.
      "...hidden/rel.mesh",
      "..rel.mesh",
  };
  for (const char *path : contained) {
    if (!cook_stamp_path_is_contained(path)) {
      std::fprintf(stderr, "an in-directory path was refused: %s\n", path);
      return 901;
    }
  }
  const char *escaping[] = {
      "",
      // Parent steps, with either separator and mixed.
      "..",
      "../victim",
      "..\\victim",
      "sub/../../victim",
      "sub\\..\\..\\victim",
      "sub/..\\../victim",
      // A current-directory step is refused too: it has no reason to be in
      // a path the packer wrote.
      "./rel.mesh",
      "sub/./rel.mesh",
      // Rooted: POSIX root, Windows current-drive root, UNC.
      "/victim",
      "\\victim",
      "//server/share/victim",
      "\\\\server\\share\\victim",
      // Drive-qualified: absolute, and the drive-relative form that names
      // another drive's current directory.
      "C:/victim",
      "C:\\victim",
      "c:victim",
      "Z:victim",
      // An empty segment.
      "sub//rel.mesh",
      "sub/",
  };
  for (const char *path : escaping) {
    if (cook_stamp_path_is_contained(path)) {
      std::fprintf(stderr, "an escaping path was accepted: [%s]\n", path);
      return 902;
    }
  }
  if (cook_stamp_path_is_contained(nullptr)) {
    return 903;
  }
  return 0;
}

/// EXPECTATION (#527 on Windows): a dependency on another volume than the
/// stamp still stamps. No path relative to the stamp exists between two
/// drives, so on base the writer refused the stamp and the cook failed
/// outright — a project on one drive with sources or an SDK on another.
/// The dependency is recorded by its absolute path, which reads back as
/// the same file, and outputs stay contained. POSIX has one root, so the
/// case cannot arise there.
int check_dependency_on_another_volume_is_stamped() {
#ifdef _WIN32
  remove_files();
  if (!write_file(kOutputPath, "cooked")) {
    return 911;
  }
  // The writer takes the digest as given and never opens a dependency, so
  // the other drive need not exist: pick a letter that is not this one.
  std::error_code ec{};
  const std::string here =
      std::filesystem::absolute(kOutputPath, ec).root_name().string();
  const std::string otherDrive = ((here == "Q:") || (here == "q:")) ? "R:" : "Q:";
  std::vector<DependencyDigest> dependencies{};
  DependencyDigest dep{};
  dep.path = otherDrive + "\\sdk\\include\\shared.sh";
  dep.hash = 0x0102030405060708ULL;
  dependencies.push_back(dep);

  const std::uint64_t sourceHash = 0x1122334455667788ULL;
  const std::uint64_t importHash = 0x99AABBCCDDEEFF00ULL;
  const std::vector<std::string> outputs{kOutputPath};
  if (!write_cook_stamp(kOutputPath, nullptr,
                   sourceHash, dependencies, importHash,
                        kPlatform, outputs)) {
    std::fprintf(stderr, "a dependency on another volume refused the stamp\n");
    remove_files();
    return 912;
  }
  const std::string stamp = read_text(kStampPath);
  const std::string recorded = " " + otherDrive + "/sdk/include/shared.sh\n";
  if (stamp.find(recorded) == std::string::npos) {
    std::fprintf(stderr, "the dependency was not recorded absolute:\n%s",
                 stamp.c_str());
    remove_files();
    return 913;
  }
  // The same dependency, however it is spelled, certifies the cook; a
  // different file on that volume does not.
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform)) {
    remove_files();
    return 914;
  }
  std::vector<DependencyDigest> respelled = dependencies;
  respelled[0].path = otherDrive + "/sdk/include/../include/shared.sh";
  if (should_repack(kOutputPath, sourceHash, respelled, importHash,
                    kPlatform)) {
    remove_files();
    return 915;
  }
  std::vector<DependencyDigest> moved = dependencies;
  moved[0].path = otherDrive + "\\sdk\\include\\other.sh";
  if (!should_repack(kOutputPath, sourceHash, moved, importHash, kPlatform)) {
    remove_files();
    return 916;
  }
  remove_files();
#endif
  return 0;
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

  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform) != true) {
    remove_files();
    return 402;
  }

  const std::vector<std::string> outputs{kOutputPath};
  if (!write_cook_stamp(kOutputPath, nullptr,
                   sourceHash, dependencies, importHash,
                        kPlatform, outputs)) {
    remove_files();
    return 403;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform)) {
    remove_files();
    return 404;
  }

  if (should_repack(kOutputPath, sourceHash + 1U, dependencies, importHash,
                    kPlatform) != true) {
    remove_files();
    return 405;
  }
  std::vector<DependencyDigest> changedDependencies = dependencies;
  changedDependencies[0].hash ^= 1ULL;
  if (should_repack(kOutputPath, sourceHash, changedDependencies,
                    importHash, kPlatform) != true) {
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
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform) != true) {
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
                "PLATFORM TestPlat\n"
                "DEP_HASH 0102030405060708 textures/crate.png\n",
                static_cast<unsigned int>(kCookToolVersion));
  if (!write_file(kStampPath, strippedStamp)) {
    remove_files();
    return 411;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform) != true) {
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
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform) != true) {
    remove_files();
    return 410;
  }

  remove_files();
  return 0;
}

/// EXPECTATION (issue #81): the platform tag is part of the cook key —
/// a stamp cooked for one platform never certifies another platform's
/// cook, and a pre-platform stamp (no PLATFORM line) recooks once.
int check_platform_tag_gates_recook() {
  remove_files();
  if (!write_file(kOutputPath, "cooked-bytes")) {
    return 601;
  }

  const std::uint64_t sourceHash = 0x1122334455667788ULL;
  const std::uint64_t importHash = 0x99AABBCCDDEEFF00ULL;
  const std::vector<DependencyDigest> dependencies{};
  const std::vector<std::string> outputs{kOutputPath};

  if (!write_cook_stamp(kOutputPath, nullptr,
                   sourceHash, dependencies, importHash,
                        kPlatform, outputs)) {
    remove_files();
    return 602;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform)) {
    remove_files();
    return 603;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    "OtherPlat") != true) {
    remove_files();
    return 604;
  }

  char prePlatformStamp[512] = {};
  std::snprintf(prePlatformStamp, sizeof(prePlatformStamp),
                "SCHEMA 3\n"
                "TOOL_VERSION %u\n"
                "SOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH 99aabbccddeeff00\n"
                "OUTPUT 0000000000000001 %s\n",
                static_cast<unsigned int>(kCookToolVersion), kOutputPath);
  if (!write_file(kStampPath, prePlatformStamp)) {
    remove_files();
    return 605;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform) != true) {
    remove_files();
    return 606;
  }

  if (write_cook_stamp(kOutputPath, nullptr,
                   sourceHash, dependencies, importHash,
                       "bad tag", outputs) != false) {
    remove_files();
    return 607;
  }
  if (is_valid_platform_tag(nullptr) || is_valid_platform_tag("") ||
      is_valid_platform_tag("two words") || !is_valid_platform_tag("Web")) {
    remove_files();
    return 608;
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

  if (!write_cook_stamp(kOutputPath, nullptr,
                   sourceHash, dependencies, importHash,
                        kPlatform, outputs)) {
    remove_files();
    return 502;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform)) {
    remove_files();
    return 503;
  }

  static_cast<void>(std::remove(kSidecarPath));
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform) != true) {
    remove_files();
    return 504;
  }

  if (!write_file(kSidecarPath, "different-skeleton-bytes")) {
    remove_files();
    return 505;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform)) {
    remove_files();
    return 506;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, importHash,
                    kPlatform, true) != true) {
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
                "SCHEMA %u\n"
                "TOOL_VERSION %u\n"
                "SOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH 99aabbccddeeff00\n"
                "OUTPUT 0000000000000001 %s\n"
                "OUTPUT 0000000000000002 %s\n",
                static_cast<unsigned int>(kCookStampSchema),
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
/// EXPECTATION (audit #211): retiring a stale thumbnail removes both the
/// image and its checksum sidecar so a failed regeneration can never be
/// certified into a fresh stamp; absent files count as already retired.
int check_retire_stale_thumbnail() {
  constexpr const char *kThumbPath = "cook_stamp_test_thumb.png";
  constexpr const char *kChecksumPath = "cook_stamp_test_thumb.checksum";
  static_cast<void>(std::remove(kThumbPath));
  static_cast<void>(std::remove(kChecksumPath));

  if (!write_file(kThumbPath, "old-thumbnail-bytes") ||
      !write_file(kChecksumPath, "old-checksum")) {
    return 601;
  }
  if (!retire_stale_thumbnail(kThumbPath, kChecksumPath)) {
    return 602;
  }
  if (file_exists(kThumbPath) || file_exists(kChecksumPath)) {
    return 603; // both stale files must be gone
  }
  // Absent files are already retired, not an error.
  if (!retire_stale_thumbnail(kThumbPath, kChecksumPath)) {
    return 604;
  }
  return 0;
}

/// EXPECTATION (#424): the cook key folds the cook-logic revision beside
/// the import settings, so a stamp written under one revision recooks
/// under another with identical settings; and a stamp declaring a newer
/// schema than this packer reads recooks instead of certifying outputs
/// through lines the reader cannot interpret.
int check_logic_revision_and_schema_gate_recook() {
  remove_files();
  if (!write_file(kOutputPath, "cooked-bytes")) {
    return 701;
  }

  const std::uint64_t sourceHash = 0x1122334455667788ULL;
  const std::uint64_t settingsHash = 0x99AABBCCDDEEFF00ULL;
  const std::vector<DependencyDigest> dependencies{};
  const std::vector<std::string> outputs{kOutputPath};

  const std::uint64_t keyA = cook_settings_key(settingsHash, "logic-a");
  const std::uint64_t keyB = cook_settings_key(settingsHash, "logic-b");
  if ((keyA == keyB) || (keyA == settingsHash) ||
      (cook_settings_key(settingsHash, nullptr) != settingsHash) ||
      (cook_settings_key(settingsHash, "logic-a") != keyA)) {
    return 702;
  }

  if (!write_cook_stamp(kOutputPath, nullptr,
                   sourceHash, dependencies, keyA,
                        kPlatform, outputs)) {
    remove_files();
    return 703;
  }
  if (should_repack(kOutputPath, sourceHash, dependencies, keyA, kPlatform)) {
    remove_files();
    return 704; // same settings, same revision: up to date
  }
  if (!should_repack(kOutputPath, sourceHash, dependencies, keyB, kPlatform)) {
    remove_files();
    return 705; // same settings, new revision: recook
  }

  char futureStamp[512] = {};
  std::snprintf(futureStamp, sizeof(futureStamp),
                "SCHEMA %u\n"
                "TOOL_VERSION %u\n"
                "SOURCE_HASH 1122334455667788\n"
                "IMPORT_HASH %016llx\n"
                "PLATFORM TestPlat\n"
                "OUTPUT 0000000000000001 %s\n",
                static_cast<unsigned int>(kCookStampSchema + 1U),
                static_cast<unsigned int>(kCookToolVersion),
                static_cast<unsigned long long>(keyA), kOutputPath);
  if (!write_file(kStampPath, futureStamp)) {
    remove_files();
    return 706;
  }
  if (!should_repack(kOutputPath, sourceHash, dependencies, keyA, kPlatform)) {
    remove_files();
    return 707; // a newer schema is unreadable, not partially trusted
  }

  remove_files();
  return 0;
}

int main() {
  const int contractResult = check_logic_revision_and_schema_gate_recook();
  if (contractResult != 0) {
    return contractResult;
  }
  const int retireResult = check_retire_stale_thumbnail();
  if (retireResult != 0) {
    return retireResult;
  }
  const int toolVersionResult = check_tool_version_gates_recook();
  if (toolVersionResult != 0) {
    return toolVersionResult;
  }
  const int platformResult = check_platform_tag_gates_recook();
  if (platformResult != 0) {
    return platformResult;
  }
  const int manifestResult = check_output_manifest_owns_output_set();
  if (manifestResult != 0) {
    return manifestResult;
  }
  const int relativeResult = check_stamp_paths_are_relative_to_the_stamp();
  if (relativeResult != 0) {
    return relativeResult;
  }
  const int containmentResult =
      check_manifest_never_removes_outside_the_stamp_directory();
  if (containmentResult != 0) {
    return containmentResult;
  }
  const int overlongResult = check_overlong_output_path_refuses_the_stamp();
  if (overlongResult != 0) {
    return overlongResult;
  }
  const int ruleResult = check_containment_rule_refuses_escaping_forms();
  if (ruleResult != 0) {
    return ruleResult;
  }
  return check_dependency_on_another_volume_is_stamped();
}
