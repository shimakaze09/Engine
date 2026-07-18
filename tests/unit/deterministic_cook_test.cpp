/// Deterministic cooking test (P1-M4-E1c).
/// Verifies that the asset packer produces byte-identical output for identical
/// inputs across multiple invocations. No GL context required.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/core/json.h"
#include "engine/core/mesh_asset.h"

// ---- Inline reimplementation of packer primitives for isolated testing ----

namespace {

constexpr std::uint64_t kFnv64Offset = 1469598103934665603ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

struct DependencyDigest final {
  std::string path{};
  std::uint64_t hash = 0ULL;
};

struct ImportSettings final {
  int meshIndex = 0;
  int primitiveIndex = 0;
  float scaleFactor = 1.0F;
  int upAxis = 1;
  bool generateNormals = false;
};

struct PrimitiveData final {
  std::vector<float> interleavedVertices{};
  std::vector<std::uint32_t> indices{};
  bool hasUVs = false;
};

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

// Write mesh binary to a buffer (same logic as write_mesh_file).
bool write_mesh_to_buffer(const PrimitiveData &data,
                          std::vector<unsigned char> *outBuffer) {
  if (outBuffer == nullptr) {
    return false;
  }
  outBuffer->clear();

  const std::size_t strideFloats = data.hasUVs ? 8U : 6U;
  if ((data.interleavedVertices.size() % strideFloats) != 0U) {
    return false;
  }

  const std::size_t vertexCount =
      data.interleavedVertices.size() / strideFloats;

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = data.hasUVs ? engine::core::kMeshAssetVersion2
                               : engine::core::kMeshAssetVersion;
  header.vertexCount = static_cast<std::uint32_t>(vertexCount);
  header.indexCount = static_cast<std::uint32_t>(data.indices.size());

  const auto *headerBytes = reinterpret_cast<const unsigned char *>(&header);
  outBuffer->insert(outBuffer->end(), headerBytes,
                    headerBytes + sizeof(header));

  if (!data.interleavedVertices.empty()) {
    const auto *vertBytes = reinterpret_cast<const unsigned char *>(
        data.interleavedVertices.data());
    const std::size_t vertSize =
        data.interleavedVertices.size() * sizeof(float);
    outBuffer->insert(outBuffer->end(), vertBytes, vertBytes + vertSize);
  }

  if (!data.indices.empty()) {
    const auto *idxBytes =
        reinterpret_cast<const unsigned char *>(data.indices.data());
    const std::size_t idxSize = data.indices.size() * sizeof(std::uint32_t);
    outBuffer->insert(outBuffer->end(), idxBytes, idxBytes + idxSize);
  }

  return true;
}

// Write metadata JSON to a string buffer (same logic as write_metadata_file).
std::string
write_metadata_to_string(const char *inputPath, const char *outputPath,
                         const PrimitiveData &data, std::uint64_t sourceHash,
                         const std::vector<DependencyDigest> &dependencies,
                         const ImportSettings &importSettings) {
  const std::size_t vertexCount =
      data.interleavedVertices.size() / (data.hasUVs ? 8U : 6U);
  const std::size_t indexCount = data.indices.size();

  char sourceHashText[17] = {};
  std::snprintf(sourceHashText, sizeof(sourceHashText), "%016llx",
                static_cast<unsigned long long>(sourceHash));

  engine::core::JsonWriter writer{};
  writer.begin_object();
  writer.write_uint("schemaVersion", 2U);
  writer.write_string("assetId", sourceHashText);
  writer.write_string("typeTag", "mesh");
  writer.write_string("source", inputPath);
  writer.write_string("output", outputPath);
  writer.write_string("assetFormat", "engine.mesh");
  writer.write_uint("assetFormatVersion",
                    data.hasUVs ? engine::core::kMeshAssetVersion2
                                : engine::core::kMeshAssetVersion);
  writer.write_string("sourceContentHash", sourceHashText);
  writer.write_uint64("fileSize", 0ULL);
  writer.write_uint64("vertexCount", static_cast<std::uint64_t>(vertexCount));
  writer.write_uint64("indexCount", static_cast<std::uint64_t>(indexCount));
  writer.begin_array("tags");
  writer.end_array();
  writer.begin_array("dependencies");
  for (const DependencyDigest &dep : dependencies) {
    char dependencyHashText[17] = {};
    std::snprintf(dependencyHashText, sizeof(dependencyHashText), "%016llx",
                  static_cast<unsigned long long>(dep.hash));
    writer.begin_object();
    writer.write_string("path", dep.path.c_str());
    writer.write_string("hash", dependencyHashText);
    writer.end_object();
  }
  writer.end_array();

  writer.write_key("importSettings");
  writer.begin_object();
  writer.write_uint64("meshIndex",
                      static_cast<std::uint64_t>(importSettings.meshIndex));
  writer.write_uint64(
      "primitiveIndex",
      static_cast<std::uint64_t>(importSettings.primitiveIndex));
  writer.write_float("scaleFactor", importSettings.scaleFactor);
  writer.write_uint64("upAxis",
                      static_cast<std::uint64_t>(importSettings.upAxis));
  writer.write_bool("generateNormals", importSettings.generateNormals);
  writer.write_string("interleavedLayout",
                      data.hasUVs ? "position_normal_texcoord"
                                  : "position_normal");
  writer.end_object();
  writer.end_object();

  return writer.ok() ? std::string(writer.result(), writer.result_size())
                     : std::string{};
}

// Write cookstamp to string (same logic as write_cook_stamp).
std::string
write_cookstamp_to_string(std::uint64_t sourceHash,
                          const std::vector<DependencyDigest> &dependencies,
                          std::uint64_t importSettingsHash) {
  char buf[4096] = {};
  int pos = std::snprintf(buf, sizeof(buf), "SCHEMA 2\nSOURCE_HASH %016llx\n",
                          static_cast<unsigned long long>(sourceHash));
  pos += std::snprintf(buf + pos, sizeof(buf) - static_cast<std::size_t>(pos),
                       "IMPORT_HASH %016llx\n",
                       static_cast<unsigned long long>(importSettingsHash));
  for (const auto &dep : dependencies) {
    pos += std::snprintf(buf + pos, sizeof(buf) - static_cast<std::size_t>(pos),
                         "DEP_HASH %016llx %s\n",
                         static_cast<unsigned long long>(dep.hash),
                         dep.path.c_str());
  }
  return std::string(buf, static_cast<std::size_t>(pos));
}

PrimitiveData make_test_triangle() {
  PrimitiveData data{};
  data.hasUVs = false;
  // 3 vertices: pos(3) + normal(3) = 6 floats each.
  data.interleavedVertices = {
      0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, // v0
      1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, // v1
      0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, // v2
  };
  data.indices = {0U, 1U, 2U};
  return data;
}

} // namespace

