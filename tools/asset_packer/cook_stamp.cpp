// Implements the packer's incremental cook bookkeeping: content and
// import-settings hashing, dependency digests, cook stamps, and the
// should-repack decision.

#include "packer_shared.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/atomic_file.h"
#include "engine/core/json.h"

namespace {
constexpr std::uint64_t kFnv64Offset = 14695981039346656037ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;
} // namespace

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
/// interrupted cooks cannot leave truncated outputs (audit H-20).
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

  std::uint64_t hash = kFnv64Offset;
  unsigned char buffer[4096] = {};
  while (true) {
    const std::size_t bytesRead = std::fread(buffer, 1U, sizeof(buffer), file);
    if (bytesRead == 0U) {
      break;
    }
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash ^= static_cast<std::uint64_t>(buffer[i]);
      hash *= kFnv64Prime;
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
  std::uint64_t hash = kFnv64Offset;
  auto feed = [&](const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0U; i < size; ++i) {
      hash ^= static_cast<std::uint64_t>(bytes[i]);
      hash *= kFnv64Prime;
    }
  };
  feed(&settings.meshIndex, sizeof(settings.meshIndex));
  feed(&settings.primitiveIndex, sizeof(settings.primitiveIndex));
  feed(&settings.scaleFactor, sizeof(settings.scaleFactor));
  feed(&settings.upAxis, sizeof(settings.upAxis));
  feed(&settings.generateNormals, sizeof(settings.generateNormals));
  return hash;
}

void sort_dependency_digests(std::vector<DependencyDigest> &digests) {
  std::sort(digests.begin(), digests.end(),
            [](const DependencyDigest &a, const DependencyDigest &b) {
              return a.path < b.path;
            });
}

