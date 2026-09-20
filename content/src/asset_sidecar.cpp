// Implements the authored source-side sidecar's path, reader and writer.
// The document is JSON with one field per line so two branches that both
// imported assets resolve their conflict per field rather than per file.

#include "engine/content/asset_sidecar.h"

#include <cstdio>
#include <cstring>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"

namespace engine::content {
namespace {

constexpr const char *kLogChannel = "assets";
constexpr const char *kSidecarSuffix = ".meta";

/// Reports a sidecar the reader could not use, naming the file so the
/// author can go and look at it.
void log_sidecar_problem(const char *path, const char *problem) noexcept {
  char message[512] = {};
  std::snprintf(message, sizeof(message), "asset sidecar: %s: %s", path,
                problem);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

} // namespace

bool asset_sidecar_path(const char *assetOsPath, char *out,
                        std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((assetOsPath == nullptr) || (assetOsPath[0] == '\0')) {
    return false;
  }
  const int written =
      std::snprintf(out, capacity, "%s%s", assetOsPath, kSidecarSuffix);
  if ((written < 0) || (static_cast<std::size_t>(written) >= capacity)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

SidecarReadResult read_asset_sidecar(const char *assetOsPath,
                                     AssetSidecar *out) noexcept {
  if (out == nullptr) {
    return SidecarReadResult::Malformed;
  }
  char path[1024] = {};
  if (!asset_sidecar_path(assetOsPath, path, sizeof(path))) {
    return SidecarReadResult::Malformed;
  }

  // The buffer is static-sized rather than allocated: this runs on the
  // editor's cold scan over every asset in the project.
  static char buffer[kMaxAssetSidecarBytes] = {};
  std::size_t size = 0U;
  switch (core::read_whole_file(path, buffer, sizeof(buffer), &size)) {
  case core::FileReadResult::Ok:
    break;
  case core::FileReadResult::Absent:
    return SidecarReadResult::Absent;
  case core::FileReadResult::Unreadable:
    log_sidecar_problem(path, "exists but could not be read; the identity on "
                              "disk is left untouched");
    return SidecarReadResult::Unreadable;
  case core::FileReadResult::TooLarge:
    log_sidecar_problem(path, "is larger than a sidecar can be; the identity "
                              "on disk is left untouched");
    return SidecarReadResult::Unreadable;
  }

  core::JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    log_sidecar_problem(path, "is not valid JSON");
    return SidecarReadResult::Malformed;
  }
  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    log_sidecar_problem(path, "is not a JSON object");
    return SidecarReadResult::Malformed;
  }

  const core::JsonValue *versionValue =
      parser.get_object_field(*root, "schemaVersion");
  std::uint32_t version = 0U;
  if ((versionValue == nullptr) ||
      !parser.as_uint(*versionValue, &version)) {
    log_sidecar_problem(path, "has no readable schemaVersion");
    return SidecarReadResult::Malformed;
  }
  if (version != kAssetSidecarSchemaVersion) {
    // A reader that meets an unknown version refuses the document rather
    // than guessing which fields still mean what they used to.
    char problem[128] = {};
    std::snprintf(problem, sizeof(problem),
                  "has schema version %u, and this build reads %u", version,
                  kAssetSidecarSchemaVersion);
    log_sidecar_problem(path, problem);
    return SidecarReadResult::Malformed;
  }

  const core::JsonValue *guidValue = parser.get_object_field(*root, "guid");
  char guidText[kAssetGuidTextLength + 1U] = {};
  if ((guidValue == nullptr) ||
      !parser.copy_string_strict(*guidValue, guidText, sizeof(guidText))) {
    log_sidecar_problem(path, "has no guid field");
    return SidecarReadResult::Malformed;
  }
  AssetGuid guid{};
  if (!parse_asset_guid(guidText, &guid)) {
    log_sidecar_problem(path, "has a guid that is not canonical UUID text");
    return SidecarReadResult::Malformed;
  }
  if (!asset_guid_is_valid(guid)) {
    log_sidecar_problem(path, "has the nil guid, which names no asset");
    return SidecarReadResult::Malformed;
  }

  // Absent means "not a folder": the flag is only written where it is
  // true, so an ordinary asset's sidecar stays two fields long.
  bool folder = false;
  const core::JsonValue *folderValue = parser.get_object_field(*root, "folder");
  if (folderValue != nullptr) {
    if (!parser.as_bool(*folderValue, &folder)) {
      log_sidecar_problem(path, "has a folder field that is not a boolean");
      return SidecarReadResult::Malformed;
    }
  }

  out->schemaVersion = version;
  out->guid = guid;
  out->folder = folder;
  return SidecarReadResult::Ok;
}

bool write_asset_sidecar(const char *assetOsPath,
                         const AssetSidecar &sidecar) noexcept {
  char path[1024] = {};
  if (!asset_sidecar_path(assetOsPath, path, sizeof(path))) {
    return false;
  }
  if (!asset_guid_is_valid(sidecar.guid)) {
    log_sidecar_problem(path, "was not written: a sidecar must carry a real "
                              "identity, and this one is nil");
    return false;
  }
  char guidText[kAssetGuidTextLength + 1U] = {};
  if (!format_asset_guid(sidecar.guid, guidText, sizeof(guidText))) {
    return false;
  }

  // Hand-built rather than routed through JsonWriter: the whole point of
  // the layout is one field per line, so a merge between two branches
  // that both imported assets resolves per field.
  char document[512] = {};
  int written = 0;
  if (sidecar.folder) {
    written = std::snprintf(document, sizeof(document),
                            "{\n  \"schemaVersion\": %u,\n  \"guid\": "
                            "\"%s\",\n  \"folder\": true\n}\n",
                            kAssetSidecarSchemaVersion, guidText);
  } else {
    written = std::snprintf(
        document, sizeof(document),
        "{\n  \"schemaVersion\": %u,\n  \"guid\": \"%s\"\n}\n",
        kAssetSidecarSchemaVersion, guidText);
  }
  if ((written <= 0) || (static_cast<std::size_t>(written) >= sizeof(document))) {
    return false;
  }

  if (!core::atomic_write_file(path, document,
                               static_cast<std::size_t>(written))) {
    log_sidecar_problem(path, "could not be written; the previous identity is "
                              "left as it was");
    return false;
  }
  return true;
}

} // namespace engine::content
