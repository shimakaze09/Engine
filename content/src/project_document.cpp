// Implements the project document's validation, reader and writer. The
// reader walks every object's members so an unknown or repeated key is
// refused rather than dropped; the writer emits one field per line.

#include "engine/content/project_document.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"

namespace engine::content {
namespace {

constexpr const char *kLogChannel = "project";

std::unexpected<ProjectReadFailure> refuse(
    const char *field, const char *reason,
    ProjectReadFailureKind kind = ProjectReadFailureKind::Malformed) noexcept {
  ProjectReadFailure failure{};
  failure.kind = kind;
  std::snprintf(failure.field, sizeof(failure.field), "%s",
                (field != nullptr) ? field : "");
  failure.reason = (reason != nullptr) ? reason : "";
  return std::unexpected(failure);
}

bool has_control_character(const char *text) noexcept {
  for (const char *c = text; *c != '\0'; ++c) {
    const unsigned char byte = static_cast<unsigned char>(*c);
    if ((byte < 0x20U) || (byte == 0x7FU)) {
      return true;
    }
  }
  return false;
}

bool ends_with(const char *text, const char *suffix) noexcept {
  const std::size_t length = std::strlen(text);
  const std::size_t suffixLength = std::strlen(suffix);
  return (length > suffixLength) &&
         (std::strcmp(text + (length - suffixLength), suffix) == 0);
}

/// A name that is also a portable file name: non-empty, no character a
/// Windows, macOS or Linux filesystem refuses or treats specially, not
/// "." or "..", and no leading or trailing space or trailing dot.
const char *name_problem(const char *name) noexcept {
  const std::size_t length = std::strlen(name);
  if (length == 0U) {
    return "is empty";
  }
  if (has_control_character(name) ||
      (std::strpbrk(name, "<>:\"/\\|?*") != nullptr)) {
    return "holds a character a file name cannot";
  }
  if ((std::strcmp(name, ".") == 0) || (std::strcmp(name, "..") == 0) ||
      (name[0] == ' ') || (name[length - 1U] == ' ') ||
      (name[length - 1U] == '.')) {
    return "is not a portable file name";
  }
  return nullptr;
}

/// A root relative to the project directory: non-empty segments separated
/// by single '/', none of them "." or "..", no drive, no backslash.
const char *root_problem(const char *root) noexcept {
  if (root[0] == '\0') {
    return "is empty";
  }
  if (has_control_character(root) || (std::strpbrk(root, "\\:") != nullptr)) {
    return "holds a backslash, a colon or a control character";
  }
  const char *segment = root;
  while (true) {
    const char *slash = std::strchr(segment, '/');
    const std::size_t length = (slash != nullptr)
                                   ? static_cast<std::size_t>(slash - segment)
                                   : std::strlen(segment);
    if (length == 0U) {
      return "is absolute or has an empty segment";
    }
    if (((length == 1U) && (segment[0] == '.')) ||
        ((length == 2U) && (segment[0] == '.') && (segment[1] == '.'))) {
      return "has a '.' or '..' segment";
    }
    if (slash == nullptr) {
      return nullptr;
    }
    segment = slash + 1;
  }
}

/// True when one root is the other or lies inside it.
bool roots_overlap(const char *a, const char *b) noexcept {
  const std::size_t aLength = std::strlen(a);
  const std::size_t bLength = std::strlen(b);
  const char *shorter = (aLength <= bLength) ? a : b;
  const char *longer = (aLength <= bLength) ? b : a;
  const std::size_t shortLength = (aLength <= bLength) ? aLength : bLength;
  return (std::strncmp(shorter, longer, shortLength) == 0) &&
         ((longer[shortLength] == '\0') || (longer[shortLength] == '/'));
}

/// A content path: canonical, under kProjectContentMount, with `suffix`.
const char *content_path_problem(const char *path,
                                 const char *suffix) noexcept {
  if (has_control_character(path)) {
    return "holds a control character";
  }
  char canonical[kProjectPathCapacity] = {};
  if (!core::canonical_virtual_path(path, canonical, sizeof(canonical)) ||
      (std::strcmp(canonical, path) != 0)) {
    return "is not a canonical virtual path";
  }
  const std::size_t mountLength = std::strlen(kProjectContentMount);
  if ((std::strncmp(path, kProjectContentMount, mountLength) != 0) ||
      (path[mountLength] != '/')) {
    return "is not under the content mount 'assets/'";
  }
  if (!ends_with(path, suffix)) {
    return (std::strcmp(suffix, ".scene") == 0) ? "is not a .scene"
                                                : "is not a .lua module";
  }
  return nullptr;
}

/// True for 0, which is unlimited, or a value within [minimum, maximum].
bool limit_in_range(std::uint32_t value, std::uint32_t minimum,
                    std::uint32_t maximum) noexcept {
  return (value == 0U) || ((value >= minimum) && (value <= maximum));
}

/// Reads an optional limit: absent leaves it unset; present must be a
/// non-negative integer that fits, and range is left to validation.
std::expected<void, ProjectReadFailure>
read_limit(const core::JsonParser &parser, const core::JsonValue &object,
           const char *key, const char *field, bool *outSet,
           std::uint32_t *out) noexcept {
  core::JsonValue value{};
  if (!parser.get_object_field(object, key, &value)) {
    return {};
  }
  std::int64_t number = 0;
  if (!parser.as_int64(value, &number)) {
    return refuse(field, "is not an integer");
  }
  if ((number < 0) || (number > static_cast<std::int64_t>(UINT32_MAX))) {
    return refuse(field, "is out of range");
  }
  *outSet = true;
  *out = static_cast<std::uint32_t>(number);
  return {};
}

/// Checks an object holds only `allowed` keys, each at most once.
std::expected<void, ProjectReadFailure>
check_members(const core::JsonParser &parser, const core::JsonValue &object,
              const char *const *allowed, std::size_t allowedCount,
              const char *prefix) noexcept {
  bool seen[8] = {};
  const std::size_t members = parser.object_size(object);
  for (std::size_t i = 0U; i < members; ++i) {
    core::JsonValue key{};
    core::JsonValue value{};
    char name[48] = {};
    if (!parser.get_object_member(object, i, &key, &value) ||
        !parser.copy_string_strict(key, name, sizeof(name))) {
      return refuse(prefix, "has a key that will not read");
    }
    std::size_t match = allowedCount;
    for (std::size_t k = 0U; k < allowedCount; ++k) {
      if (std::strcmp(name, allowed[k]) == 0) {
        match = k;
      }
    }
    char field[64] = {};
    std::snprintf(field, sizeof(field), "%s%s%s", prefix,
                  (prefix[0] != '\0') ? "." : "", name);
    if (match == allowedCount) {
      return refuse(field, "is not a key this schema knows");
    }
    if (seen[match]) {
      return refuse(field, "appears twice");
    }
    seen[match] = true;
  }
  return {};
}

/// Copies a required string field whole.
std::expected<void, ProjectReadFailure>
read_string(const core::JsonParser &parser, const core::JsonValue &object,
            const char *key, const char *field, char *out,
            std::size_t capacity) noexcept {
  core::JsonValue value{};
  if (!parser.get_object_field(object, key, &value)) {
    return refuse(field, "is missing");
  }
  if (value.type != core::JsonValue::Type::String) {
    return refuse(field, "is not a string");
  }
  if (!parser.copy_string_strict(value, out, capacity)) {
    return refuse(field, "is too long");
  }
  return {};
}

std::expected<void, ProjectReadFailure>
read_object(const core::JsonParser &parser, const core::JsonValue &object,
            const char *key, core::JsonValue *out) noexcept {
  if (!parser.get_object_field(object, key, out)) {
    return refuse(key, "is missing");
  }
  if (out->type != core::JsonValue::Type::Object) {
    return refuse(key, "is not an object");
  }
  return {};
}

/// Appends text whole; sticky failure once it does not fit.
struct Appender final {
  char *out;
  std::size_t capacity;
  std::size_t length = 0U;
  bool ok = true;

