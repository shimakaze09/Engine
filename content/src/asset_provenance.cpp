// Implements the cooked-output provenance index by reading the SOURCE_GUID
// and ASSET lines the cook writes into every stamp.

#include "engine/content/asset_provenance.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/core/file_read.h"
#include "engine/core/logging.h"

namespace engine::content {
namespace {

constexpr const char *kStampSuffix = ".cookstamp";
/// A stamp is a short line-oriented manifest; anything larger is not one.
constexpr std::size_t kMaxStampBytes = 256U * 1024U;

/// Parses 16 lowercase hex digits; false for any other shape.
bool parse_hex_u64(const char *text, std::size_t length,
                   std::uint64_t *out) noexcept {
  if (length != 16U) {
    return false;
  }
  std::uint64_t value = 0U;
  for (std::size_t i = 0U; i < length; ++i) {
    const char ch = text[i];
    std::uint64_t digit = 0U;
    if ((ch >= '0') && (ch <= '9')) {
      digit = static_cast<std::uint64_t>(ch - '0');
    } else if ((ch >= 'a') && (ch <= 'f')) {
      digit = static_cast<std::uint64_t>(ch - 'a') + 10U;
    } else {
      return false;
    }
    value = (value << 4U) | digit;
  }
  *out = value;
  return true;
}

/// Records one output's reference; counts an overflow rather than
/// silently dropping it, so a walk that outgrew the index says so.
void record(ProvenanceIndex *index, const std::string &relativePath,
            const AssetRef &ref) noexcept {
  if (index->count >= ProvenanceIndex::kMaxOutputs) {
    ++index->overflowed;
    return;
  }
  if (relativePath.size() >= ProvenanceIndex::kMaxPathLength) {
    ++index->overflowed;
    return;
  }
  ProvenanceIndex::Entry &entry = index->entries[index->count];
  std::memcpy(entry.relativePath, relativePath.c_str(),
              relativePath.size() + 1U);
  entry.ref = ref;
  ++index->count;
}

/// Reads one stamp's provenance lines into the index.
void read_stamp(const std::filesystem::path &stampPath,
                const std::filesystem::path &root,
                ProvenanceIndex *index) noexcept {
  static char buffer[kMaxStampBytes] = {};
  std::size_t size = 0U;
  if (core::read_whole_file(stampPath.string().c_str(), buffer,
                            sizeof(buffer), &size) !=
      core::FileReadResult::Ok) {
    return;
  }

  // Outputs are recorded relative to the stamp's own directory, so they
  // are rebased onto the index's root here.
  std::error_code ec{};
  const std::filesystem::path stampDirectory = stampPath.parent_path();

  AssetGuid sourceGuid{};
  const char *cursor = buffer;
  const char *const end = buffer + size;
  while (cursor < end) {
    const char *lineEnd = static_cast<const char *>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    const char *const stop = (lineEnd != nullptr) ? lineEnd : end;
    const std::string line(cursor, static_cast<std::size_t>(stop - cursor));
    cursor = (lineEnd != nullptr) ? (lineEnd + 1) : end;

    if (line.compare(0U, 12U, "SOURCE_GUID ") == 0) {
      const std::string text = line.substr(12U);
      if (!parse_asset_guid(text.c_str(), &sourceGuid)) {
        sourceGuid = kNilAssetGuid;
      }
      continue;
    }
    if (line.compare(0U, 6U, "ASSET ") != 0) {
      continue;
    }
    // "ASSET <16 hex local id> <path relative to the stamp>".
    if (line.size() < 24U) {
      continue;
    }
    std::uint64_t localId = 0U;
    if (!parse_hex_u64(line.c_str() + 6U, 16U, &localId) ||
        (line[22U] != ' ')) {
      continue;
    }
    if (!asset_guid_is_valid(sourceGuid)) {
      // An ASSET line before a readable SOURCE_GUID names no producer.
      continue;
    }
    const std::filesystem::path output =
        (stampDirectory / line.substr(23U)).lexically_normal();
    const std::filesystem::path relative =
        std::filesystem::relative(output, root, ec);
    if (ec || relative.empty()) {
      ec.clear();
      continue;
    }
    record(index, relative.generic_string(), AssetRef{sourceGuid, localId});
  }
}

} // namespace

bool build_provenance_index(const char *osRoot,
                            ProvenanceIndex *out) noexcept {
  if ((osRoot == nullptr) || (osRoot[0] == '\0') || (out == nullptr)) {
    return false;
  }
  *out = ProvenanceIndex{};

  std::error_code ec{};
  const std::filesystem::path root(osRoot);
  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return false;
  }
  const std::filesystem::recursive_directory_iterator end{};
  for (; it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code kindEc{};
    if (!it->is_regular_file(kindEc) || kindEc) {
      continue;
    }
    const std::string name = it->path().filename().string();
    const std::size_t suffixLength = std::strlen(kStampSuffix);
    if ((name.size() <= suffixLength) ||
        (name.compare(name.size() - suffixLength, suffixLength,
                      kStampSuffix) != 0)) {
      continue;
    }
    read_stamp(it->path(), root, out);
  }

  if (out->overflowed > 0U) {
    char message[192] = {};
    std::snprintf(message, sizeof(message),
                  "asset provenance: %zu cooked output(s) did not fit the "
                  "index; those assets will report as unidentified",
                  out->overflowed);
    core::log_message(core::LogLevel::Error, "assets", message);
  }
  return true;
}

AssetRef provenance_for_output(const ProvenanceIndex &index,
                               const char *relativePath) noexcept {
  if (relativePath == nullptr) {
    return AssetRef{};
  }
  for (std::size_t i = 0U; i < index.count; ++i) {
    if (std::strcmp(index.entries[i].relativePath, relativePath) == 0) {
      return index.entries[i].ref;
    }
  }
  return AssetRef{};
}

} // namespace engine::content
