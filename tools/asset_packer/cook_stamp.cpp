// Implements the packer's incremental cook bookkeeping: content and
// import-settings hashing, dependency digests, cook stamps, and the
// should-repack decision.

#include "packer_shared.h"

#include "engine/content/asset_metadata.h"
#include "engine/content/asset_identity.h"
#include "engine/content/asset_sidecar.h"
#include "engine/content/cook_contract.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/atomic_file.h"
#include "engine/core/hash.h"
#include "engine/core/json.h"
#include "engine/content/asset_type_table.h"

bool file_exists(const char *path) {
  if (path == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fclose(file);
  return true;
}

/// Writes a complete text buffer through a staged atomic replacement so
/// interrupted cooks cannot leave truncated outputs.
bool write_text_file(const char *path, const char *text, std::size_t textSize) {
  if ((path == nullptr) || (text == nullptr)) {
    return false;
  }
  return engine::core::atomic_write_file(path, text, textSize);
}

void format_hex_u64(std::uint64_t value, char (&out)[17]) noexcept {
  std::snprintf(out, 17U, "%016llx", static_cast<unsigned long long>(value));
}

std::uint64_t hash_file_contents(const char *path, bool *ok) {
  if (ok != nullptr) {
    *ok = false;
  }

  if (path == nullptr) {
    return 0ULL;
  }
  // Only regular files are fingerprinted: a device or FIFO named by a
  // stamp or sidecar would otherwise be read until it ends, which a
  // device never does.
  std::error_code statusError{};
  if (!std::filesystem::is_regular_file(std::filesystem::path(path),
                                        statusError) ||
      statusError) {
    return 0ULL;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return 0ULL;
  }

  std::uint64_t hash = engine::core::kFnv1a64Offset;
  unsigned char buffer[4096] = {};
  while (true) {
    const std::size_t bytesRead = std::fread(buffer, 1U, sizeof(buffer), file);
    if (bytesRead == 0U) {
      break;
    }
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash = engine::core::fnv1a_64_append(hash, buffer[i]);
    }
  }

  const bool readFailed = std::ferror(file) != 0;
  const bool closeFailed = std::fclose(file) != 0;
  if (readFailed || closeFailed) {
    return 0ULL;
  }
  if (ok != nullptr) {
    *ok = true;
  }
  return hash;
}

namespace {

/// The directory the stamp of `outputPath` lives in ("." for a bare name).
std::filesystem::path stamp_directory(const char *outputPath) {
  const std::filesystem::path parent =
      std::filesystem::path(outputPath).parent_path();
  return parent.empty() ? std::filesystem::path(".") : parent;
}

/// One absolute, lexically normalized, `/`-separated spelling of an OS
/// path so two invocations naming the same file compare equal whatever
/// form each used.
std::string normalized_os_path(const std::string &path) {
  std::error_code ec{};
  const std::filesystem::path absolute =
      std::filesystem::absolute(std::filesystem::path(path), ec);
  if (ec) {
    return std::filesystem::path(path).lexically_normal().generic_string();
  }
  return absolute.lexically_normal().generic_string();
}

/// Rewrites an invoked OS path relative to the stamp's directory, the
/// form a schema-4 stamp records; empty when it cannot be
/// expressed.
std::string stamp_relative_path(const char *outputPath,
                                const std::string &path) {
  std::error_code ec{};
  const std::filesystem::path base =
      std::filesystem::absolute(stamp_directory(outputPath), ec)
          .lexically_normal();
  if (ec) {
    return std::string{};
  }
  const std::filesystem::path target =
      std::filesystem::absolute(std::filesystem::path(path), ec)
          .lexically_normal();
  if (ec) {
    return std::string{};
  }
  return target.lexically_relative(base).generic_string();
}

/// Joins a schema-4 recorded path back under the stamp's directory.
std::string stamp_joined_path(const char *outputPath,
                              const std::string &recorded) {
  const std::filesystem::path base = stamp_directory(outputPath);
  if (base == std::filesystem::path(".")) {
    return std::filesystem::path(recorded).lexically_normal().string();
  }
  return (base / std::filesystem::path(recorded)).lexically_normal().string();
}

/// Appends one stamp line, refusing anything the readers would not take
/// back whole.
bool append_stamp_line(std::string *stamp, const std::string &line) {
  if ((line.size() + 1U) > kMaxCookStampLineBytes) {
    std::fprintf(stderr,
                 "error: cook stamp line exceeds %zu bytes; the path is "
                 "too long to certify: %s\n",
                 kMaxCookStampLineBytes, line.c_str());
    return false;
  }
  *stamp += line;
  *stamp += '\n';
  return true;
}

} // namespace

