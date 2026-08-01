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

constexpr std::uint64_t kFnv64Offset = 14695981039346656037ULL;
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

bool json_parses(const std::string &json) noexcept {
  engine::core::JsonParser parser{};
  return parser.parse(json.c_str(), json.size());
}

std::string nested_array_json(std::size_t depth) {
  std::string json(depth, '[');
  json += '0';
  json.append(depth, ']');
  return json;
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

  ImportSettings modified = base;
  modified.scaleFactor = 2.0F;
  const std::uint64_t modifiedHash = hash_import_settings(modified);

  if (baseHash == modifiedHash) {
    std::fprintf(stderr, "FAIL: changing scaleFactor did not change hash\n");
    return 1;
  }

  ImportSettings modified2 = base;
  modified2.meshIndex = 5;
  const std::uint64_t modified2Hash = hash_import_settings(modified2);

  if (baseHash == modified2Hash) {
    std::fprintf(stderr, "FAIL: changing meshIndex did not change hash\n");
    return 1;
  }

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

static int test_json_parser_rejects_invalid_strings() noexcept {
  if (!json_parses("{\"s\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\"}")) {
    std::fprintf(stderr, "FAIL: valid JSON escapes were rejected\n");
    return 1;
  }

  if (json_parses("{\"s\":\"bad\\q\"}")) {
    std::fprintf(stderr, "FAIL: invalid JSON escape was accepted\n");
    return 1;
  }
  if (json_parses("{\"s\":\"bad\\u12G4\"}")) {
    std::fprintf(stderr, "FAIL: invalid unicode escape was accepted\n");
    return 1;
  }

  std::string rawControl = "{\"s\":\"bad";
  rawControl.push_back('\x01');
  rawControl += "\"}";
  if (json_parses(rawControl)) {
    std::fprintf(stderr, "FAIL: raw control character was accepted\n");
    return 1;
  }

  std::printf("PASS: JSON parser rejects invalid string tokens\n");
  return 0;
}

static int test_json_parser_rejects_excessive_depth() noexcept {
  if (!json_parses(nested_array_json(16U))) {
    std::fprintf(stderr, "FAIL: reasonable JSON nesting was rejected\n");
    return 1;
  }
  if (json_parses(nested_array_json(140U))) {
    std::fprintf(stderr, "FAIL: excessive JSON nesting was accepted\n");
    return 1;
  }

  std::printf("PASS: JSON parser enforces nesting depth\n");
  return 0;
}

/// Realistic meta document with schema, mappings, and an unknown
/// forward-compatible field, as the H-21 preservation fixture.
constexpr const char *kMetaFixture =
    "{\n"
    "  \"schemaVersion\": 3,\n"
    "  \"source\": \"props/crate.gltf\",\n"
    "  \"outputs\": { \"mesh\": \"props/crate.mesh\", "
    "\"importSettings\": \"nested-decoy\" },\n"
    "  \"futureField\": [1, 2, {\"deep\": \"}\\\"{\"}],\n"
    "  \"importSettings\": { \"meshIndex\": 0, \"scaleFactor\": 1.0 },\n"
    "  \"tags\": [\"prop\", \"importSettings\"]\n"
    "}\n";

/// EXPECTATION (audit H-21): replacing the top-level importSettings value
/// preserves every other field byte-for-byte — schema, mappings, the
/// unknown forward-compatible field, a nested decoy key, and a string
/// containing the key text all survive, and the result parses with the
/// new settings readable.
int test_json_splice_preserves_unknown_fields() {
  char output[4096] = {};
  std::size_t outputLength = 0U;
  const char *newValue = "{ \"meshIndex\": 7, \"generateNormals\": true }";
  if (!engine::core::json_replace_top_level_field(
          kMetaFixture, std::strlen(kMetaFixture), "importSettings", newValue,
          output, sizeof(output), &outputLength)) {
    std::fprintf(stderr, "FAIL: splice rejected the fixture\n");
    return 1;
  }

  if ((std::strstr(output, "\"schemaVersion\": 3") == nullptr) ||
      (std::strstr(output, "\"source\": \"props/crate.gltf\"") == nullptr) ||
      (std::strstr(output, "\"importSettings\": \"nested-decoy\"") ==
       nullptr) ||
      (std::strstr(output, "{\"deep\": \"}\\\"{\"}") == nullptr) ||
      (std::strstr(output, "\"tags\": [\"prop\", \"importSettings\"]") ==
       nullptr)) {
    std::fprintf(stderr, "FAIL: splice destroyed a preserved field\n");
    return 1;
  }
  if (std::strstr(output, newValue) == nullptr) {
    std::fprintf(stderr, "FAIL: splice did not install the new value\n");
    return 1;
  }
  if (std::strstr(output, "\"scaleFactor\": 1.0") != nullptr) {
    std::fprintf(stderr, "FAIL: old settings value survived the splice\n");
    return 1;
  }

  engine::core::JsonParser parser{};
  if (!parser.parse(output, outputLength) || (parser.root() == nullptr)) {
    std::fprintf(stderr, "FAIL: spliced document does not parse\n");
    return 1;
  }
  const engine::core::JsonValue *settings =
      parser.get_object_field(*parser.root(), "importSettings");
  std::uint32_t meshIndex = 0U;
  const engine::core::JsonValue *meshIndexValue =
      (settings != nullptr) ? parser.get_object_field(*settings, "meshIndex")
                            : nullptr;
  if ((meshIndexValue == nullptr) ||
      !parser.as_uint(*meshIndexValue, &meshIndex) || (meshIndex != 7U)) {
    std::fprintf(stderr, "FAIL: new settings not readable after splice\n");
    return 1;
  }

  std::printf("PASS: splice preserves unknown fields\n");
  return 0;
}

/// EXPECTATION (audit H-21): a document without the field gains it at the
/// top level (nested decoys and string occurrences are not matches), an
/// empty object insert stays valid, and malformed documents, non-object
/// roots, and undersized output buffers are rejected.
int test_json_splice_insert_and_rejection() {
  const char *withoutField =
      "{\n  \"schemaVersion\": 3,\n"
      "  \"outputs\": { \"importSettings\": \"nested-decoy\" },\n"
      "  \"note\": \"importSettings\"\n}\n";
  char output[2048] = {};
  std::size_t outputLength = 0U;
  if (!engine::core::json_replace_top_level_field(
          withoutField, std::strlen(withoutField), "importSettings",
          "{ \"meshIndex\": 2 }", output, sizeof(output), &outputLength)) {
    std::fprintf(stderr, "FAIL: insert rejected a valid document\n");
    return 1;
  }
  engine::core::JsonParser parser{};
  if (!parser.parse(output, outputLength) || (parser.root() == nullptr) ||
      (parser.get_object_field(*parser.root(), "importSettings") ==
       nullptr) ||
      (std::strstr(output, "\"importSettings\": \"nested-decoy\"") ==
       nullptr) ||
      (std::strstr(output, "\"note\": \"importSettings\"") == nullptr)) {
    std::fprintf(stderr, "FAIL: insert result wrong or decoys touched\n");
    return 1;
  }

  if (!engine::core::json_replace_top_level_field(
          "{}", 2U, "importSettings", "{ \"meshIndex\": 1 }", output,
          sizeof(output), &outputLength)) {
    std::fprintf(stderr, "FAIL: empty-object insert rejected\n");
    return 1;
  }
  engine::core::JsonParser emptyParser{};
  if (!emptyParser.parse(output, outputLength)) {
    std::fprintf(stderr, "FAIL: empty-object insert does not parse\n");
    return 1;
  }

  const char *truncated = "{ \"a\": { \"b\": 1 }";
  if (engine::core::json_replace_top_level_field(
          truncated, std::strlen(truncated), "a", "2", output, sizeof(output),
          &outputLength)) {
    std::fprintf(stderr, "FAIL: malformed document accepted\n");
    return 1;
  }
  const char *arrayRoot = "[1, 2, 3]";
  if (engine::core::json_replace_top_level_field(
          arrayRoot, std::strlen(arrayRoot), "a", "2", output, sizeof(output),
          &outputLength)) {
    std::fprintf(stderr, "FAIL: non-object root accepted\n");
    return 1;
  }
  char tiny[8] = {};
  if (engine::core::json_replace_top_level_field(
          kMetaFixture, std::strlen(kMetaFixture), "importSettings",
          "{ \"meshIndex\": 7 }", tiny, sizeof(tiny), &outputLength)) {
    std::fprintf(stderr, "FAIL: undersized buffer accepted\n");
    return 1;
  }

  std::printf("PASS: splice insert and rejection paths\n");
  return 0;
}

/// Runs this executable or test program.
int main() {
  int failures = 0;
  failures += test_import_settings_roundtrip_default();
  failures += test_import_settings_roundtrip_custom();
  failures += test_import_settings_hash_change();
  failures += test_import_settings_hash_roundtrip();
  failures += test_json_parser_rejects_invalid_strings();
  failures += test_json_parser_rejects_excessive_depth();
  failures += test_json_splice_preserves_unknown_fields();
  failures += test_json_splice_insert_and_rejection();
  if (failures > 0) {
    std::fprintf(stderr, "FAILED: %d test(s) failed\n", failures);
  }
  return failures;
}
