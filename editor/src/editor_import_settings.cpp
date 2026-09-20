// Implements the import-settings sidecar cache declared in
// editor_import_settings.h.

#include "editor_import_settings.h"

#include <cstdio>
#include <cstring>

#include "engine/core/json.h"

namespace engine::editor {

namespace {

/// The one cached sidecar: the asset it belongs to and the parsed text.
struct ImportSettingsCache final {
  bool valid = false;
  char assetPath[1024] = {};
  ImportSettingsDocument document{};
  std::uint64_t reads = 0U;
};

ImportSettingsCache g_cache{};

/// Reads one optional integer field of the importSettings object.
void read_int_field(const core::JsonParser &parser,
                    const core::JsonValue &object, const char *name,
                    int *out) noexcept {
  core::JsonValue value{};
  std::uint32_t parsed = 0U;
  if (parser.get_object_field(object, name, &value) &&
      parser.as_uint(value, &parsed)) {
    *out = static_cast<int>(parsed);
  }
}

/// Reads `<assetPath>.cookmeta` into the cache's document.
void read_sidecar(const char *assetPath, ImportSettingsDocument *out) noexcept {
  *out = ImportSettingsDocument{};
  ++g_cache.reads;

  char metaPath[1024] = {};
  std::snprintf(metaPath, sizeof(metaPath), "%s.cookmeta", assetPath);
  std::FILE *metaFile = nullptr;
#ifdef _WIN32
  if (fopen_s(&metaFile, metaPath, "rb") != 0) {
    metaFile = nullptr;
  }
#else
  metaFile = std::fopen(metaPath, "rb");
#endif
  if (metaFile == nullptr) {
    out->state = ImportSettingsDocument::State::Missing;
    return;
  }
  std::fseek(metaFile, 0, SEEK_END);
  const long fileSize = std::ftell(metaFile);
  std::fseek(metaFile, 0, SEEK_SET);
  if ((fileSize <= 0) ||
      (static_cast<unsigned long>(fileSize) >
       ImportSettingsDocument::kMaxDocumentBytes)) {
    std::fclose(metaFile);
    out->state = ImportSettingsDocument::State::Unreadable;
    return;
  }
  const std::size_t readCount = std::fread(
      out->document, 1U, static_cast<std::size_t>(fileSize), metaFile);
  std::fclose(metaFile);
  out->document[readCount] = '\0';
  out->documentLength = readCount;

  core::JsonParser parser{};
  if (!parser.parse(out->document, readCount) || (parser.root() == nullptr) ||
      (parser.root()->type != core::JsonValue::Type::Object)) {
    out->state = ImportSettingsDocument::State::Malformed;
    return;
  }
  out->state = ImportSettingsDocument::State::Valid;
  core::JsonValue importObj{};
  if (!parser.get_object_field(*parser.root(), "importSettings", &importObj) ||
      (importObj.type != core::JsonValue::Type::Object)) {
    return; // defaults stand for a sidecar without import settings
  }
  read_int_field(parser, importObj, "meshIndex", &out->meshIndex);
  read_int_field(parser, importObj, "primitiveIndex", &out->primitiveIndex);
  core::JsonValue value{};
  if (parser.get_object_field(importObj, "scaleFactor", &value)) {
    parser.as_float(value, &out->scaleFactor);
  }
  read_int_field(parser, importObj, "upAxis", &out->upAxis);
  if (parser.get_object_field(importObj, "generateNormals", &value)) {
    parser.as_bool(value, &out->generateNormals);
  }
}

} // namespace

const ImportSettingsDocument *
import_settings_for_asset(const char *assetPath) noexcept {
  if ((assetPath == nullptr) || (assetPath[0] == '\0') ||
      (std::strlen(assetPath) >= sizeof(g_cache.assetPath))) {
    return nullptr;
  }
  if (g_cache.valid && (std::strcmp(g_cache.assetPath, assetPath) == 0)) {
    return &g_cache.document;
  }
  std::snprintf(g_cache.assetPath, sizeof(g_cache.assetPath), "%s", assetPath);
  read_sidecar(assetPath, &g_cache.document);
  g_cache.valid = true;
  return &g_cache.document;
}

void invalidate_import_settings_cache() noexcept { g_cache.valid = false; }

std::uint64_t import_settings_read_count() noexcept { return g_cache.reads; }

} // namespace engine::editor
