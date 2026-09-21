// Implements the packer's generated-source certification: reads
// the directory's generated.manifest and compares the listed size and
// FNV-1a 64 hash against the bytes about to be cooked.

#include "generated_manifest.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>

#include "engine/core/json.h"

namespace {

constexpr const char *kManifestName = "generated.manifest";
constexpr std::uint64_t kManifestSchema = 1U;
constexpr std::size_t kMaxManifestBytes = 4U * 1024U * 1024U;

/// Reads a whole file into `out`; false when absent, unreadable or too
/// large. `exists` reports whether the file was there at all.
bool read_whole_file(const std::filesystem::path &path, std::string *out,
                     bool *exists) {
  *exists = false;
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.string().c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  *exists = true;
  std::string text{};
  char buffer[4096] = {};
  std::size_t got = 0U;
  while ((got = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    text.append(buffer, got);
    if (text.size() > kMaxManifestBytes) {
      std::fclose(file);
      return false;
    }
  }
  const bool readFailed = std::ferror(file) != 0;
  std::fclose(file);
  if (readFailed) {
    return false;
  }
  *out = std::move(text);
  return true;
}

bool parse_hex_u64(const char *text, std::uint64_t *outValue) {
  errno = 0;
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 16);
  if ((end == text) || (*end != '\0') || (errno == ERANGE)) {
    return false;
  }
  *outValue = static_cast<std::uint64_t>(value);
  return true;
}

void format_message(char *message, std::size_t capacity, const char *what,
                    const char *path) {
  if ((message == nullptr) || (capacity == 0U)) {
    return;
  }
  std::snprintf(message, capacity, "%s: %s", what, path);
}

} // namespace

bool generated_source_certified(const char *path, std::uint64_t contentHash,
                                char *message, std::size_t messageCapacity) {
  if (path == nullptr) {
    return false;
  }
  const std::filesystem::path sourcePath(path);
  std::filesystem::path directory = sourcePath.parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  std::string text{};
  bool exists = false;
  if (!read_whole_file(directory / kManifestName, &text, &exists)) {
    if (!exists) {
      return true; // not a generated directory
    }
    format_message(message, messageCapacity,
                   "generated manifest unreadable (interrupted publish; "
                   "re-run the generator)",
                   path);
    return false;
  }

  engine::core::JsonParser parser{};
  const engine::core::JsonValue *root = nullptr;
  if (!parser.parse(text.data(), text.size()) ||
      ((root = parser.root()) == nullptr) ||
      (root->type != engine::core::JsonValue::Type::Object)) {
    format_message(message, messageCapacity,
                   "generated manifest is not a JSON object (re-run the "
                   "generator)",
                   path);
    return false;
  }
  const engine::core::JsonValue *schema =
      parser.get_object_field(*root, "schema");
  std::uint64_t schemaValue = 0U;
  if ((schema == nullptr) || !parser.as_uint64(*schema, &schemaValue) ||
      (schemaValue != kManifestSchema)) {
    format_message(message, messageCapacity,
                   "generated manifest schema unsupported (re-run this "
                   "tree's generator)",
                   path);
    return false;
  }
  const engine::core::JsonValue *files =
      parser.get_object_field(*root, "files");
  if ((files == nullptr) ||
      (files->type != engine::core::JsonValue::Type::Object)) {
    format_message(message, messageCapacity,
                   "generated manifest lacks its file table (re-run the "
                   "generator)",
                   path);
    return false;
  }
  const std::string name = sourcePath.filename().string();
  const engine::core::JsonValue *entry =
      parser.get_object_field(*files, name.c_str());
  if (entry == nullptr) {
    return true; // hand-authored neighbour of a generated set
  }
  const engine::core::JsonValue *bytes =
      parser.get_object_field(*entry, "bytes");
  const engine::core::JsonValue *hash =
      parser.get_object_field(*entry, "fnv1a64");
  std::uint64_t listedBytes = 0U;
  char hashText[17] = {};
  std::uint64_t listedHash = 0U;
  if ((bytes == nullptr) || (hash == nullptr) ||
      !parser.as_uint64(*bytes, &listedBytes) ||
      !parser.copy_string(*hash, hashText, sizeof(hashText)) ||
      !parse_hex_u64(hashText, &listedHash)) {
    format_message(message, messageCapacity,
                   "generated manifest entry malformed (re-run the "
                   "generator)",
                   path);
    return false;
  }
  std::error_code sizeError{};
  const std::uintmax_t actualBytes =
      std::filesystem::file_size(sourcePath, sizeError);
  if (sizeError || (actualBytes != listedBytes) ||
      (contentHash != listedHash)) {
    format_message(message, messageCapacity,
                   "source disagrees with its generated manifest: the "
                   "generator's publish was interrupted, so this is a mixed "
                   "generation; re-run the generator before cooking",
                   path);
    return false;
  }
  return true;
}
