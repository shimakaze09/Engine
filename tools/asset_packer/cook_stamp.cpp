// Implements the packer's incremental cook bookkeeping: content and
// import-settings hashing, dependency digests, cook stamps, and the
// should-repack decision.

#include "packer_shared.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

/// Writes a complete text buffer to a file.
bool write_text_file(const char *path, const char *text, std::size_t textSize) {
  if ((path == nullptr) || (text == nullptr)) {
    return false;
  }

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

  const bool ok = (std::fwrite(text, 1U, textSize, file) == textSize);
  std::fclose(file);
  return ok;
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

  std::fclose(file);
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

/// Writes cook stamp data.
bool write_cook_stamp(const char *outputPath, std::uint64_t sourceHash,
                      const std::vector<DependencyDigest> &dependencies,
                      std::uint64_t importSettingsHash) {
  char stampPath[512] = {};
  if (!make_cookstamp_path(outputPath, stampPath, sizeof(stampPath))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, stampPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(stampPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fprintf(file, "SCHEMA 2\n");
  std::fprintf(file, "SOURCE_HASH %016llx\n",
               static_cast<unsigned long long>(sourceHash));
  std::fprintf(file, "IMPORT_HASH %016llx\n",
               static_cast<unsigned long long>(importSettingsHash));
  for (const DependencyDigest &dependency : dependencies) {
    std::fprintf(file, "DEP_HASH %016llx %s\n",
                 static_cast<unsigned long long>(dependency.hash),
                 dependency.path.c_str());
  }

  std::fclose(file);
  return true;
}

/// Reads cook stamp data.
bool read_cook_stamp(const char *outputPath, std::uint64_t *outSourceHash,
                     std::vector<DependencyDigest> *outDependencies,
                     std::uint64_t *outImportSettingsHash) {
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

  char line[1024] = {};
  while (std::fgets(line, static_cast<int>(sizeof(line)), file) != nullptr) {
    unsigned long long hash = 0ULL;
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

/// Returns whether should repack.
bool should_repack(const char *outputPath, std::uint64_t sourceHash,
                   const std::vector<DependencyDigest> &dependencies,
                   std::uint64_t importSettingsHash) {
  if (!file_exists(outputPath)) {
    return true;
  }

  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  std::uint64_t previousImportHash = 0ULL;
  if (!read_cook_stamp(outputPath, &previousSourceHash, &previousDependencies,
                       &previousImportHash)) {
    return true;
  }

  if (previousSourceHash != sourceHash) {
    return true;
  }

  if (previousImportHash != importSettingsHash) {
    return true;
  }

  return !dependency_digests_equal(previousDependencies, dependencies);
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