/// Reads import settings from meta data.
bool read_import_settings_from_meta(const char *outputPath,
                                    ImportSettings *outSettings) {
  if ((outputPath == nullptr) || (outSettings == nullptr)) {
    return false;
  }

  char metadataPath[512] = {};
  const int pathResult = std::snprintf(metadataPath, sizeof(metadataPath),
                                       "%s.meta.json", outputPath);
  if ((pathResult <= 0) ||
      (pathResult >= static_cast<int>(sizeof(metadataPath)))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, metadataPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(metadataPath, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (fileSize <= 0 || fileSize > 1024 * 1024) {
    std::fclose(file);
    return false;
  }

  std::vector<char> buffer(static_cast<std::size_t>(fileSize) + 1U, '\0');
  const std::size_t readBytes =
      std::fread(buffer.data(), 1U, static_cast<std::size_t>(fileSize), file);
  std::fclose(file);
  if (readBytes != static_cast<std::size_t>(fileSize)) {
    return false;
  }

  engine::core::JsonParser parser{};
  if (!parser.parse(buffer.data(), readBytes)) {
    return false;
  }

  const engine::core::JsonValue *root = parser.root();
  if ((root == nullptr) ||
      (root->type != engine::core::JsonValue::Type::Object)) {
    return false;
  }

  const engine::core::JsonValue *importObj =
      parser.get_object_field(*root, "importSettings");
  if ((importObj == nullptr) ||
      (importObj->type != engine::core::JsonValue::Type::Object)) {
    return false;
  }

  float scaleFactor = 1.0F;
  std::uint32_t meshIndex = 0U;
  std::uint32_t primitiveIndex = 0U;
  std::uint32_t upAxis = 1U;
  bool generateNormals = false;

  const engine::core::JsonValue *scaleVal =
      parser.get_object_field(*importObj, "scaleFactor");
  if (scaleVal != nullptr) {
    parser.as_float(*scaleVal, &scaleFactor);
  }

  const engine::core::JsonValue *meshVal =
      parser.get_object_field(*importObj, "meshIndex");
  if (meshVal != nullptr) {
    parser.as_uint(*meshVal, &meshIndex);
  }

  const engine::core::JsonValue *primVal =
      parser.get_object_field(*importObj, "primitiveIndex");
  if (primVal != nullptr) {
    parser.as_uint(*primVal, &primitiveIndex);
  }

  const engine::core::JsonValue *upVal =
      parser.get_object_field(*importObj, "upAxis");
  if (upVal != nullptr) {
    parser.as_uint(*upVal, &upAxis);
  }

  const engine::core::JsonValue *normVal =
      parser.get_object_field(*importObj, "generateNormals");
  if (normVal != nullptr) {
    parser.as_bool(*normVal, &generateNormals);
  }

  outSettings->scaleFactor = scaleFactor;
  outSettings->meshIndex = static_cast<int>(meshIndex);
  outSettings->primitiveIndex = static_cast<int>(primitiveIndex);
  outSettings->upAxis = static_cast<int>(upAxis);
  outSettings->generateNormals = generateNormals;
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
bool write_cook_stamp(const char *outputPath, std::uint64_t sourceHash,
                      const std::vector<DependencyDigest> &dependencies,
                      std::uint64_t importSettingsHash,
                      const std::vector<std::string> &outputPaths) {
  char stampPath[512] = {};
  if (!make_cookstamp_path(outputPath, stampPath, sizeof(stampPath))) {
    return false;
  }

  // The stamp is the cook's commit marker (written after every output),
  // so it must itself land atomically or not at all (audit H-20).
  std::string stamp{};
  char line[1024] = {};
  std::snprintf(line, sizeof(line), "SCHEMA 3\nTOOL_VERSION %u\n",
                static_cast<unsigned int>(kCookToolVersion));
  stamp += line;
  std::snprintf(line, sizeof(line), "SOURCE_HASH %016llx\n",
                static_cast<unsigned long long>(sourceHash));
  stamp += line;
  std::snprintf(line, sizeof(line), "IMPORT_HASH %016llx\n",
                static_cast<unsigned long long>(importSettingsHash));
  stamp += line;
  for (const DependencyDigest &dependency : dependencies) {
    std::snprintf(line, sizeof(line), "DEP_HASH %016llx %s\n",
                  static_cast<unsigned long long>(dependency.hash),
                  dependency.path.c_str());
    stamp += line;
  }
  for (const std::string &producedPath : outputPaths) {
    bool hashOk = false;
    const std::uint64_t producedHash =
        hash_file_contents(producedPath.c_str(), &hashOk);
    if (!hashOk) {
      std::fprintf(stderr,
                   "error: cooked output missing at stamp time: %s\n",
                   producedPath.c_str());
      return false;
    }
    std::snprintf(line, sizeof(line), "OUTPUT %016llx %s\n",
                  static_cast<unsigned long long>(producedHash),
                  producedPath.c_str());
    stamp += line;
  }

  return engine::core::atomic_write_file(stampPath, stamp.data(),
                                         stamp.size());
}

/// Reads cook stamp data. outToolVersion reports 0 for stamps written
/// before the TOOL_VERSION key existed, which forces one recook;
/// outOutputs (nullable) receives the output manifest, empty for
/// pre-manifest stamps.
bool read_cook_stamp(const char *outputPath, std::uint64_t *outSourceHash,
                     std::vector<DependencyDigest> *outDependencies,
                     std::uint64_t *outImportSettingsHash,
                     std::uint32_t *outToolVersion,
                     std::vector<OutputRecord> *outOutputs) {
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

  char line[1024] = {};
  while (std::fgets(line, static_cast<int>(sizeof(line)), file) != nullptr) {
    unsigned long long hash = 0ULL;
    unsigned int toolVersion = 0U;
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

    char depPath[900] = {};
    if (std::sscanf(line, "DEP_HASH %llx %899[^\n]", &hash, depPath) == 2) {
      DependencyDigest dep{};
      dep.path = depPath;
      dep.hash = static_cast<std::uint64_t>(hash);
      outDependencies->push_back(dep);
      continue;
    }

    if ((std::sscanf(line, "OUTPUT %llx %899[^\n]", &hash, depPath) == 2) &&
        (outOutputs != nullptr)) {
      OutputRecord record{};
      record.path = depPath;
      record.hash = static_cast<std::uint64_t>(hash);
      outOutputs->push_back(record);
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
    if ((a[i].hash != b[i].hash) || (a[i].path != b[i].path)) {
      return false;
    }
  }

  return true;
}

/// Returns whether the output set must be recooked. A current-version
/// stamp without a manifest never certifies a cook (issue #55: legacy
/// or tampered stamps recook instead of hiding missing sidecars), and
/// every manifest-listed output must exist — verifyOutputHashes
/// additionally re-hashes each one against its recorded fingerprint.
bool should_repack(const char *outputPath, std::uint64_t sourceHash,
                   const std::vector<DependencyDigest> &dependencies,
                   std::uint64_t importSettingsHash,
                   bool verifyOutputHashes) {
  if (!file_exists(outputPath)) {
    return true;
  }

  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  std::uint64_t previousImportHash = 0ULL;
  std::uint32_t previousToolVersion = 0U;
  std::vector<OutputRecord> previousOutputs{};
  if (!read_cook_stamp(outputPath, &previousSourceHash, &previousDependencies,
                       &previousImportHash, &previousToolVersion,
                       &previousOutputs)) {
    return true;
  }

  if (previousToolVersion != kCookToolVersion) {
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
/// output set still containing stale files (issue #55). Pre-manifest
/// stamps list nothing, so their strays are out of reach here and are
/// retired by the one-time tool-version recook only going forward.
bool remove_stale_outputs(const char *outputPath,
                          const std::vector<std::string> &currentOutputs) {
  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  std::vector<OutputRecord> previousOutputs{};
  if (!read_cook_stamp(outputPath, &previousSourceHash, &previousDependencies,
                       nullptr, nullptr, &previousOutputs)) {
    return true;
  }

  for (const OutputRecord &record : previousOutputs) {
    const bool stillProduced =
        std::find(currentOutputs.begin(), currentOutputs.end(), record.path) !=
        currentOutputs.end();
    if (stillProduced) {
      continue;
    }
    std::error_code removeError{};
    const bool removed =
        std::filesystem::remove(std::filesystem::path(record.path),
                                removeError);
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
                       nullptr, nullptr, &stampOutputs) ||
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
                          &siblingOutputs)) {
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

  static constexpr const char *kSweptSuffixes[] = {".anim", ".skel", ".hull",
                                                   ".meta.json"};
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
    for (const char *suffix : kSweptSuffixes) {
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
        (std::strcmp(matchedSuffix, ".meta.json") == 0)) {
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

std::uint64_t hash_path_to_asset_id(const char *path) {
  if (path == nullptr) {
    return 0ULL;
  }

  std::uint64_t hash = kFnv64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(path);
       *cursor != 0U; ++cursor) {
    const unsigned char ch = (*cursor == static_cast<unsigned char>('\\'))
                                 ? static_cast<unsigned char>('/')
                                 : *cursor;
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnv64Prime;
  }

  if (hash == 0ULL) {
    hash = 1ULL;
  }
  return hash;
}