/// Builds the requested runtime data for dependency digests.
bool build_dependency_digests(const std::vector<std::string> &dependencyPaths,
                              std::vector<DependencyDigest> *outDigests) {
  if (outDigests == nullptr) {
    return false;
  }

  outDigests->clear();
  outDigests->reserve(dependencyPaths.size());
  for (const std::string &path : dependencyPaths) {
    bool ok = false;
    const std::uint64_t hash = hash_file_contents(path.c_str(), &ok);
    if (!ok) {
      std::fprintf(stderr, "error: dependency missing or unreadable: %s\n",
                   path.c_str());
      return false;
    }
    DependencyDigest digest{};
    digest.path = path;
    digest.hash = hash;
    outDigests->push_back(digest);
  }

  return true;
}

std::uint64_t hash_import_settings(const ImportSettings &settings) {
  std::uint64_t hash = engine::core::kFnv1a64Offset;
  auto feed = [&](const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0U; i < size; ++i) {
      hash = engine::core::fnv1a_64_append(hash, bytes[i]);
    }
  };
  feed(&settings.meshIndex, sizeof(settings.meshIndex));
  feed(&settings.primitiveIndex, sizeof(settings.primitiveIndex));
  feed(&settings.scaleFactor, sizeof(settings.scaleFactor));
  feed(&settings.upAxis, sizeof(settings.upAxis));
  feed(&settings.generateNormals, sizeof(settings.generateNormals));
  return hash;
}

std::uint64_t cook_settings_key(std::uint64_t importSettingsHash,
                                const char *logicRevision) {
  std::uint64_t hash = importSettingsHash;
  if (logicRevision == nullptr) {
    return hash;
  }
  // The terminator is fed too, so "a" + "b" and "ab" cannot collide.
  const std::size_t length = std::strlen(logicRevision) + 1U;
  for (std::size_t i = 0U; i < length; ++i) {
    hash = engine::core::fnv1a_64_append(
        hash, static_cast<unsigned char>(logicRevision[i]));
  }
  return hash;
}

void sort_dependency_digests(std::vector<DependencyDigest> &digests) {
  std::sort(digests.begin(), digests.end(),
            [](const DependencyDigest &a, const DependencyDigest &b) {
              return a.path < b.path;
            });
}

/// Reads the authored import settings out of the source's sidecar.
bool read_authored_import_settings(const char *sourcePath,
                                   ImportSettings *outSettings) {
  if ((sourcePath == nullptr) || (outSettings == nullptr)) {
    return false;
  }
  engine::content::AssetSidecar sidecar{};
  if (engine::content::read_asset_sidecar(sourcePath, &sidecar) !=
      engine::content::SidecarReadResult::Ok) {
    return false;
  }
  if (!sidecar.hasMeshImport) {
    return false;
  }
  *outSettings = sidecar.meshImport;
  return true;
}

bool make_cookstamp_path(const char *outputPath, char *outPath,
                         std::size_t outPathSize) {
  if ((outputPath == nullptr) || (outPath == nullptr) || (outPathSize == 0U)) {
    return false;
  }
  const int written =
      std::snprintf(outPath, outPathSize, "%s.cookstamp", outputPath);
  return (written > 0) && (written < static_cast<int>(outPathSize));
}

