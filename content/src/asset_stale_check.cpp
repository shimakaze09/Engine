// Implements the runtime cooked-asset trust checks: the staleness
// diagnostic (reads the .meta.json sidecar's source path + content hash,
// re-hashes the source, and logs a once-per-asset warning on mismatch,
// issue #81) and the cook-generation validation (audit #211: verifies the
// .cookstamp output manifest against the files on disk so a torn or mixed
// cook is rejected before a load accepts it). Both run on the CPU load
// path only (sync loads and the streaming worker), never per frame; the
// once-per-asset memories are fixed lock-free tables.

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

// ---- Cook-generation validation (audit #211) ------------------------------

constexpr std::size_t kMaxStampFileBytes = 1024U * 1024U;
constexpr std::size_t kMaxVerdictEntries = 512U;
constexpr std::uint32_t kVerdictPending = 0U;
constexpr std::uint32_t kVerdictOk = 1U;
constexpr std::uint32_t kVerdictRejected = 2U;

/// Fixed CAS-claimed verdict cache so each cooked path is hashed and
/// validated once per session; a full table or an in-flight entry just
/// revalidates without caching, which is correct and merely slower.
std::atomic<std::uint64_t> g_verdictPaths[kMaxVerdictEntries] = {};
std::atomic<std::uint32_t> g_verdictValues[kMaxVerdictEntries] = {};

/// Reads the whole stamp file; false when absent or oversized.
bool read_stamp_file(const char *cookedPath, std::unique_ptr<char[]> *outText,
                     std::size_t *outSize) noexcept {
  char stampPath[512] = {};
  const int written =
      std::snprintf(stampPath, sizeof(stampPath), "%s.cookstamp", cookedPath);
  if ((written <= 0) || (written >= static_cast<int>(sizeof(stampPath)))) {
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

  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  if ((fileSize <= 0) ||
      (static_cast<std::size_t>(fileSize) > kMaxStampFileBytes) ||
      (std::fseek(file, 0, SEEK_SET) != 0)) {
    std::fclose(file);
    return false;
  }

  std::unique_ptr<char[]> buffer(
      new (std::nothrow) char[static_cast<std::size_t>(fileSize) + 1U]);
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
  buffer[readBytes] = '\0';
  *outText = std::move(buffer);
  *outSize = readBytes;
  return true;
}

/// True for outputs that only affect presentation (browser thumbnails and
/// their checksum sidecars): their loss or drift must not brick the asset.
bool is_presentation_output(const char *path) noexcept {
  return (std::strstr(path, "/.thumbnails/") != nullptr) ||
         (std::strstr(path, "\\.thumbnails\\") != nullptr);
}

/// Validates every OUTPUT line of the stamp text against the files on
/// disk. Returns the verdict; logs the first contradiction with both the
/// asset and the offending output so the diagnostic is actionable.
std::uint32_t validate_stamp_outputs(const char *cookedPath, char *text) noexcept {
  bool sawOutputLine = false;
  char *cursor = text;
  while ((cursor != nullptr) && (*cursor != '\0')) {
    char *lineEnd = std::strchr(cursor, '\n');
    if (lineEnd != nullptr) {
      *lineEnd = '\0';
    }
    char *line = cursor;
    cursor = (lineEnd != nullptr) ? (lineEnd + 1) : nullptr;

    if (std::strncmp(line, "OUTPUT ", 7U) != 0) {
      continue;
    }
    sawOutputLine = true;

    // `OUTPUT <16-hex-hash> <path>`; a stamp that certifies outputs it
    // cannot even describe is a torn commit marker.
    const char *hashStart = line + 7;
    char hashText[17] = {};
    std::uint64_t recordedHash = 0ULL;
    bool lineOk = std::strlen(hashStart) >= 18U;
    if (lineOk) {
      std::memcpy(hashText, hashStart, 16U);
      lineOk = (hashStart[16] == ' ') && (hashStart[17] != '\0') &&
               parse_hex_u64(hashText, &recordedHash);
    }
    if (!lineOk) {
      char message[640] = {};
      std::snprintf(message, sizeof(message),
                    "rejecting cooked asset: malformed cook-stamp output "
                    "manifest (re-run the asset packer): %s",
                    cookedPath);
      core::log_message(core::LogLevel::Error, "assets", message);
      return kVerdictRejected;
    }
    const char *outputPath = hashStart + 17;

    std::uint64_t currentHash = 0ULL;
    const bool hashed = hash_file_bytes(outputPath, &currentHash);
    if (hashed && (currentHash == recordedHash)) {
      continue;
    }
    if (is_presentation_output(outputPath)) {
      char message[640] = {};
      std::snprintf(message, sizeof(message),
                    "cooked thumbnail %s than its cook stamp records "
                    "(re-run the asset packer): %s",
                    hashed ? "is newer or older" : "is missing", outputPath);
      core::log_message(core::LogLevel::Warning, "assets", message);
      continue;
    }
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "rejecting cooked asset %s: stamped output %s %s "
                  "(interrupted or mixed cook; re-run the asset packer)",
                  cookedPath, outputPath,
                  hashed ? "does not match its cook stamp" : "is missing");
    core::log_message(core::LogLevel::Error, "assets", message);
    return kVerdictRejected;
  }

  if (!sawOutputLine) {
    // Pre-manifest stamp schema: nothing certified, nothing to contradict.
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "cook stamp has no output manifest; generation not "
                  "validated: %s",
                  cookedPath);
    core::log_message(core::LogLevel::Info, "assets", message);
  }
  return kVerdictOk;
}