// ---- Tests ----

static int test_mesh_binary_determinism() noexcept {
  PrimitiveData data = make_test_triangle();

  std::vector<unsigned char> buf1{};
  std::vector<unsigned char> buf2{};
  if (!write_mesh_to_buffer(data, &buf1) ||
      !write_mesh_to_buffer(data, &buf2)) {
    std::fprintf(stderr, "FAIL: write_mesh_to_buffer failed\n");
    return 1;
  }

  if (buf1.size() != buf2.size()) {
    std::fprintf(stderr, "FAIL: mesh binary sizes differ: %zu vs %zu\n",
                 buf1.size(), buf2.size());
    return 1;
  }

  if (std::memcmp(buf1.data(), buf2.data(), buf1.size()) != 0) {
    std::fprintf(stderr, "FAIL: mesh binary content differs\n");
    return 1;
  }

  std::printf("PASS: mesh binary output is byte-identical\n");
  return 0;
}

static int test_metadata_determinism() noexcept {
  PrimitiveData data = make_test_triangle();
  ImportSettings settings{};

  // Dependencies in non-sorted order — sort should produce identical output.
  std::vector<DependencyDigest> deps = {{"textures/z_tex.png", 0xDEADULL},
                                        {"textures/a_tex.png", 0xBEEFULL}};

  sort_dependency_digests(deps);
  const std::string meta1 = write_metadata_to_string(
      "test.gltf", "test.mesh", data, 0x1234ULL, deps, settings);

  sort_dependency_digests(deps);
  const std::string meta2 = write_metadata_to_string(
      "test.gltf", "test.mesh", data, 0x1234ULL, deps, settings);

  if (meta1 != meta2) {
    std::fprintf(stderr, "FAIL: metadata JSON differs between runs\n");
    std::fprintf(stderr, "--- run 1 ---\n%s\n--- run 2 ---\n%s\n",
                 meta1.c_str(), meta2.c_str());
    return 1;
  }

  std::printf("PASS: metadata JSON is byte-identical\n");
  return 0;
}