/// Writes cook stamp data including the output manifest: every listed
/// output is re-hashed from its committed bytes, so an unreadable
/// output blocks the commit marker instead of being certified.
bool is_valid_platform_tag(const char *platformTag) {
  if ((platformTag == nullptr) || (platformTag[0] == '\0')) {
    return false;
  }
  for (const char *c = platformTag; *c != '\0'; ++c) {
    if ((*c == ' ') || (*c == '\t') || (*c == '\n') || (*c == '\r')) {
      return false;
    }
  }
  return std::strlen(platformTag) < 64U;
}

bool write_cook_stamp(const char *outputPath, const char *sourcePath,
                      std::uint64_t sourceHash,
                      const std::vector<DependencyDigest> &dependencies,
                      std::uint64_t importSettingsHash,
                      const char *platformTag,
                      const std::vector<std::string> &outputPaths) {
  char stampPath[512] = {};
  if (!make_cookstamp_path(outputPath, stampPath, sizeof(stampPath))) {
    return false;
  }
  if (!is_valid_platform_tag(platformTag)) {
    std::fprintf(stderr, "error: invalid cook platform tag\n");
    return false;
  }

  // The stamp is the cook's commit marker (written after every output),
  // so it must itself land atomically or not at all.
  // Schema 4 records every path relative to the stamp's own directory,
  // so the stamp certifies the same files from any working directory,
  // and an output must live inside that directory.
  std::string stamp{};
  char line[128] = {};
  std::snprintf(line, sizeof(line), "SCHEMA %u\nTOOL_VERSION %u\n",
                static_cast<unsigned int>(kCookStampSchema),
                static_cast<unsigned int>(kCookToolVersion));
  stamp += line;
  std::snprintf(line, sizeof(line), "SOURCE_HASH %016llx\n",
                static_cast<unsigned long long>(sourceHash));
  stamp += line;
  std::snprintf(line, sizeof(line), "IMPORT_HASH %016llx\n",
                static_cast<unsigned long long>(importSettingsHash));
  stamp += line;
  std::snprintf(line, sizeof(line), "PLATFORM %s\n", platformTag);
  stamp += line;

  // The producing source's identity, so the catalog reads a cooked
  // output's provenance instead of inferring it from the filename.
  // A source with no sidecar has no identity to record; the cook still
  // runs, and the catalog reports the output as unidentified.
  engine::content::AssetSidecar sidecar{};
  const bool haveSourceGuid =
      (sourcePath != nullptr) &&
      (engine::content::read_asset_sidecar(sourcePath, &sidecar) ==
       engine::content::SidecarReadResult::Ok);
  std::string sourceStem{};
  if (haveSourceGuid) {
    char guidText[engine::content::kAssetGuidTextLength + 1U] = {};
    if (!engine::content::format_asset_guid(sidecar.guid, guidText,
                                            sizeof(guidText))) {
      return false;
    }
    if (!append_stamp_line(&stamp, std::string("SOURCE_GUID ") + guidText)) {
      return false;
    }
    sourceStem = std::filesystem::path(sourcePath).stem().string();
  }
  for (const DependencyDigest &dependency : dependencies) {
    // A dependency on another volume has no path relative to the stamp:
    // Windows drives share no root, so a project on one drive cooking
    // against sources or an SDK on another could not write a stamp at
    // all. Such a dependency is recorded by its normalized absolute path.
    // That keeps what a dependency line has to guarantee — it names the
    // same file from any working directory — and needs no containment,
    // because a dependency is only ever read and hashed, never removed.
    // The reader's join already yields an absolute operand unchanged.
    // Outputs stay strict below: they are what retirement deletes.
    std::string relative = stamp_relative_path(outputPath, dependency.path);
    if (relative.empty()) {
      relative = normalized_os_path(dependency.path);
    }
    if (relative.empty()) {
      std::fprintf(stderr,
                   "error: dependency path cannot be recorded relative to "
                   "the cook stamp: %s\n",
                   dependency.path.c_str());
      return false;
    }
    char hashText[17] = {};
    format_hex_u64(dependency.hash, hashText);
    if (!append_stamp_line(&stamp, std::string("DEP_HASH ") + hashText +
                                       " " + relative)) {
      return false;
    }
  }
  for (const std::string &producedPath : outputPaths) {
    const std::string relative = stamp_relative_path(outputPath, producedPath);
    if (!engine::content::cook_stamp_path_is_contained(relative.c_str())) {
      std::fprintf(stderr,
                   "error: cooked output lies outside the cook stamp's "
                   "directory and cannot be certified: %s\n",
                   producedPath.c_str());
      return false;
    }
    bool hashOk = false;
    const std::uint64_t producedHash =
        hash_file_contents(producedPath.c_str(), &hashOk);
    if (!hashOk) {
      std::fprintf(stderr,
                   "error: cooked output missing or not a regular file at "
                   "stamp time: %s\n",
                   producedPath.c_str());
      return false;
    }
    char hashText[17] = {};
    format_hex_u64(producedHash, hashText);
    if (!append_stamp_line(&stamp, std::string("OUTPUT ") + hashText + " " +
                                       relative)) {
      return false;
    }

    // An output that is a runtime asset form gets its local id recorded
    // beside it. Derived from the name here, at cook time, where the
    // source is known for certain; the catalog never re-derives it.
    if (!haveSourceGuid) {
      continue;
    }
    const engine::content::AssetClassification outputKind =
        engine::content::classify_asset_path(relative.c_str());
    if ((outputKind.tag == engine::content::AssetTypeTag::Unknown) ||
        outputKind.source) {
      continue;
    }
    const std::string outputName =
        std::filesystem::path(relative).filename().string();
    if ((outputName.size() <= (sourceStem.size() + 1U)) ||
        (outputName.compare(0U, sourceStem.size(), sourceStem) != 0) ||
        (outputName[sourceStem.size()] != '.')) {
      continue;
    }
    char localText[17] = {};
    format_hex_u64(engine::content::asset_local_id(
                       outputName.substr(sourceStem.size() + 1U).c_str()),
                   localText);
    if (!append_stamp_line(&stamp, std::string("ASSET ") + localText + " " +
                                       relative)) {
      return false;
    }
  }

  return engine::core::atomic_write_file(stampPath, stamp.data(),
                                         stamp.size());
}