/// Computes the verdict for one cooked path (no cache involvement).
std::uint32_t compute_generation_verdict(const char *cookedPath) noexcept {
  std::unique_ptr<char[]> stampText{};
  std::size_t stampSize = 0U;
  if (!read_stamp_file(cookedPath, &stampText, &stampSize)) {
    // Never-certified content (hand-placed, legacy, or test assets) stays
    // loadable; the notice keeps the gap visible without failing loads.
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "no cook stamp; generation not validated: %s", cookedPath);
    core::log_message(core::LogLevel::Info, "assets", message);
    return kVerdictOk;
  }
  return validate_stamp_outputs(cookedPath, stampText.get());
}

} // namespace

bool cooked_asset_generation_ok(const char *cookedPath) noexcept {
  if (cookedPath == nullptr) {
    return false;
  }

  const std::uint64_t pathHash = core::fnv1a_64(cookedPath);
  std::size_t claimedSlot = kMaxVerdictEntries;
  for (std::size_t i = 0U; i < kMaxVerdictEntries; ++i) {
    std::uint64_t current = g_verdictPaths[i].load(std::memory_order_acquire);
    if (current == 0ULL) {
      std::uint64_t expected = 0ULL;
      if (g_verdictPaths[i].compare_exchange_strong(
              expected, pathHash, std::memory_order_acq_rel)) {
        claimedSlot = i;
        break;
      }
      current = expected;
    }
    if (current == pathHash) {
      const std::uint32_t cached =
          g_verdictValues[i].load(std::memory_order_acquire);
      if (cached != kVerdictPending) {
        return cached == kVerdictOk;
      }
      // Another thread is validating this path right now; validate
      // redundantly rather than blocking the load path.
      break;
    }
  }

  const std::uint32_t verdict = compute_generation_verdict(cookedPath);
  if (claimedSlot < kMaxVerdictEntries) {
    g_verdictValues[claimedSlot].store(verdict, std::memory_order_release);
  }
  return verdict == kVerdictOk;
}

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
  for (std::size_t i = 0U; i < kMaxVerdictEntries; ++i) {
    g_verdictValues[i].store(kVerdictPending, std::memory_order_release);
    g_verdictPaths[i].store(0ULL, std::memory_order_release);
  }
}

} // namespace engine::content
