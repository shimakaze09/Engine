// Implements the authored source-side sidecar's path, reader and writer.
// The document is JSON with one field per line so two branches that both
// imported assets resolve their conflict per field rather than per file.

#include "engine/content/asset_sidecar.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/content/asset_import_settings.h"
#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"

namespace engine::content {
namespace {

constexpr const char *kLogChannel = "assets";
constexpr const char *kSidecarSuffix = ".meta";

/// Reads one optional field of an object, leaving `*out` at its default
/// when the field is absent; false only when it is present and will not
/// read as the expected type.
bool read_int_field(const core::JsonParser &parser,
                    const core::JsonValue &object, const char *name,
                    std::int32_t *out) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, name, &field)) {
    return true;
  }
  std::int64_t wide = 0;
  if (!parser.as_int64(field, &wide)) {
    return false;
  }
  if ((wide < INT32_MIN) || (wide > INT32_MAX)) {
    return false;
  }
  *out = static_cast<std::int32_t>(wide);
  return true;
}

bool read_float_field(const core::JsonParser &parser,
                      const core::JsonValue &object, const char *name,
                      float *out) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, name, &field)) {
    return true;
  }
  return parser.as_float(field, out);
}

bool read_bool_field(const core::JsonParser &parser,
                     const core::JsonValue &object, const char *name,
                     bool *out) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, name, &field)) {
    return true;
  }
  return parser.as_bool(field, out);
}

/// Reads one optional string field that must be one of `names` (indexed
/// by the enum's value), leaving `*out` at its default when absent; false
/// when it is present and is not one of them.
template <typename Enum, std::size_t Count>
bool read_enum_field(const core::JsonParser &parser,
                     const core::JsonValue &object, const char *name,
                     const char *const (&names)[Count], Enum *out) noexcept {
  core::JsonValue field{};
  if (!parser.get_object_field(object, name, &field)) {
    return true;
  }
  char text[16] = {};
  if (!parser.copy_string_strict(field, text, sizeof(text))) {
    return false;
  }
  for (std::size_t i = 0U; i < Count; ++i) {
    if (std::strcmp(text, names[i]) == 0) {
      *out = static_cast<Enum>(i);
      return true;
    }
  }
  return false;
}

constexpr const char *kColorSpaceNames[] = {"auto", "srgb", "linear"};
constexpr const char *kFilterNames[] = {"linear", "nearest"};
constexpr const char *kWrapNames[] = {"repeat", "clamp"};

/// The settings kind of the asset a sidecar at `assetOsPath` belongs to.
ImportSettingsKind settings_kind_of(const char *assetOsPath) noexcept {
  return (assetOsPath != nullptr)
             ? import_settings_kind(classify_asset_path(assetOsPath).tag)
             : ImportSettingsKind::None;
}

/// Reports a sidecar the reader could not use, naming the file so the
/// author can go and look at it.
void log_sidecar_problem(const char *path, const char *problem) noexcept {
  if (path == nullptr) {
    return; // the writer reading back what it replaces stays quiet
  }
  char message[512] = {};
  std::snprintf(message, sizeof(message), "asset sidecar: %s: %s", path,
                problem);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

/// Unknown-key warnings logged this process; a newer build's key sits in
/// every sidecar, so past the first few the scan says so once.
std::size_t g_unknownKeyReports = 0U;
constexpr std::size_t kUnknownKeyReportsLogged = 16U;

struct UnknownKeyContext final {
  const char *path = nullptr;
};

void report_unknown_sidecar_key(const char *key, void *userData) noexcept {
  const auto *context = static_cast<const UnknownKeyContext *>(userData);
  ++g_unknownKeyReports;
  if (g_unknownKeyReports > kUnknownKeyReportsLogged) {
    if (g_unknownKeyReports == kUnknownKeyReportsLogged + 1U) {
      core::log_message(core::LogLevel::Warning, kLogChannel,
                        "asset sidecar: further keys this build does not "
                        "read are not listed");
    }
    return;
  }
  char message[640] = {};
  std::snprintf(message, sizeof(message),
                "asset sidecar: %s: key '%s' is not read by this build; a "
                "top-level key is kept when this build rewrites the sidecar",
                context->path, key);
  core::log_message(core::LogLevel::Warning, kLogChannel, message);
}

/// Names each member of a sidecar that no lookup read, as the scene loader
/// names a scene's: a newer build's key, or a typo.
void report_unknown_sidecar_keys(
    const char *path, const core::JsonValue &root,
    const core::JsonReadTracker &tracker) noexcept {
  UnknownKeyContext context{};
  context.path = path;
  static_cast<void>(core::json_visit_unread_members(
      root, tracker, &report_unknown_sidecar_key, &context));
}

} // namespace

