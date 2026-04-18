/// Import settings round-trip test (P1-M4-E2c).
/// Verifies that import settings survive JSON write/read and that changing
/// settings produces a different hash, triggering a recook.

#include "engine/core/json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ---- Inline copies of packer types/helpers for isolated testing ----

namespace {

constexpr std::uint64_t kFnv64Offset = 1469598103934665603ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

struct ImportSettings final {
  int meshIndex = 0;
  int primitiveIndex = 0;
  float scaleFactor = 1.0F;
  int upAxis = 1;
  bool generateNormals = false;
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

/// Write import settings into a JSON string (same format as
/// write_metadata_file).
std::string write_import_settings_json(const ImportSettings &settings) {
  char buf[1024] = {};
  const int n =
      std::snprintf(buf, sizeof(buf),
                    "{\n"
                    "  \"importSettings\": {\n"
                    "    \"meshIndex\": %d,\n"
                    "    \"primitiveIndex\": %d,\n"
                    "    \"scaleFactor\": %.6g,\n"
                    "    \"upAxis\": %d,\n"
                    "    \"generateNormals\": %s,\n"
                    "    \"interleavedLayout\": \"position_normal\"\n"
                    "  }\n"
                    "}\n",
                    settings.meshIndex, settings.primitiveIndex,
                    static_cast<double>(settings.scaleFactor), settings.upAxis,
                    settings.generateNormals ? "true" : "false");
  return std::string(buf, static_cast<std::size_t>(n));
}

/// Read import settings from a JSON string using the engine JsonParser.
ImportSettings read_import_settings_from_json(const std::string &jsonStr) {
  ImportSettings out{};
  engine::core::JsonParser parser{};
  if (!parser.parse(jsonStr.c_str(), jsonStr.size())) {
    return out;
  }
  const engine::core::JsonValue *root = parser.root();
  if ((root == nullptr) ||
      (root->type != engine::core::JsonValue::Type::Object)) {
    return out;
  }
  const engine::core::JsonValue *importObj =
      parser.get_object_field(*root, "importSettings");
  if ((importObj == nullptr) ||
      (importObj->type != engine::core::JsonValue::Type::Object)) {
    return out;
  }

  {
    const engine::core::JsonValue *v =
        parser.get_object_field(*importObj, "meshIndex");
    if (v != nullptr) {
      std::uint32_t tmp = 0U;
      if (parser.as_uint(*v, &tmp)) {
        out.meshIndex = static_cast<int>(tmp);
      }
    }
  }
  {
    const engine::core::JsonValue *v =
        parser.get_object_field(*importObj, "primitiveIndex");
    if (v != nullptr) {
      std::uint32_t tmp = 0U;
      if (parser.as_uint(*v, &tmp)) {
        out.primitiveIndex = static_cast<int>(tmp);
      }
    }
  }
  {
    const engine::core::JsonValue *v =
        parser.get_object_field(*importObj, "scaleFactor");
    if (v != nullptr) {
      parser.as_float(*v, &out.scaleFactor);
    }
  }
  {
    const engine::core::JsonValue *v =
        parser.get_object_field(*importObj, "upAxis");
    if (v != nullptr) {
      std::uint32_t tmp = 1U;
      if (parser.as_uint(*v, &tmp)) {
        out.upAxis = static_cast<int>(tmp);
      }
    }
  }
  {
    const engine::core::JsonValue *v =
        parser.get_object_field(*importObj, "generateNormals");
    if (v != nullptr) {
      parser.as_bool(*v, &out.generateNormals);
    }
  }
  return out;
}

} // namespace

// ---- Tests ----

/// Round-trip: write default settings to JSON, read back, verify identical.
static int test_import_settings_roundtrip_default() noexcept {
  ImportSettings original{};
  const std::string json = write_import_settings_json(original);
  const ImportSettings parsed = read_import_settings_from_json(json);

  if (parsed.meshIndex != original.meshIndex ||
      parsed.primitiveIndex != original.primitiveIndex ||
      parsed.upAxis != original.upAxis ||
      parsed.generateNormals != original.generateNormals) {
    std::fprintf(stderr, "FAIL: default settings mismatch after round-trip\n");
    return 1;
  }

  if (std::fabs(static_cast<double>(parsed.scaleFactor) -
                static_cast<double>(original.scaleFactor)) > 1e-6) {
    std::fprintf(stderr, "FAIL: scaleFactor mismatch: %f vs %f\n",
                 static_cast<double>(parsed.scaleFactor),
                 static_cast<double>(original.scaleFactor));
    return 1;
  }

  std::printf("PASS: default import settings survive round-trip\n");
  return 0;
}

/// Round-trip: write non-default settings, read back, verify identical.
static int test_import_settings_roundtrip_custom() noexcept {
  ImportSettings original{};
  original.meshIndex = 3;
  original.primitiveIndex = 2;
  original.scaleFactor = 0.01F;
  original.upAxis = 2;
  original.generateNormals = true;

  const std::string json = write_import_settings_json(original);
  const ImportSettings parsed = read_import_settings_from_json(json);

  if (parsed.meshIndex != 3 || parsed.primitiveIndex != 2 ||
      parsed.upAxis != 2 || !parsed.generateNormals) {
    std::fprintf(stderr, "FAIL: custom settings mismatch after round-trip\n");
    return 1;
  }

  if (std::fabs(static_cast<double>(parsed.scaleFactor) - 0.01) > 1e-6) {
    std::fprintf(stderr, "FAIL: scaleFactor mismatch: %f vs 0.01\n",
                 static_cast<double>(parsed.scaleFactor));
    return 1;
  }

  std::printf("PASS: custom import settings survive round-trip\n");
  return 0;
}

/// Changing a setting produces a different hash.
static int test_import_settings_hash_change() noexcept {
  ImportSettings base{};
  const std::uint64_t baseHash = hash_import_settings(base);

  // Change scaleFactor.
  ImportSettings modified = base;
  modified.scaleFactor = 2.0F;
  const std::uint64_t modifiedHash = hash_import_settings(modified);

  if (baseHash == modifiedHash) {
    std::fprintf(stderr, "FAIL: changing scaleFactor did not change hash\n");
    return 1;
  }

  // Change meshIndex.
  ImportSettings modified2 = base;
  modified2.meshIndex = 5;
  const std::uint64_t modified2Hash = hash_import_settings(modified2);

  if (baseHash == modified2Hash) {
    std::fprintf(stderr, "FAIL: changing meshIndex did not change hash\n");
    return 1;
  }

  // Change generateNormals.
  ImportSettings modified3 = base;
  modified3.generateNormals = true;
  const std::uint64_t modified3Hash = hash_import_settings(modified3);

  if (baseHash == modified3Hash) {
    std::fprintf(stderr,
                 "FAIL: changing generateNormals did not change hash\n");
    return 1;
  }

  std::printf("PASS: changing import settings produces different hash\n");
  return 0;
}

/// Round-trip hash: write settings, read back, hash must match original.
static int test_import_settings_hash_roundtrip() noexcept {
  ImportSettings original{};
  original.scaleFactor = 3.14F;
  original.meshIndex = 7;
  original.generateNormals = true;

  const std::uint64_t hashBefore = hash_import_settings(original);

  const std::string json = write_import_settings_json(original);
  const ImportSettings parsed = read_import_settings_from_json(json);
  const std::uint64_t hashAfter = hash_import_settings(parsed);

  if (hashBefore != hashAfter) {
    std::fprintf(stderr,
                 "FAIL: hash changed after round-trip: %016llx vs %016llx\n",
                 static_cast<unsigned long long>(hashBefore),
                 static_cast<unsigned long long>(hashAfter));
    return 1;
  }

  std::printf("PASS: import settings hash survives JSON round-trip\n");
  return 0;
}

int main() {
  int failures = 0;
  failures += test_import_settings_roundtrip_default();
  failures += test_import_settings_roundtrip_custom();
  failures += test_import_settings_hash_change();
  failures += test_import_settings_hash_roundtrip();
  if (failures > 0) {
    std::fprintf(stderr, "FAILED: %d test(s) failed\n", failures);
  }
  return failures;
}