static int test_metadata_json_escaping() noexcept {
  PrimitiveData data{};
  data.hasUVs = false;
  data.interleavedVertices.resize(6U, 0.0F);
  data.indices.push_back(0U);

  std::string inputPath = "meshes\\hero \"source\"\ninput.gltf";
  inputPath.push_back('\x02');
  const std::string outputPath = "out\\hero\tmesh.bin";
  std::vector<DependencyDigest> deps{{"textures\\hero \"albedo\".png", 7ULL}};
  ImportSettings settings{};

  const std::string metadata =
      write_metadata_to_string(inputPath.c_str(), outputPath.c_str(), data,
                               0x1234ULL, deps, settings);
  if (metadata.find("meshes\\\\hero \\\"source\\\"\\ninput.gltf\\u0002") ==
      std::string::npos) {
    std::fprintf(stderr, "FAIL: metadata source path was not JSON escaped\n");
    return 1;
  }
  if (metadata.find("out\\\\hero\\tmesh.bin") == std::string::npos) {
    std::fprintf(stderr, "FAIL: metadata output path was not JSON escaped\n");
    return 1;
  }
  if (metadata.find("textures\\\\hero \\\"albedo\\\".png") ==
      std::string::npos) {
    std::fprintf(stderr,
                 "FAIL: metadata dependency path was not JSON escaped\n");
    return 1;
  }

  std::printf("PASS: metadata JSON escapes paths\n");
  return 0;
}

static int test_cookstamp_determinism() noexcept {
  std::vector<DependencyDigest> deps = {{"b_dep.png", 0x2222ULL},
                                        {"a_dep.png", 0x1111ULL}};
  sort_dependency_digests(deps);
  ImportSettings settings{};
  const std::uint64_t importHash = hash_import_settings(settings);

  const std::string stamp1 =
      write_cookstamp_to_string(0xABCDULL, deps, importHash);
  const std::string stamp2 =
      write_cookstamp_to_string(0xABCDULL, deps, importHash);

  if (stamp1 != stamp2) {
    std::fprintf(stderr, "FAIL: cookstamp differs between runs\n");
    return 1;
  }

  std::printf("PASS: cookstamp is byte-identical\n");
  return 0;
}

static int test_dependency_sort_determinism() noexcept {
  // Two different initial orderings should produce identical sorted output.
  std::vector<DependencyDigest> deps1 = {
      {"c.png", 3ULL}, {"a.png", 1ULL}, {"b.png", 2ULL}};
  std::vector<DependencyDigest> deps2 = {
      {"b.png", 2ULL}, {"a.png", 1ULL}, {"c.png", 3ULL}};

  sort_dependency_digests(deps1);
  sort_dependency_digests(deps2);

  if (deps1.size() != deps2.size()) {
    std::fprintf(stderr, "FAIL: sorted sizes differ\n");
    return 1;
  }

  for (std::size_t i = 0U; i < deps1.size(); ++i) {
    if (deps1[i].path != deps2[i].path || deps1[i].hash != deps2[i].hash) {
      std::fprintf(stderr,
                   "FAIL: sorted deps differ at index %zu: %s/%llu vs "
                   "%s/%llu\n",
                   i, deps1[i].path.c_str(),
                   static_cast<unsigned long long>(deps1[i].hash),
                   deps2[i].path.c_str(),
                   static_cast<unsigned long long>(deps2[i].hash));
      return 1;
    }
  }

  std::printf("PASS: dependency sort produces deterministic order\n");
  return 0;
}

static int test_import_settings_hash_determinism() noexcept {
  ImportSettings s1{};
  s1.scaleFactor = 2.5F;
  s1.meshIndex = 1;
  const std::uint64_t h1 = hash_import_settings(s1);

  ImportSettings s2{};
  s2.scaleFactor = 2.5F;
  s2.meshIndex = 1;
  const std::uint64_t h2 = hash_import_settings(s2);

  if (h1 != h2) {
    std::fprintf(stderr, "FAIL: identical settings produce different hashes\n");
    return 1;
  }

  // Different settings should produce different hash.
  ImportSettings s3{};
  s3.scaleFactor = 3.0F;
  const std::uint64_t h3 = hash_import_settings(s3);
  if (h1 == h3) {
    std::fprintf(stderr,
                 "FAIL: different settings produced same hash (unlikely)\n");
    return 1;
  }

  std::printf("PASS: import settings hash is deterministic\n");
  return 0;
}

/// Runs this executable or test program.
int main() {
  int failures = 0;
  failures += test_mesh_binary_determinism();
  failures += test_metadata_determinism();
  failures += test_metadata_json_escaping();
  failures += test_cookstamp_determinism();
  failures += test_dependency_sort_determinism();
  failures += test_import_settings_hash_determinism();
  if (failures > 0) {
    std::fprintf(stderr, "FAILED: %d test(s) failed\n", failures);
  }
  return failures;
}
