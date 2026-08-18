// Implements the runtime cooked-asset staleness diagnostic: reads the
// .meta.json sidecar's source path + content hash, re-hashes the source,
// and logs a once-per-asset warning on mismatch (issue #81). Runs on the
// CPU load path only (sync loads and the streaming worker), never per
// frame; the once-per-asset memory is a fixed lock-free table.

#include "engine/content/asset_staleness.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/hash.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"

namespace engine::content {

namespace {

constexpr std::size_t kMaxCheckedAssets = 512U;
constexpr std::size_t kMaxMetaFileBytes = 1024U * 1024U;

/// Fixed CAS-insert table of already-checked cooked-path hashes; the
/// staleness check (and its file IO) runs once per asset per session.
std::atomic<std::uint64_t> g_checkedPaths[kMaxCheckedAssets] = {};

/// Marks the path checked; false when it already was (or the table is
/// full, which disables further checks rather than re-warning).
bool try_mark_checked(std::uint64_t pathHash) noexcept {
  for (std::size_t i = 0U; i < kMaxCheckedAssets; ++i) {
    std::uint64_t current = g_checkedPaths[i].load(std::memory_order_acquire);
    if (current == pathHash) {
      return false;
    }
    if (current == 0ULL) {
      std::uint64_t expected = 0ULL;
      if (g_checkedPaths[i].compare_exchange_strong(
              expected, pathHash, std::memory_order_acq_rel)) {
        return true;
      }
      if (expected == pathHash) {
        return false;
      }
    }
  }
  return false;
}

/// FNV-1a of the file bytes, matching the packer's source-content hash.
bool hash_file_bytes(const char *path, std::uint64_t *outHash) noexcept {
  if ((path == nullptr) || (outHash == nullptr)) {
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

  std::uint64_t hash = core::kFnv1a64Offset;
  unsigned char buffer[4096] = {};
  while (true) {
    const std::size_t bytesRead = std::fread(buffer, 1U, sizeof(buffer), file);
    if (bytesRead == 0U) {
      break;
    }
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash = core::fnv1a_64_append(hash, buffer[i]);
    }
  }

  const bool readFailed = std::ferror(file) != 0;
  const bool closeFailed = std::fclose(file) != 0;
  if (readFailed || closeFailed) {
    return false;
  }
  *outHash = hash;
  return true;
}

bool parse_hex_u64(const char *text, std::uint64_t *outValue) noexcept {
  if ((text == nullptr) || (outValue == nullptr)) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 16);
  if ((end == text) || (*end != '\0') || (errno == ERANGE)) {
    return false;
  }
  *outValue = static_cast<std::uint64_t>(value);
  return true;
}

/// Reads the sidecar's recorded source path and source content hash.
bool read_meta_source_record(const char *cookedPath,
                             char (&outSourcePath)[512],
                             std::uint64_t *outSourceHash) noexcept {
  char metaPath[512] = {};
  const int written =
      std::snprintf(metaPath, sizeof(metaPath), "%s.meta.json", cookedPath);
  if ((written <= 0) || (written >= static_cast<int>(sizeof(metaPath)))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, metaPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(metaPath, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  if ((fileSize <= 0) ||
      (static_cast<std::size_t>(fileSize) > kMaxMetaFileBytes) ||
      (std::fseek(file, 0, SEEK_SET) != 0)) {
    std::fclose(file);
    return false;
  }

  // Cold load-path IO: allocation failure just skips the diagnostic.
  std::unique_ptr<char[]> buffer(
      new (std::nothrow) char[static_cast<std::size_t>(fileSize)]);
  if (buffer == nullptr) {
    std::fclose(file);
    return false;
  }
  const std::size_t readBytes =
      std::fread(buffer.get(), 1U, static_cast<std::size_t>(fileSize), file);
  std::fclose(file);
  if (readBytes != static_cast<std::size_t>(fileSize)) {
    return false;
  }

  core::JsonParser parser{};
  if (!parser.parse(buffer.get(), readBytes)) {
    return false;
  }
  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return false;
  }

  const core::JsonValue *sourceValue = parser.get_object_field(*root, "source");
  if ((sourceValue == nullptr) ||
      !parser.copy_string(*sourceValue, outSourcePath,
                          sizeof(outSourcePath))) {
    return false;
  }

  const core::JsonValue *hashValue =
      parser.get_object_field(*root, "sourceContentHash");
  if (hashValue == nullptr) {
    return false;
  }
  char hashText[17] = {};
  if (!parser.copy_string(*hashValue, hashText, sizeof(hashText))) {
    return false;
  }
  return parse_hex_u64(hashText, outSourceHash);
}

} // namespace

void warn_if_cooked_asset_stale(const char *cookedPath) noexcept {
  if (cookedPath == nullptr) {
    return;
  }

  const std::uint64_t pathHash = core::fnv1a_64(cookedPath);
  if (!try_mark_checked(pathHash)) {
    return;
  }

  char sourcePath[512] = {};
  std::uint64_t recordedSourceHash = 0ULL;
  if (!read_meta_source_record(cookedPath, sourcePath, &recordedSourceHash)) {
    return;
  }

  std::uint64_t currentSourceHash = 0ULL;
  if (!hash_file_bytes(sourcePath, &currentSourceHash)) {
    return;
  }

  if (currentSourceHash == recordedSourceHash) {
    return;
  }

  char message[640] = {};
  std::snprintf(message, sizeof(message),
                "stale cooked asset (source changed since last cook, re-run "
                "the asset packer): %s",
                cookedPath);
  core::log_message(core::LogLevel::Warning, "assets", message);
}

void reset_cooked_asset_stale_warnings() noexcept {
  for (std::size_t i = 0U; i < kMaxCheckedAssets; ++i) {
    g_checkedPaths[i].store(0ULL, std::memory_order_release);
  }
}

} // namespace engine::content