/// Reads cook stamp data. outToolVersion reports 0 for stamps written
/// before the TOOL_VERSION key existed, which forces one recook;
/// outOutputs (nullable) receives the output manifest, empty for
/// pre-manifest stamps; outPlatformTag (nullable) reads empty for
/// stamps written before the platform tag joined the cook key.
bool read_cook_stamp(const char *outputPath, std::uint64_t *outSourceHash,
                     std::vector<DependencyDigest> *outDependencies,
                     std::uint64_t *outImportSettingsHash,
                     std::uint32_t *outToolVersion,
                     std::vector<OutputRecord> *outOutputs,
                     std::string *outPlatformTag, std::uint32_t *outSchema) {
  if ((outSourceHash == nullptr) || (outDependencies == nullptr)) {
    return false;
  }

  char stampPath[512] = {};
  if (!make_cookstamp_path(outputPath, stampPath, sizeof(stampPath))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, stampPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(stampPath, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  *outSourceHash = 0ULL;
  outDependencies->clear();
  if (outImportSettingsHash != nullptr) {
    *outImportSettingsHash = 0ULL;
  }
  if (outToolVersion != nullptr) {
    *outToolVersion = 0U;
  }
  if (outOutputs != nullptr) {
    outOutputs->clear();
  }
  if (outPlatformTag != nullptr) {
    outPlatformTag->clear();
  }

  // Schema 4 paths are stamp-relative and OUTPUT paths contained; a
  // legacy stamp's paths are its invocation paths, kept verbatim only so
  // the tool-version gate can recook it — they are never retired.
  std::uint32_t schema = 0U;
  if (outSchema != nullptr) {
    *outSchema = 0U;
  }
  char line[kMaxCookStampLineBytes] = {};
  while (std::fgets(line, static_cast<int>(sizeof(line)), file) != nullptr) {
    const std::size_t lineLength = std::strlen(line);
    if ((lineLength > 0U) && (line[lineLength - 1U] != '\n') &&
        (std::feof(file) == 0)) {
      // Longer than the writer emits: a truncated path would name a
      // different file, so the stamp is corrupt and recooks.
      std::fclose(file);
      return false;
    }
    unsigned long long hash = 0ULL;
    unsigned int toolVersion = 0U;
    unsigned int declaredSchema = 0U;
    if (std::sscanf(line, "SCHEMA %u", &declaredSchema) == 1) {
      // Any schema but this build's is unreadable rather than partially
      // trusted, and the caller recooks to get a stamp it can read back.
      // Newer is obvious: its lines have meanings this reader does not
      // know. Older matters just as much now that the stamp carries the
      // provenance the catalog resolves cooked outputs through — an
      // older stamp simply does not have it, and accepting one would
      // leave those outputs unidentified with nothing to say why.
      if (declaredSchema != kCookStampSchema) {
        std::fclose(file);
        return false;
      }
      schema = declaredSchema;
      if (outSchema != nullptr) {
        *outSchema = schema;
      }
      continue;
    }
    if (std::sscanf(line, "TOOL_VERSION %u", &toolVersion) == 1) {
      if (outToolVersion != nullptr) {
        *outToolVersion = static_cast<std::uint32_t>(toolVersion);
      }
      continue;
    }
    if (std::sscanf(line, "SOURCE_HASH %llx", &hash) == 1) {
      *outSourceHash = static_cast<std::uint64_t>(hash);
      continue;
    }

    if (std::sscanf(line, "IMPORT_HASH %llx", &hash) == 1) {
      if (outImportSettingsHash != nullptr) {
        *outImportSettingsHash = static_cast<std::uint64_t>(hash);
      }
      continue;
    }

    char platformTag[64] = {};
    if (std::sscanf(line, "PLATFORM %63s", platformTag) == 1) {
      if (outPlatformTag != nullptr) {
        *outPlatformTag = platformTag;
      }
      continue;
    }

    char depPath[kMaxCookStampLineBytes] = {};
    if (std::sscanf(line, "DEP_HASH %llx %1023[^\n]", &hash, depPath) == 2) {
      DependencyDigest dep{};
      dep.path = (schema >= 4U) ? stamp_joined_path(outputPath, depPath)
                                : std::string(depPath);
      dep.hash = static_cast<std::uint64_t>(hash);
      outDependencies->push_back(dep);
      continue;
    }

    if (std::sscanf(line, "OUTPUT %llx %1023[^\n]", &hash, depPath) == 2) {
      if ((schema >= 4U) &&
          !engine::content::cook_stamp_path_is_contained(depPath)) {
        // An output outside the stamp's directory was never written by
        // this packer: corrupt, so nothing it names is trusted or removed.
        std::fclose(file);
        return false;
      }
      if (outOutputs != nullptr) {
        OutputRecord record{};
        record.path = (schema >= 4U) ? stamp_joined_path(outputPath, depPath)
                                     : std::string(depPath);
        record.hash = static_cast<std::uint64_t>(hash);
        outOutputs->push_back(record);
      }
    }
  }

  std::fclose(file);
  return true;
}

bool dependency_digests_equal(const std::vector<DependencyDigest> &a,
                              const std::vector<DependencyDigest> &b) {
  if (a.size() != b.size()) {
    return false;
  }

  for (std::size_t i = 0U; i < a.size(); ++i) {
    if ((a[i].hash != b[i].hash) ||
        (normalized_os_path(a[i].path) != normalized_os_path(b[i].path))) {
      return false;
    }
  }

  return true;
}

/// Returns whether the output set must be recooked. A current-version
/// stamp without a manifest never certifies a cook (legacy or tampered
/// stamps recook instead of hiding missing sidecars), and
/// every manifest-listed output must exist — verifyOutputHashes
/// additionally re-hashes each one against its recorded fingerprint.
bool should_repack(const char *outputPath, std::uint64_t sourceHash,
                   const std::vector<DependencyDigest> &dependencies,
                   std::uint64_t importSettingsHash, const char *platformTag,
                   bool verifyOutputHashes) {
  if (!file_exists(outputPath)) {
    return true;
  }

  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  std::uint64_t previousImportHash = 0ULL;
  std::uint32_t previousToolVersion = 0U;
  std::vector<OutputRecord> previousOutputs{};
  std::string previousPlatformTag{};
  if (!read_cook_stamp(outputPath, &previousSourceHash, &previousDependencies,
                       &previousImportHash, &previousToolVersion,
                       &previousOutputs, &previousPlatformTag, nullptr)) {
    return true;
  }

  if (previousToolVersion != kCookToolVersion) {
    return true;
  }

  if ((platformTag == nullptr) || (previousPlatformTag != platformTag)) {
    return true;
  }

  if (previousSourceHash != sourceHash) {
    return true;
  }

  if (previousImportHash != importSettingsHash) {
    return true;
  }

  if (previousOutputs.empty()) {
    return true;
  }

  for (const OutputRecord &record : previousOutputs) {
    if (!file_exists(record.path.c_str())) {
      return true;
    }
    if (verifyOutputHashes) {
      bool hashOk = false;
      const std::uint64_t currentHash =
          hash_file_contents(record.path.c_str(), &hashOk);
      if (!hashOk || (currentHash != record.hash)) {
        return true;
      }
    }
  }

  return !dependency_digests_equal(previousDependencies, dependencies);
}

/// Retires previous-manifest outputs the current cook no longer
/// produces, after the new outputs committed and before the new stamp:
/// a failed deletion must block the stamp so it can never certify an
/// output set still containing stale files. Pre-manifest
/// stamps list nothing, so their strays are out of reach here and are
/// retired by the one-time tool-version recook only going forward.
bool remove_stale_outputs(const char *outputPath,
                          const std::vector<std::string> &currentOutputs) {
  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  std::vector<OutputRecord> previousOutputs{};
  std::uint32_t previousSchema = 0U;
  if (!read_cook_stamp(outputPath, &previousSourceHash, &previousDependencies,
                       nullptr, nullptr, &previousOutputs, nullptr,
                       &previousSchema)) {
    return true;
  }
  if (previousSchema < 4U) {
    // A legacy manifest's paths were whatever the old invocation named,
    // with no containment; retiring them could delete anything.
    // The tool-version recook re-certifies the set; strays are the
    // orphan sweep's, which only reaches same-base siblings.
    std::printf("legacy cook stamp: stale outputs not retired: %s\n",
                outputPath);
    return true;
  }

  const std::filesystem::path stampDir =
      std::filesystem::absolute(stamp_directory(outputPath)).lexically_normal();
  std::vector<std::string> currentNormalized{};
  currentNormalized.reserve(currentOutputs.size());
  for (const std::string &current : currentOutputs) {
    currentNormalized.push_back(normalized_os_path(current));
  }

  for (const OutputRecord &record : previousOutputs) {
    const std::string normalized = normalized_os_path(record.path);
    const bool stillProduced =
        std::find(currentNormalized.begin(), currentNormalized.end(),
                  normalized) != currentNormalized.end();
    if (stillProduced) {
      continue;
    }
    // The reader already refused an escaping path; this re-checks the
    // joined result so no removal can ever reach outside the stamp's
    // directory, and never removes anything but a regular file.
    const std::filesystem::path candidate(normalized);
    std::string stampDirText = stampDir.generic_string();
    while (!stampDirText.empty() && (stampDirText.back() == '/')) {
      stampDirText.pop_back();
    }
    stampDirText += "/";
    if (normalized.compare(0U, stampDirText.size(), stampDirText) != 0) {
      std::fprintf(stderr,
                   "error: stale cooked output outside the stamp "
                   "directory; not removed: %s\n",
                   record.path.c_str());
      return false;
    }
    std::error_code kindError{};
    if (std::filesystem::exists(candidate, kindError) && !kindError &&
        !std::filesystem::is_regular_file(candidate, kindError)) {
      std::fprintf(stderr,
                   "error: stale cooked output is not a regular file; not "
                   "removed: %s\n",
                   record.path.c_str());
      return false;
    }
    std::error_code removeError{};
    const bool removed =
        std::filesystem::remove(candidate, removeError);
    if (removeError) {
      std::fprintf(stderr, "error: failed to remove stale cooked output: %s\n",
                   record.path.c_str());
      return false;
    }
    if (removed) {
      std::printf("removed stale cooked output: %s\n", record.path.c_str());
    }
  }

  return true;
}

bool retire_stale_thumbnail(const char *thumbPath, const char *checksumPath) {
  bool ok = true;
  const char *paths[2] = {thumbPath, checksumPath};
  for (const char *path : paths) {
    if ((path == nullptr) || (path[0] == '\0')) {
      continue;
    }
    std::error_code removeError{};
    static_cast<void>(
        std::filesystem::remove(std::filesystem::path(path), removeError));
    if (removeError) {
      std::fprintf(stderr,
                   "error: failed to retire stale thumbnail output: %s\n",
                   path);
      ok = false;
    }
  }
  return ok;
}

namespace {

/// Filename component of a manifest or disk path (either separator).
std::string manifest_entry_filename(const std::string &path) {
  const std::size_t separator = path.find_last_of("/\\");
  return (separator == std::string::npos) ? path : path.substr(separator + 1U);
}

/// Whether the name ends with the given suffix.
bool name_ends_with(const std::string &name, const char *suffix) {
  const std::size_t suffixLength = std::strlen(suffix);
  return (name.size() >= suffixLength) &&
         (name.compare(name.size() - suffixLength, suffixLength, suffix) == 0);
}

} // namespace

/// Deletes same-base cooked sidecars no cookstamp manifest or sibling .mesh accounts for.
bool sweep_orphan_outputs(const char *outputPath) {
  if (outputPath == nullptr) {
    return false;
  }

  std::uint64_t stampSourceHash = 0ULL;
  std::vector<DependencyDigest> stampDependencies{};
  std::vector<OutputRecord> stampOutputs{};
  if (!read_cook_stamp(outputPath, &stampSourceHash, &stampDependencies,
                       nullptr, nullptr, &stampOutputs, nullptr, nullptr) ||
      stampOutputs.empty()) {
    std::fprintf(stderr,
                 "error: orphan sweep needs a manifest-bearing cook stamp: "
                 "%s.cookstamp\n",
                 outputPath);
    return false;
  }

  const std::filesystem::path cookedPath(outputPath);
  std::filesystem::path directory = cookedPath.parent_path();
  if (directory.empty()) {
    directory = ".";
  }

  std::string baseName = cookedPath.filename().string();
  const std::size_t baseDot = baseName.rfind('.');
  if (baseDot != std::string::npos) {
    baseName.resize(baseDot);
  }
  const std::string basePrefix = baseName + ".";

  const std::string outputFilename = cookedPath.filename().string();
  std::vector<std::string> protectedNames{};
  std::vector<std::string> siblingMeshNames{};
  std::error_code scanError{};
  for (std::filesystem::directory_iterator
           entry(directory, scanError),
       end{};
       !scanError && (entry != end); entry.increment(scanError)) {
    const std::string entryName = entry->path().filename().string();
    if (name_ends_with(entryName, ".cookstamp")) {
      const std::string ownerPath =
          (directory / entryName.substr(0U, entryName.size() - 10U)).string();
      std::uint64_t siblingSourceHash = 0ULL;
      std::vector<DependencyDigest> siblingDependencies{};
      std::vector<OutputRecord> siblingOutputs{};
      if (read_cook_stamp(ownerPath.c_str(), &siblingSourceHash,
                          &siblingDependencies, nullptr, nullptr,
                          &siblingOutputs, nullptr, nullptr)) {
        for (const OutputRecord &record : siblingOutputs) {
          protectedNames.push_back(manifest_entry_filename(record.path));
        }
      }
    }
    if (name_ends_with(entryName, ".mesh") && (entryName != outputFilename)) {
      siblingMeshNames.push_back(entryName);
    }
  }
  if (scanError) {
    std::fprintf(stderr, "error: orphan sweep failed to scan directory: %s\n",
                 directory.string().c_str());
    return false;
  }

  // Everything a mesh cook leaves beside its output: the derived asset
  // types' cooked forms (from the type table) plus the cook's own sidecars.
  std::vector<const char *> sweptSuffixes = {".hull", ".cookmeta"};
  for (std::size_t i = 0U; i < engine::content::kAssetTypeCount; ++i) {
    const engine::content::AssetTypeDescriptor &row =
        engine::content::asset_type_descriptor(
            static_cast<engine::content::AssetTypeTag>(i));
    if (row.policy != engine::content::AssetSourcePolicy::Derived) {
      continue;
    }
    for (std::size_t j = 0U; j < row.cookedSuffixCount; ++j) {
      sweptSuffixes.push_back(row.cookedSuffixes[j]);
    }
  }
  std::vector<std::string> orphanNames{};
  scanError.clear();
  for (std::filesystem::directory_iterator
           entry(directory, scanError),
       end{};
       !scanError && (entry != end); entry.increment(scanError)) {
    const std::string entryName = entry->path().filename().string();
    if (entryName.compare(0U, basePrefix.size(), basePrefix) != 0) {
      continue;
    }
    const char *matchedSuffix = nullptr;
    for (const char *suffix : sweptSuffixes) {
      if (name_ends_with(entryName, suffix)) {
        matchedSuffix = suffix;
        break;
      }
    }
    if (matchedSuffix == nullptr) {
      continue;
    }
    if (std::find(protectedNames.begin(), protectedNames.end(), entryName) !=
        protectedNames.end()) {
      continue;
    }
    const std::string stem =
        entryName.substr(0U, entryName.size() - std::strlen(matchedSuffix));
    std::string owningMesh{};
    if ((std::strcmp(matchedSuffix, ".hull") == 0) ||
        (std::strcmp(matchedSuffix, ".cookmeta") == 0)) {
      owningMesh = stem;
    } else if (std::strcmp(matchedSuffix, ".skel") == 0) {
      owningMesh = stem + ".mesh";
    } else {
      const std::size_t clipDot = stem.rfind('.');
      if (clipDot != std::string::npos) {
        owningMesh = stem.substr(0U, clipDot) + ".mesh";
      }
    }
    if (!owningMesh.empty() &&
        (std::find(siblingMeshNames.begin(), siblingMeshNames.end(),
                   owningMesh) != siblingMeshNames.end())) {
      continue;
    }
    orphanNames.push_back(entryName);
  }
  if (scanError) {
    std::fprintf(stderr, "error: orphan sweep failed to scan directory: %s\n",
                 directory.string().c_str());
    return false;
  }

  std::sort(orphanNames.begin(), orphanNames.end());
  for (const std::string &orphanName : orphanNames) {
    const std::filesystem::path orphanPath = directory / orphanName;
    std::error_code removeError{};
    if (!std::filesystem::remove(orphanPath, removeError) || removeError) {
      std::fprintf(stderr, "error: failed to remove orphan cooked output: %s\n",
                   orphanPath.string().c_str());
      return false;
    }
    std::printf("removed orphan cooked output: %s\n",
                orphanPath.string().c_str());
  }

  return true;
}

// Cooked ids must agree byte-for-byte with the runtime's — one shared
// implementation instead of a drifting duplicate.
std::uint64_t hash_path_to_asset_id(const char *path) {
  return engine::content::make_asset_id_from_path(path);
}