bool is_asset_sidecar_path(const char *path) noexcept {
  if (path == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(path);
  const std::size_t suffixLength = std::strlen(kSidecarSuffix);
  return (length > suffixLength) &&
         (std::strcmp(path + (length - suffixLength), kSidecarSuffix) == 0);
}

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

namespace {

/// Reads a sidecar's text, already loaded from `path` (null: report
/// nothing), into `*out` (left untouched for every result but Ok). With a
/// tracker, every member the reader looks up is recorded in it, so the members
/// it never looked up are the ones this build does not know.
SidecarReadResult parse_sidecar(const char *path, ImportSettingsKind kind,
                                const char *text, std::size_t size,
                                core::JsonParser &parser,
                                core::JsonReadTracker *tracker,
                                AssetSidecar *out) noexcept {
  if (!parser.parse(text, size)) {
    log_sidecar_problem(path, "is not valid JSON");
    return SidecarReadResult::Malformed;
  }
  if ((tracker != nullptr) && tracker->reset_for(text, size)) {
    parser.set_read_tracker(tracker);
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

  // Import settings are optional: a source with none cooks, or loads, at
  // the defaults. Present-but-malformed is refused rather than defaulted,
  // because silently using the defaults would throw away what the author
  // typed and look like it worked. The block is read as the settings of
  // the asset's own type.
  MeshImportSettings meshImport{};
  bool hasMeshImport = false;
  TextureImportSettings textureImport{};
  bool hasTextureImport = false;
  AudioImportSettings audioImport{};
  bool hasAudioImport = false;
  const core::JsonValue *settings =
      parser.get_object_field(*root, "importSettings");
  if (settings != nullptr) {
    if (settings->type != core::JsonValue::Type::Object) {
      log_sidecar_problem(path, "has an importSettings that is not an object");
      return SidecarReadResult::Malformed;
    }
    const core::JsonValue settingsValue = *settings;
    // The block's own version: absent is 1, and one newer than this build
    // reads is refused, as the sidecar's schema version is.
    std::int32_t blockVersion = 1;
    const std::uint32_t newestVersion =
        (kind == ImportSettingsKind::Mesh)      ? kMeshImportSettingsVersion
        : (kind == ImportSettingsKind::Texture) ? kTextureImportSettingsVersion
        : (kind == ImportSettingsKind::Audio)   ? kAudioImportSettingsVersion
                                                : 0U;
    if (!read_int_field(parser, settingsValue, "version", &blockVersion) ||
        (blockVersion < 1)) {
      log_sidecar_problem(path, "has an importSettings version that is not "
                                "a positive integer");
      return SidecarReadResult::Malformed;
    }
    bool read = false;
    switch (kind) {
    case ImportSettingsKind::None:
      log_sidecar_problem(path, "has importSettings, but its asset type has "
                                "no import settings");
      return SidecarReadResult::Malformed;
    case ImportSettingsKind::Mesh:
      read =
          read_int_field(parser, settingsValue, "meshIndex",
                         &meshImport.meshIndex) &&
          read_int_field(parser, settingsValue, "primitiveIndex",
                         &meshImport.primitiveIndex) &&
          read_int_field(parser, settingsValue, "upAxis", &meshImport.upAxis) &&
          read_float_field(parser, settingsValue, "scaleFactor",
                           &meshImport.scaleFactor) &&
          read_bool_field(parser, settingsValue, "generateNormals",
                          &meshImport.generateNormals);
      hasMeshImport = true;
      break;
    case ImportSettingsKind::Texture:
      read = read_enum_field(parser, settingsValue, "colorSpace",
                             kColorSpaceNames, &textureImport.colorSpace) &&
             read_bool_field(parser, settingsValue, "generateMips",
                             &textureImport.generateMips) &&
             read_enum_field(parser, settingsValue, "filter", kFilterNames,
                             &textureImport.filter) &&
             read_enum_field(parser, settingsValue, "wrap", kWrapNames,
                             &textureImport.wrap);
      hasTextureImport = true;
      break;
    case ImportSettingsKind::Audio: {
      std::int32_t sampleRate = 0;
      read = read_int_field(parser, settingsValue, "sampleRate", &sampleRate) &&
             read_bool_field(parser, settingsValue, "forceMono",
                             &audioImport.forceMono) &&
             // 0 keeps the file's rate; anything else is a rate a sound
             // can be resampled to.
             ((sampleRate == 0) ||
              ((sampleRate >=
                static_cast<std::int32_t>(kMinAudioImportSampleRate)) &&
               (sampleRate <=
                static_cast<std::int32_t>(kMaxAudioImportSampleRate))));
      audioImport.sampleRate =
          static_cast<std::uint32_t>((sampleRate > 0) ? sampleRate : 0);
      hasAudioImport = true;
      break;
    }
    }
    if (static_cast<std::uint32_t>(blockVersion) > newestVersion) {
      char problem[160] = {};
      std::snprintf(problem, sizeof(problem),
                    "has importSettings version %d, and this build reads %u",
                    static_cast<int>(blockVersion), newestVersion);
      log_sidecar_problem(path, problem);
      return SidecarReadResult::Malformed;
    }
    if (!read) {
      log_sidecar_problem(path, "has an importSettings field that will not "
                                "read; the settings are not guessed at");
      return SidecarReadResult::Malformed;
    }
  }

  // Labels are optional. Every one must be a valid label and distinct, and
  // there may be no more than the catalog holds: a label that does not fit
  // is refused, never shortened, because a shortened label is another one.
  AssetLabels labels{};
  const core::JsonValue *labelsValue = parser.get_object_field(*root, "labels");
  if (labelsValue != nullptr) {
    if (labelsValue->type != core::JsonValue::Type::Array) {
      log_sidecar_problem(path, "has a labels field that is not an array");
      return SidecarReadResult::Malformed;
    }
    const std::size_t count = parser.array_size(*labelsValue);
    if (count > AssetMetadata::kMaxTags) {
      log_sidecar_problem(path, "has more labels than an asset can carry");
      return SidecarReadResult::Malformed;
    }
    for (std::size_t i = 0U; i < count; ++i) {
      const core::JsonValue *element =
          parser.get_array_element(*labelsValue, i);
      char label[AssetMetadata::kMaxTagLength] = {};
      if ((element == nullptr) ||
          !parser.copy_string_strict(*element, label, sizeof(label)) ||
          !asset_label_is_valid(label) || asset_labels_has(labels, label) ||
          !asset_labels_add(&labels, label)) {
        log_sidecar_problem(path, "has a label that is not a distinct label "
                                  "of letters, digits, '_', '-' or '.' up to "
                                  "31 bytes");
        return SidecarReadResult::Malformed;
      }
    }
  }

  out->schemaVersion = version;
  out->guid = guid;
  out->folder = folder;
  out->hasMeshImport = hasMeshImport;
  out->meshImport = meshImport;
  out->hasTextureImport = hasTextureImport;
  out->textureImport = textureImport;
  out->hasAudioImport = hasAudioImport;
  out->audioImport = audioImport;
  out->labels = labels;
  return SidecarReadResult::Ok;
}

} // namespace

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
  core::JsonReadTracker tracker{};
  const SidecarReadResult result = parse_sidecar(
      path, settings_kind_of(assetOsPath), buffer, size, parser, &tracker, out);
  if (result == SidecarReadResult::Ok) {
    report_unknown_sidecar_keys(path, *parser.root(), tracker);
  }
  return result;
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
  // Room for a whole sidecar: every known field at its widest plus the
  // members a newer build wrote, carried below. Static, as the reader's
  // buffer is: sidecars are written on the editor's and packer's single
  // thread.
  static char document[kMaxAssetSidecarBytes] = {};
  int written = std::snprintf(
      document, sizeof(document),
      "{\n  \"schemaVersion\": %u,\n  \"guid\": \"%s\"",
      kAssetSidecarSchemaVersion, guidText);
  if (written <= 0) {
    return false;
  }
  auto append = [&](const char *format, auto... args) noexcept {
    if (written < 0) {
      return;
    }
    const int more =
        std::snprintf(document + written,
                      sizeof(document) - static_cast<std::size_t>(written),
                      format, args...);
    if ((more < 0) || (static_cast<std::size_t>(written + more) >=
                       sizeof(document))) {
      written = -1;
      return;
    }
    written += more;
  };
  if (sidecar.folder) {
    append("%s", ",\n  \"folder\": true");
  }
  const ImportSettingsKind kind = settings_kind_of(assetOsPath);
  if ((sidecar.hasMeshImport && (kind != ImportSettingsKind::Mesh)) ||
      (sidecar.hasTextureImport && (kind != ImportSettingsKind::Texture)) ||
      (sidecar.hasAudioImport && (kind != ImportSettingsKind::Audio))) {
    log_sidecar_problem(path, "was not written: it carries import settings "
                              "for another type of asset");
    return false;
  }
  if (sidecar.hasMeshImport) {
    // One field per line here too: a merge between two branches that each
    // tuned one setting resolves to both edits rather than one winning.
    append(",\n  \"importSettings\": {"
           "\n    \"meshIndex\": %d,"
           "\n    \"primitiveIndex\": %d,"
           "\n    \"scaleFactor\": %.9g,"
           "\n    \"upAxis\": %d,"
           "\n    \"generateNormals\": %s"
           "\n  }",
           static_cast<int>(sidecar.meshImport.meshIndex),
           static_cast<int>(sidecar.meshImport.primitiveIndex),
           static_cast<double>(sidecar.meshImport.scaleFactor),
           static_cast<int>(sidecar.meshImport.upAxis),
           sidecar.meshImport.generateNormals ? "true" : "false");
  }
  if (sidecar.hasTextureImport) {
    const TextureImportSettings &texture = sidecar.textureImport;
    append(",\n  \"importSettings\": {"
           "\n    \"colorSpace\": \"%s\","
           "\n    \"generateMips\": %s,"
           "\n    \"filter\": \"%s\","
           "\n    \"wrap\": \"%s\""
           "\n  }",
           kColorSpaceNames[static_cast<std::size_t>(texture.colorSpace)],
           texture.generateMips ? "true" : "false",
           kFilterNames[static_cast<std::size_t>(texture.filter)],
           kWrapNames[static_cast<std::size_t>(texture.wrap)]);
  }
  if (sidecar.hasAudioImport) {
    append(",\n  \"importSettings\": {"
           "\n    \"sampleRate\": %u,"
           "\n    \"forceMono\": %s"
           "\n  }",
           static_cast<unsigned>(sidecar.audioImport.sampleRate),
           sidecar.audioImport.forceMono ? "true" : "false");
  }
  if (sidecar.labels.count > 0U) {
    // One label per line, so labels two branches added both survive a merge.
    append("%s", ",\n  \"labels\": [");
    for (std::size_t i = 0U; i < sidecar.labels.count; ++i) {
      const char *label = sidecar.labels.names[i].data();
      if (!asset_label_is_valid(label)) {
        log_sidecar_problem(path, "was not written: it carries a label that "
                                  "is not a valid label");
        return false;
      }
      append("\n    \"%s\"%s", label,
             (i + 1U < sidecar.labels.count) ? "," : "");
    }
    append("%s", "\n  ]");
  }
  // Members this build does not read are carried from the sidecar on
  // disk, so an older build relabelling an asset never erases what a
  // newer one wrote there, as Unity keeps fields its types do not know.
  static char previous[kMaxAssetSidecarBytes] = {};
  std::size_t previousSize = 0U;
  if (core::read_whole_file(path, previous, sizeof(previous), &previousSize) ==
      core::FileReadResult::Ok) {
    core::JsonParser parser{};
    core::JsonReadTracker tracker{};
    AssetSidecar ignored{};
    if (parse_sidecar(nullptr, kind, previous, previousSize, parser, &tracker,
                      &ignored) == SidecarReadResult::Ok) {
      const auto carry = [](const char *key, std::size_t keyLength,
                            const char *value, std::size_t valueLength,
                            void *userData) noexcept {
        (*static_cast<decltype(append) *>(userData))(
            ",\n  \"%.*s\": %.*s", static_cast<int>(keyLength), key,
            static_cast<int>(valueLength), value);
      };
      static_cast<void>(core::json_visit_unread_top_level_members(
          parser, *parser.root(), tracker, carry, &append));
    }
  }
  append("%s", "\n}\n");
  if (written <= 0) {
    log_sidecar_problem(path, "was not written: it would not fit in a "
                              "sidecar; the previous one is left as it was");
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