  void text(const char *value) noexcept {
    if (!ok) {
      return;
    }
    const std::size_t size = std::strlen(value);
    if (size >= capacity - length) {
      ok = false;
      return;
    }
    std::memcpy(out + length, value, size + 1U);
    length += size;
  }

  /// A JSON string literal. Validation has refused control characters, so
  /// only the quote and the backslash need escaping.
  void quoted(const char *value) noexcept {
    text("\"");
    for (const char *c = value; ok && (*c != '\0'); ++c) {
      if ((*c == '"') || (*c == '\\')) {
        text("\\");
      }
      const char one[2] = {*c, '\0'};
      text(one);
    }
    text("\"");
  }
};

} // namespace

std::expected<void, ProjectReadFailure>
validate_project_document(const ProjectDocument &document) noexcept {
  if (const char *problem = name_problem(document.name)) {
    return refuse("identity.name", problem);
  }
  if (has_control_character(document.organisation)) {
    return refuse("identity.organisation", "holds a control character");
  }
  if ((document.version[0] == '\0') ||
      has_control_character(document.version)) {
    return refuse("identity.version", "is empty or holds a control character");
  }
  if (!asset_guid_is_valid(document.guid)) {
    return refuse("identity.guid", "is the nil guid, which names nothing");
  }
  if (const char *problem = root_problem(document.contentRoot)) {
    return refuse("roots.content", problem);
  }
  if (const char *problem = root_problem(document.cacheRoot)) {
    return refuse("roots.cache", problem);
  }
  if (roots_overlap(document.contentRoot, document.cacheRoot)) {
    return refuse("roots.cache", "is the content root or is nested with it");
  }
  if (document.sceneCount == 0U) {
    return refuse("scenes", "is empty; a project starts in one of them");
  }
  if (document.sceneCount > kMaxProjectScenes) {
    return refuse("scenes", "lists more scenes than a project can hold");
  }
  bool startupListed = false;
  for (std::size_t i = 0U; i < document.sceneCount; ++i) {
    char field[64] = {};
    std::snprintf(field, sizeof(field), "scenes[%zu]", i);
    if (const char *problem =
            content_path_problem(document.scenes[i], ".scene")) {
      return refuse(field, problem);
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (std::strcmp(document.scenes[i], document.scenes[j]) == 0) {
        return refuse(field, "repeats an earlier scene");
      }
    }
    startupListed = startupListed || (std::strcmp(document.scenes[i],
                                                  document.startupScene) == 0);
  }
  if (!startupListed) {
    return refuse("startupScene", "is not one of the project's scenes");
  }
  if (document.mainScript[0] != '\0') {
    if (const char *problem =
            content_path_problem(document.mainScript, ".lua")) {
      return refuse("mainScript", problem);
    }
  }
  const ProjectScriptLimits &limits = document.scriptLimits;
  if (limits.instructionLimitSet &&
      !limit_in_range(limits.instructionLimit, kProjectMinInstructionLimit,
                      kProjectMaxInstructionLimit)) {
    return refuse("scripting.instructionLimit",
                  "is neither 0 (unlimited) nor from 100000 to 1000000000");
  }
  if (limits.memoryLimitSet &&
      !limit_in_range(limits.memoryLimitMiB, kProjectMinMemoryLimitMiB,
                      kProjectMaxMemoryLimitMiB)) {
    return refuse("scripting.memoryLimitMiB",
                  "is neither 0 (unlimited) nor from 16 to 2048");
  }
  return {};
}

std::expected<void, ProjectReadFailure>
parse_project_document(const char *text, std::size_t length,
                       ProjectDocument *out) noexcept {
  if ((text == nullptr) || (out == nullptr)) {
    return refuse("", "has no text to read");
  }
  core::JsonParser parser{};
  if (!parser.parse(text, length)) {
    return refuse("", "is not valid JSON");
  }
  const core::JsonValue *rootPointer = parser.root();
  if ((rootPointer == nullptr) ||
      (rootPointer->type != core::JsonValue::Type::Object)) {
    return refuse("", "is not a JSON object");
  }
  const core::JsonValue root = *rootPointer;

  // The version first: a document from a newer schema is refused as that,
  // not for the first key this build does not recognise.
  core::JsonValue versionValue{};
  std::int64_t version = 0;
  if (!parser.get_object_field(root, "schemaVersion", &versionValue)) {
    return refuse("schemaVersion", "is missing");
  }
  if (!parser.as_int64(versionValue, &version)) {
    return refuse("schemaVersion", "is not an integer");
  }
  if (version != static_cast<std::int64_t>(kProjectSchemaVersion)) {
    return refuse("schemaVersion", "is not the version this build reads");
  }

  constexpr const char *kTopKeys[] = {
      "schemaVersion", "identity",   "roots",    "scenes",
      "startupScene",  "mainScript", "scripting"};
  constexpr const char *kIdentityKeys[] = {"name", "organisation", "version",
                                           "guid"};
  constexpr const char *kRootKeys[] = {"content", "cache"};
  constexpr const char *kScriptingKeys[] = {"instructionLimit",
                                            "memoryLimitMiB"};
  if (auto checked = check_members(parser, root, kTopKeys, 7U, "");
      !checked.has_value()) {
    return checked;
  }

  // Staged into a local and copied out only once every rule has passed, so
  // a refusal leaves the caller's document exactly as it was.
  std::unique_ptr<ProjectDocument> staged(new (std::nothrow) ProjectDocument());
  if (staged == nullptr) {
    return refuse("", "could not be staged",
                  ProjectReadFailureKind::Unreadable);
  }

  core::JsonValue identity{};
  if (auto r = read_object(parser, root, "identity", &identity);
      !r.has_value()) {
    return r;
  }
  if (auto r = check_members(parser, identity, kIdentityKeys, 4U, "identity");
      !r.has_value()) {
    return r;
  }
  char guidText[kAssetGuidTextLength + 1U] = {};
  if (auto r = read_string(parser, identity, "name", "identity.name",
                           staged->name, sizeof(staged->name));
      !r.has_value()) {
    return r;
  }
  if (auto r =
          read_string(parser, identity, "organisation", "identity.organisation",
                      staged->organisation, sizeof(staged->organisation));
      !r.has_value()) {
    return r;
  }
  if (auto r = read_string(parser, identity, "version", "identity.version",
                           staged->version, sizeof(staged->version));
      !r.has_value()) {
    return r;
  }
  if (auto r = read_string(parser, identity, "guid", "identity.guid", guidText,
                           sizeof(guidText));
      !r.has_value()) {
    return r;
  }
  if (!parse_asset_guid(guidText, &staged->guid)) {
    return refuse("identity.guid", "is not canonical UUID text");
  }

  core::JsonValue roots{};
  if (auto r = read_object(parser, root, "roots", &roots); !r.has_value()) {
    return r;
  }
  if (auto r = check_members(parser, roots, kRootKeys, 2U, "roots");
      !r.has_value()) {
    return r;
  }
  if (auto r = read_string(parser, roots, "content", "roots.content",
                           staged->contentRoot, sizeof(staged->contentRoot));
      !r.has_value()) {
    return r;
  }
  if (auto r = read_string(parser, roots, "cache", "roots.cache",
                           staged->cacheRoot, sizeof(staged->cacheRoot));
      !r.has_value()) {
    return r;
  }

  core::JsonValue scenes{};
  if (!parser.get_object_field(root, "scenes", &scenes)) {
    return refuse("scenes", "is missing");
  }
  if (scenes.type != core::JsonValue::Type::Array) {
    return refuse("scenes", "is not an array");
  }
  const std::size_t sceneCount = parser.array_size(scenes);
  if (sceneCount > kMaxProjectScenes) {
    return refuse("scenes", "lists more scenes than a project can hold");
  }
  for (std::size_t i = 0U; i < sceneCount; ++i) {
    char field[64] = {};
    std::snprintf(field, sizeof(field), "scenes[%zu]", i);
    core::JsonValue scene{};
    if (!parser.get_array_element(scenes, i, &scene) ||
        (scene.type != core::JsonValue::Type::String)) {
      return refuse(field, "is not a string");
    }
    if (!parser.copy_string_strict(scene, staged->scenes[i],
                                   sizeof(staged->scenes[i]))) {
      return refuse(field, "is too long");
    }
  }
  staged->sceneCount = sceneCount;

  if (auto r = read_string(parser, root, "startupScene", "startupScene",
                           staged->startupScene, sizeof(staged->startupScene));
      !r.has_value()) {
    return r;
  }
  core::JsonValue mainScript{};
  if (parser.get_object_field(root, "mainScript", &mainScript)) {
    if (auto r = read_string(parser, root, "mainScript", "mainScript",
                             staged->mainScript, sizeof(staged->mainScript));
        !r.has_value()) {
      return r;
    }
    if (staged->mainScript[0] == '\0') {
      // Absent is how a project says it has no main script; an empty
      // string would be a second spelling of the same thing.
      return refuse("mainScript", "is empty; omit it instead");
    }
  }

  core::JsonValue scripting{};
  if (parser.get_object_field(root, "scripting", &scripting)) {
    if (scripting.type != core::JsonValue::Type::Object) {
      return refuse("scripting", "is not an object");
    }
    if (auto r =
            check_members(parser, scripting, kScriptingKeys, 2U, "scripting");
        !r.has_value()) {
      return r;
    }
    ProjectScriptLimits &limits = staged->scriptLimits;
    if (auto r = read_limit(
            parser, scripting, "instructionLimit", "scripting.instructionLimit",
            &limits.instructionLimitSet, &limits.instructionLimit);
        !r.has_value()) {
      return r;
    }
    if (auto r = read_limit(parser, scripting, "memoryLimitMiB",
                            "scripting.memoryLimitMiB", &limits.memoryLimitSet,
                            &limits.memoryLimitMiB);
        !r.has_value()) {
      return r;
    }
    if (!limits.instructionLimitSet && !limits.memoryLimitSet) {
      // Absent is how a project keeps the engine's limits; an empty object
      // would be a second spelling of the same thing.
      return refuse("scripting", "is empty; omit it instead");
    }
  }

  if (auto r = validate_project_document(*staged); !r.has_value()) {
    return r;
  }
  *out = *staged;
  return {};
}

std::expected<void, ProjectReadFailure>
read_project_document(const char *osPath, ProjectDocument *out) noexcept {
  if ((osPath == nullptr) || (out == nullptr)) {
    return refuse("", "has no path", ProjectReadFailureKind::Unreadable);
  }
  std::unique_ptr<char[]> buffer(
      new (std::nothrow) char[kMaxProjectDocumentBytes]);
  if (buffer == nullptr) {
    return refuse("", "could not be read", ProjectReadFailureKind::Unreadable);
  }
  std::size_t size = 0U;
  std::expected<void, ProjectReadFailure> result{};
  switch (core::read_whole_file(osPath, buffer.get(), kMaxProjectDocumentBytes,
                                &size)) {
  case core::FileReadResult::Ok:
    result = parse_project_document(buffer.get(), size, out);
    break;
  case core::FileReadResult::Absent:
    result = refuse("", "does not exist", ProjectReadFailureKind::Absent);
    break;
  case core::FileReadResult::Unreadable:
    result = refuse("", "exists but could not be read",
                    ProjectReadFailureKind::Unreadable);
    break;
  case core::FileReadResult::TooLarge:
    result = refuse("", "is larger than a project document can be",
                    ProjectReadFailureKind::TooLarge);
    break;
  }
  if (!result.has_value()) {
    char message[512] = {};
    std::snprintf(message, sizeof(message), "project document %s: %s%s%s",
                  osPath, result.error().field,
                  (result.error().field[0] != '\0') ? " " : "",
                  result.error().reason);
    core::log_message(core::LogLevel::Error, kLogChannel, message);
  }
  return result;
}

bool format_project_document(const ProjectDocument &document, char *out,
                             std::size_t capacity,
                             std::size_t *outLength) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  char guidText[kAssetGuidTextLength + 1U] = {};
  if (!validate_project_document(document).has_value() ||
      !format_asset_guid(document.guid, guidText, sizeof(guidText))) {
    return false;
  }
  Appender a{out, capacity};
  char version[16] = {};
  std::snprintf(version, sizeof(version), "%u", kProjectSchemaVersion);
  a.text("{\n  \"schemaVersion\": ");
  a.text(version);
  a.text(",\n  \"identity\": {\n    \"name\": ");
  a.quoted(document.name);
  a.text(",\n    \"organisation\": ");
  a.quoted(document.organisation);
  a.text(",\n    \"version\": ");
  a.quoted(document.version);
  a.text(",\n    \"guid\": ");
  a.quoted(guidText);
  a.text("\n  },\n  \"roots\": {\n    \"content\": ");
  a.quoted(document.contentRoot);
  a.text(",\n    \"cache\": ");
  a.quoted(document.cacheRoot);
  a.text("\n  },\n  \"scenes\": [");
  for (std::size_t i = 0U; i < document.sceneCount; ++i) {
    a.text((i == 0U) ? "\n    " : ",\n    ");
    a.quoted(document.scenes[i]);
  }
  a.text("\n  ],\n  \"startupScene\": ");
  a.quoted(document.startupScene);
  if (document.mainScript[0] != '\0') {
    a.text(",\n  \"mainScript\": ");
    a.quoted(document.mainScript);
  }
  const ProjectScriptLimits &limits = document.scriptLimits;
  if (limits.instructionLimitSet || limits.memoryLimitSet) {
    char number[16] = {};
    a.text(",\n  \"scripting\": {");
    if (limits.instructionLimitSet) {
      std::snprintf(number, sizeof(number), "%u", limits.instructionLimit);
      a.text("\n    \"instructionLimit\": ");
      a.text(number);
    }
    if (limits.memoryLimitSet) {
      std::snprintf(number, sizeof(number), "%u", limits.memoryLimitMiB);
      a.text(limits.instructionLimitSet ? ",\n    \"memoryLimitMiB\": "
                                        : "\n    \"memoryLimitMiB\": ");
      a.text(number);
    }
    a.text("\n  }");
  }
  a.text("\n}\n");
  if (!a.ok) {
    out[0] = '\0';
    return false;
  }
  if (outLength != nullptr) {
    *outLength = a.length;
  }
  return true;
}

bool write_project_document(const char *osPath,
                            const ProjectDocument &document) noexcept {
  if (osPath == nullptr) {
    return false;
  }
  if (auto valid = validate_project_document(document); !valid.has_value()) {
    char message[512] = {};
    std::snprintf(message, sizeof(message),
                  "project document %s was not written: %s %s", osPath,
                  valid.error().field, valid.error().reason);
    core::log_message(core::LogLevel::Error, kLogChannel, message);
    return false;
  }
  std::unique_ptr<char[]> buffer(
      new (std::nothrow) char[kMaxProjectDocumentBytes]);
  std::size_t length = 0U;
  if ((buffer == nullptr) ||
      !format_project_document(document, buffer.get(), kMaxProjectDocumentBytes,
                               &length)) {
    return false;
  }
  return core::atomic_write_file(osPath, buffer.get(), length);
}

} // namespace engine::content
