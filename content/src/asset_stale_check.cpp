// Implements the runtime cooked-asset trust checks: the staleness
// diagnostic (re-hashes the source the .cookmeta sidecar records and each
// dependency the .cookstamp's DEP_HASH lines record, and logs a
// once-per-asset warning naming what changed)
// and the cook-generation validation (verifies the .cookstamp output
// manifest against the files on disk so a torn or mixed cook is rejected
// before a load accepts it). Both run on the CPU load
// path only (sync loads and the streaming worker), never per frame; the
// once-per-asset memories are fixed lock-free tables.

#include "engine/content/asset_staleness.h"

#include "engine/content/cook_contract.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

/// Largest file the load path re-hashes; a stamp or sidecar naming
/// something bigger is refused rather than read.
constexpr std::uintmax_t kMaxHashedFileBytes = 512ULL * 1024ULL * 1024ULL;

/// FNV-1a of the file bytes, matching the packer's source-content hash.
/// Only a regular file within kMaxHashedFileBytes is opened: a device,
/// FIFO or directory named by a stamp or sidecar would otherwise block
/// the streaming worker (and editor shutdown, which joins it) forever, so
/// the read is also bounded by the size observed up front.
bool hash_file_bytes(const char *path, std::uint64_t *outHash) noexcept {
  if ((path == nullptr) || (outHash == nullptr)) {
    return false;
  }
  std::error_code statusError{};
  const std::filesystem::file_status status =
      std::filesystem::status(std::filesystem::path(path), statusError);
  if (statusError || !std::filesystem::is_regular_file(status)) {
    return false;
  }
  std::error_code sizeError{};
  const std::uintmax_t fileSize =
      std::filesystem::file_size(std::filesystem::path(path), sizeError);
  if (sizeError || (fileSize > kMaxHashedFileBytes)) {
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
  std::uintmax_t remaining = fileSize;
  while (remaining > 0U) {
    const std::size_t want = (remaining < sizeof(buffer))
                                 ? static_cast<std::size_t>(remaining)
                                 : sizeof(buffer);
    const std::size_t bytesRead = std::fread(buffer, 1U, want, file);
    if (bytesRead == 0U) {
      break;
    }
    remaining -= bytesRead;
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
bool read_meta_source_record(const char *cookedPath, char (&outSourcePath)[512],
                             std::uint64_t *outSourceHash) noexcept {
  char metaPath[512] = {};
  const int written =
      std::snprintf(metaPath, sizeof(metaPath), "%s.cookmeta", cookedPath);
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
      !parser.copy_string(*sourceValue, outSourcePath, sizeof(outSourcePath))) {
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

// ---- Cook-generation validation ------------------------------

constexpr std::size_t kMaxStampFileBytes = 1024U * 1024U;
constexpr std::size_t kMaxVerdictEntries = 512U;
constexpr std::uint32_t kVerdictPending = 0U;
constexpr std::uint32_t kVerdictOk = 1U;
constexpr std::uint32_t kVerdictRejected = 2U;

/// Fixed CAS-claimed verdict cache so each cooked path's outputs are hashed
/// once per stamp: an entry is the verdict in the low two bits over the
/// stamp's content key, in one atomic so a reader never pairs one stamp's
/// key with another's verdict. A recook rewrites the stamp, so its next
/// check validates afresh. A full table or an in-flight entry just
/// revalidates without caching, which is correct and merely slower.
std::atomic<std::uint64_t> g_verdictPaths[kMaxVerdictEntries] = {};
std::atomic<std::uint64_t> g_verdictValues[kMaxVerdictEntries] = {};

/// The 62-bit key a verdict is cached under: the stamp's content hash, or
/// a fixed key for an asset with no stamp.
constexpr std::uint64_t kStampKeyMask = ~0ULL >> 2U;
constexpr std::uint64_t kNoStampKey = kStampKeyMask;

std::uint64_t pack_verdict(std::uint64_t stampKey,
                           std::uint32_t verdict) noexcept {
  return ((stampKey & kStampKeyMask) << 2U) | verdict;
}

/// Writes `<cookedPath>.cookstamp`; false when it does not fit.
bool build_stamp_path(const char *cookedPath, char (&out)[512]) noexcept {
  const int written =
      std::snprintf(out, sizeof(out), "%s.cookstamp", cookedPath);
  return (written > 0) && (written < static_cast<int>(sizeof(out)));
}

/// Reads the whole stamp file; false when absent or oversized.
bool read_stamp_file(const char *stampPath, std::unique_ptr<char[]> *outText,
                     std::size_t *outSize) noexcept {

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

/// Parses an unsigned decimal value after `prefix` on `line`; false when
/// the line does not start with the prefix or carries no plain number.
bool parse_prefixed_uint(const char *line, const char *prefix,
                         std::uint32_t *outValue) noexcept {
  const std::size_t prefixLength = std::strlen(prefix);
  if (std::strncmp(line, prefix, prefixLength) != 0) {
    return false;
  }
  const char *digits = line + prefixLength;
  if ((*digits < '0') || (*digits > '9')) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long value = std::strtoul(digits, &end, 10);
  if ((errno != 0) || (end == digits) || (value > 0xFFFFFFFFUL)) {
    return false;
  }
  *outValue = static_cast<std::uint32_t>(value);
  return true;
}

/// Validates the stamp's contract lines and every OUTPUT line against
/// the files on disk. A stamp declaring a newer schema than this build
/// reads, or a tool version other than the one this build was cooked
/// against, certifies nothing here: its outputs may be a format or an
/// import semantics this loader does not expect, so the asset is refused
/// until the tree is recooked. Returns the verdict; logs the first
/// contradiction with both the asset and the offending line so the
/// diagnostic is actionable.
std::uint32_t validate_stamp_outputs(const char *cookedPath,
                                     char *text) noexcept {
  bool sawOutputLine = false;
  bool sawToolVersion = false;
  std::uint32_t schema = 0U;
  // Schema-4 paths join under the stamp's directory.
  const char *stampDirEnd = std::strrchr(cookedPath, '/');
  const char *stampDirEndBackslash = std::strrchr(cookedPath, '\\');
  if ((stampDirEndBackslash != nullptr) &&
      ((stampDirEnd == nullptr) || (stampDirEndBackslash > stampDirEnd))) {
    stampDirEnd = stampDirEndBackslash;
  }
  const std::size_t stampDirLength =
      (stampDirEnd != nullptr)
          ? static_cast<std::size_t>(stampDirEnd - cookedPath)
          : 0U;
  char *cursor = text;
  while ((cursor != nullptr) && (*cursor != '\0')) {
    char *lineEnd = std::strchr(cursor, '\n');
    if (lineEnd != nullptr) {
      *lineEnd = '\0';
    }
    char *line = cursor;
    cursor = (lineEnd != nullptr) ? (lineEnd + 1) : nullptr;
    // A CR before the line feed belongs to the terminator. The packer
    // writes LF, but a stamp is a text file a checkout may rewrite —
    // Git for Windows does by default — and a CR kept as the last byte of
    // a recorded path names a file that does not exist. What the stamp
    // certifies is unaffected: every output is still hashed below.
    const std::size_t lineLength = std::strlen(line);
    if ((lineLength > 0U) && (line[lineLength - 1U] == '\r')) {
      line[lineLength - 1U] = '\0';
    }
    if (std::strlen(line) + 1U > kMaxCookStampLineBytes) {
      // Longer than the writer ever emits: a truncated or hand-edited
      // stamp, never a path to open.
      char message[640] = {};
      std::snprintf(message, sizeof(message),
                    "rejecting cooked asset: cook-stamp line exceeds %zu "
                    "bytes (re-run the asset packer): %s",
                    kMaxCookStampLineBytes, cookedPath);
      core::log_message(core::LogLevel::Error, "assets", message);
      return kVerdictRejected;
    }

    std::uint32_t declared = 0U;
    if (parse_prefixed_uint(line, "SCHEMA ", &declared)) {
      schema = declared;
      if (declared > kCookStampSchema) {
        char message[640] = {};
        std::snprintf(message, sizeof(message),
                      "rejecting cooked asset %s: cook stamp schema %u is "
                      "newer than this build reads (%u); re-run this "
                      "build's asset packer",
                      cookedPath, static_cast<unsigned int>(declared),
                      static_cast<unsigned int>(kCookStampSchema));
        core::log_message(core::LogLevel::Error, "assets", message);
        return kVerdictRejected;
      }
      continue;
    }
    if (parse_prefixed_uint(line, "TOOL_VERSION ", &declared)) {
      sawToolVersion = true;
      if (declared != kCookToolVersion) {
        char message[640] = {};
        std::snprintf(message, sizeof(message),
                      "rejecting cooked asset %s: cooked by asset packer "
                      "tool version %u, this build expects %u; re-run the "
                      "asset packer",
                      cookedPath, static_cast<unsigned int>(declared),
                      static_cast<unsigned int>(kCookToolVersion));
        core::log_message(core::LogLevel::Error, "assets", message);
        return kVerdictRejected;
      }
      continue;
    }

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
    const char *recordedPath = hashStart + 17;
    // Schema 4 records the path relative to the stamp's directory and
    // never outside it; a legacy schema recorded the packer's invocation
    // path, which the TOOL_VERSION gate above has already refused for
    // every stamp this build did not cook. Either way the file is
    // addressed only after it passed the containment rule.
    char joinedPath[512] = {};
    const char *outputPath = recordedPath;
    if (schema >= 4U) {
      if (!cook_stamp_path_is_contained(recordedPath)) {
        char message[640] = {};
        std::snprintf(message, sizeof(message),
                      "rejecting cooked asset %s: stamped output path "
                      "leaves the stamp directory (corrupt cook stamp; "
                      "re-run the asset packer): %s",
                      cookedPath, recordedPath);
        core::log_message(core::LogLevel::Error, "assets", message);
        return kVerdictRejected;
      }
      const int written =
          (stampDirLength > 0U)
              ? std::snprintf(joinedPath, sizeof(joinedPath), "%.*s/%s",
                              static_cast<int>(stampDirLength), cookedPath,
                              recordedPath)
              : std::snprintf(joinedPath, sizeof(joinedPath), "%s",
                              recordedPath);
      if ((written <= 0) || (written >= static_cast<int>(sizeof(joinedPath)))) {
        char message[640] = {};
        std::snprintf(message, sizeof(message),
                      "rejecting cooked asset %s: stamped output path does "
                      "not fit (re-run the asset packer)",
                      cookedPath);
        core::log_message(core::LogLevel::Error, "assets", message);
        return kVerdictRejected;
      }
      outputPath = joinedPath;
    }

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

  // From schema 3 on the packer always writes TOOL_VERSION; a stamp that
  // declares that schema without it is torn or hand-edited and certifies
  // nothing. Older schemas predate the line and stay on the legacy
  // accept path.
  if ((schema >= 3U) && !sawToolVersion) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "rejecting cooked asset %s: cook stamp schema %u declares "
                  "no TOOL_VERSION (re-run the asset packer)",
                  cookedPath, static_cast<unsigned int>(schema));
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

/// Computes the verdict for one cooked path from its stamp text, or null
/// when it has none (no cache involvement).
std::uint32_t compute_generation_verdict(const char *cookedPath,
                                         char *stampText) noexcept {
  if (stampText == nullptr) {
    // Never-certified content (hand-placed, legacy, or test assets) stays
    // loadable; the notice keeps the gap visible without failing loads.
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "no cook stamp; generation not validated: %s", cookedPath);
    core::log_message(core::LogLevel::Info, "assets", message);
    return kVerdictOk;
  }
  return validate_stamp_outputs(cookedPath, stampText);
}

} // namespace

bool cooked_asset_generation_ok(const char *cookedPath) noexcept {
  if (cookedPath == nullptr) {
    return false;
  }

  // The stamp is small and read on every check; the outputs it certifies
  // are what the cache saves hashing again. Its key is its content and
  // when it was written: a clean recook of a torn asset writes the same
  // text the rejected cook did, and only the write tells them apart.
  char stampPath[512] = {};
  std::unique_ptr<char[]> stampText{};
  std::size_t stampSize = 0U;
  const bool hasStamp = build_stamp_path(cookedPath, stampPath) &&
                        read_stamp_file(stampPath, &stampText, &stampSize);
  std::uint64_t stampKey = kNoStampKey;
  if (hasStamp) {
    std::uint64_t hash = core::kFnv1a64Offset;
    for (std::size_t i = 0U; i < stampSize; ++i) {
      hash =
          core::fnv1a_64_append(hash, static_cast<std::uint8_t>(stampText[i]));
    }
    std::error_code timeError;
    const auto writeTime = std::filesystem::last_write_time(
        std::filesystem::path(stampPath), timeError);
    if (!timeError) {
      hash = core::fnv1a_64_append_u64(
          hash,
          static_cast<std::uint64_t>(writeTime.time_since_epoch().count()));
    }
    // A stamp that hashes to the no-stamp key only costs a revalidation.
    stampKey = hash & kStampKeyMask;
  }

  const std::uint64_t pathHash = core::fnv1a_64(cookedPath);
  std::size_t slot = kMaxVerdictEntries;
  for (std::size_t i = 0U; i < kMaxVerdictEntries; ++i) {
    std::uint64_t current = g_verdictPaths[i].load(std::memory_order_acquire);
    if (current == 0ULL) {
      std::uint64_t expected = 0ULL;
      if (g_verdictPaths[i].compare_exchange_strong(
              expected, pathHash, std::memory_order_acq_rel)) {
        slot = i;
        break;
      }
      current = expected;
    }
    if (current == pathHash) {
      const std::uint64_t cached =
          g_verdictValues[i].load(std::memory_order_acquire);
      const auto verdict = static_cast<std::uint32_t>(cached & 3U);
      if ((verdict != kVerdictPending) && ((cached >> 2U) == stampKey)) {
        return verdict == kVerdictOk;
      }
      // Pending on another thread, or cached under a stamp since
      // rewritten: validate now, and record the result for this stamp.
      slot = i;
      break;
    }
  }

  const std::uint32_t verdict = compute_generation_verdict(
      cookedPath, hasStamp ? stampText.get() : nullptr);
  if (slot < kMaxVerdictEntries) {
    g_verdictValues[slot].store(pack_verdict(stampKey, verdict),
                                std::memory_order_release);
  }
  return verdict == kVerdictOk;
}

namespace {

/// Whether a stamp's DEP_HASH path names the file whole: the cook writes
/// one on another volume as an absolute path.
bool is_absolute_stamp_path(const char *path) noexcept {
  const bool drive = (((path[0] >= 'A') && (path[0] <= 'Z')) ||
                      ((path[0] >= 'a') && (path[0] <= 'z'))) &&
                     (path[1] == ':');
  return (path[0] == '/') || (path[0] == '\\') || drive;
}

/// Writes into `outDependency` the first file the stamp's DEP_HASH lines
/// record whose bytes no longer match, and returns true; false when every
/// readable dependency matches or there is no stamp. The cook records
/// every file it read beside the source, so these lines are the asset's
/// whole dependency set. A dependency that cannot be read is not
/// reported, the same rule as a source that cannot be: a shipped build
/// carries cooked assets without what they were cooked from.
bool find_changed_dependency(const char *cookedPath,
                             char (&outDependency)[512]) noexcept {
  char stampPath[512] = {};
  std::unique_ptr<char[]> text{};
  std::size_t size = 0U;
  if (!build_stamp_path(cookedPath, stampPath) ||
      !read_stamp_file(stampPath, &text, &size)) {
    return false;
  }
  const char *slash = std::strrchr(cookedPath, '/');
  const char *backslash = std::strrchr(cookedPath, '\\');
  if ((backslash != nullptr) && ((slash == nullptr) || (backslash > slash))) {
    slash = backslash;
  }
  const int stampDirLength =
      (slash != nullptr) ? static_cast<int>(slash - cookedPath) : 0;

  char *cursor = text.get();
  while ((cursor != nullptr) && (*cursor != '\0')) {
    char *lineEnd = std::strchr(cursor, '\n');
    if (lineEnd != nullptr) {
      *lineEnd = '\0';
    }
    char *line = cursor;
    cursor = (lineEnd != nullptr) ? (lineEnd + 1) : nullptr;
    const std::size_t lineLength = std::strlen(line);
    if ((lineLength > 0U) && (line[lineLength - 1U] == '\r')) {
      line[lineLength - 1U] = '\0';
    }
    // `DEP_HASH <16-hex-hash> <path>`; a malformed line certifies nothing
    // and is left to the packer, which refuses the stamp.
    if ((std::strncmp(line, "DEP_HASH ", 9U) != 0) ||
        (std::strlen(line) < 27U) || (line[25] != ' ')) {
      continue;
    }
    char hashText[17] = {};
    std::memcpy(hashText, line + 9, 16U);
    std::uint64_t recordedHash = 0ULL;
    if (!parse_hex_u64(hashText, &recordedHash)) {
      continue;
    }
    const char *recordedPath = line + 26;
    char dependencyPath[512] = {};
    const int written =
        (is_absolute_stamp_path(recordedPath) || (stampDirLength == 0))
            ? std::snprintf(dependencyPath, sizeof(dependencyPath), "%s",
                            recordedPath)
            : std::snprintf(dependencyPath, sizeof(dependencyPath), "%.*s/%s",
                            stampDirLength, cookedPath, recordedPath);
    if ((written <= 0) ||
        (written >= static_cast<int>(sizeof(dependencyPath)))) {
      continue;
    }
    std::uint64_t currentHash = 0ULL;
    if (hash_file_bytes(dependencyPath, &currentHash) &&
        (currentHash != recordedHash)) {
      std::memcpy(outDependency, dependencyPath,
                  static_cast<std::size_t>(written) + 1U);
      return true;
    }
  }
  return false;
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
  std::uint64_t currentSourceHash = 0ULL;
  if (read_meta_source_record(cookedPath, sourcePath, &recordedSourceHash) &&
      hash_file_bytes(sourcePath, &currentSourceHash) &&
      (currentSourceHash != recordedSourceHash)) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "stale cooked asset (source changed since last cook, "
                  "re-run the asset packer): %s",
                  cookedPath);
    core::log_message(core::LogLevel::Warning, "assets", message);
    return;
  }

  char dependency[512] = {};
  if (find_changed_dependency(cookedPath, dependency)) {
    char message[1152] = {};
    std::snprintf(message, sizeof(message),
                  "stale cooked asset (dependency %s changed since last "
                  "cook, re-run the asset packer): %s",
                  dependency, cookedPath);
    core::log_message(core::LogLevel::Warning, "assets", message);
  }
}

void reset_cooked_asset_stale_warnings() noexcept {
  for (std::size_t i = 0U; i < kMaxCheckedAssets; ++i) {
    g_checkedPaths[i].store(0ULL, std::memory_order_release);
  }
  for (std::size_t i = 0U; i < kMaxVerdictEntries; ++i) {
    g_verdictValues[i].store(0ULL, std::memory_order_release);
    g_verdictPaths[i].store(0ULL, std::memory_order_release);
  }
}

} // namespace engine::content
