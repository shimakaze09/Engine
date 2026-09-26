// Implements the cooked-output provenance index by reading the SOURCE_GUID,
// ASSET and DEP_HASH lines the cook writes into every stamp.

#include "engine/content/asset_provenance.h"

#include <algorithm>
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

/// A stamp's dependency run in the index.
struct DependencyRun final {
  std::uint32_t first = 0U;
  std::uint32_t count = 0U;
};

/// Records one output's reference; counts an overflow rather than
/// silently dropping it, so a walk that outgrew the index says so.
void record(ProvenanceIndex *index, const std::string &relativePath,
            const AssetRef &ref, const DependencyRun &run) noexcept {
  if (relativePath.size() >= ProvenanceIndex::kMaxPathLength) {
    ++index->overflowed;
    return;
  }
  ProvenanceIndex::Entry entry{};
  entry.relativePath = relativePath;
  entry.ref = ref;
  entry.firstDependency = run.first;
  entry.dependencyCount = run.count;
  index->entries.push_back(std::move(entry));
}

/// The next line of the stamp text, advancing `cursor`.
std::string next_line(const char *&cursor, const char *end) {
  const char *lineEnd = static_cast<const char *>(
      std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
  const char *const stop = (lineEnd != nullptr) ? lineEnd : end;
  std::string line(cursor, static_cast<std::size_t>(stop - cursor));
  cursor = (lineEnd != nullptr) ? (lineEnd + 1) : end;
  if (!line.empty() && (line.back() == '\r')) {
    line.pop_back();
  }
  return line;
}

/// `path`, relative to the stamp, as a path under `root`; empty when it
/// resolves outside it.
std::string rebase_onto_root(const std::filesystem::path &stampDirectory,
                             const std::string &path,
                             const std::filesystem::path &root) {
  std::error_code ec{};
  const std::filesystem::path joined =
      (stampDirectory / path).lexically_normal();
  const std::filesystem::path relative =
      std::filesystem::relative(joined, root, ec);
  const std::string generic = relative.generic_string();
  if (ec || generic.empty() || (generic.compare(0U, 2U, "..") == 0)) {
    return std::string();
  }
  return generic;
}

/// Appends the ids of the files the stamp's DEP_HASH lines name, as the
/// catalog names them under `mountPrefix`.
DependencyRun read_dependencies(const char *text, std::size_t size,
                                const std::filesystem::path &stampDirectory,
                                const std::filesystem::path &root,
                                const char *mountPrefix,
                                ProvenanceIndex *index) {
  DependencyRun run{static_cast<std::uint32_t>(index->dependencies.size()), 0U};
  if ((mountPrefix == nullptr) || (mountPrefix[0] == '\0')) {
    return run;
  }
  const char *cursor = text;
  const char *const end = text + size;
  while (cursor < end) {
    // "DEP_HASH <16 hex content hash> <path relative to the stamp>".
    const std::string line = next_line(cursor, end);
    if ((line.compare(0U, 9U, "DEP_HASH ") != 0) || (line.size() < 27U) ||
        (line[25U] != ' ')) {
      continue;
    }
    const std::string relative =
        rebase_onto_root(stampDirectory, line.substr(26U), root);
    if (relative.empty()) {
      continue;
    }
    const std::string virtualPath = std::string(mountPrefix) + "/" + relative;
    const AssetId id = make_asset_id_from_path(virtualPath.c_str());
    if (id == kInvalidAssetId) {
      continue;
    }
    index->dependencies.push_back(id);
    ++run.count;
  }
  return run;
}

/// Reads one stamp's provenance lines into the index.
void read_stamp(const std::filesystem::path &stampPath,
                const std::filesystem::path &root, const char *mountPrefix,
                ProvenanceIndex *index) noexcept {
  static char buffer[kMaxStampBytes] = {};
  std::size_t size = 0U;
  if (core::read_whole_file(stampPath.string().c_str(), buffer, sizeof(buffer),
                            &size) != core::FileReadResult::Ok) {
    return;
  }

  // Paths are recorded relative to the stamp's own directory, so they
  // are rebased onto the index's root here.
  const std::filesystem::path stampDirectory = stampPath.parent_path();
  // Every output of the cook shares the files it read; read them first,
  // since the stamp lists them before or after its outputs as it likes.
  const DependencyRun run =
      read_dependencies(buffer, size, stampDirectory, root, mountPrefix, index);

  AssetGuid sourceGuid{};
  const char *cursor = buffer;
  const char *const end = buffer + size;
  while (cursor < end) {
    const std::string line = next_line(cursor, end);

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
    if (!parse_asset_local_id(line.c_str() + 6U, 16U, &localId) ||
        (line[22U] != ' ')) {
      continue;
    }
    if (!asset_guid_is_valid(sourceGuid)) {
      // An ASSET line before a readable SOURCE_GUID names no producer.
      continue;
    }
    const std::string relative =
        rebase_onto_root(stampDirectory, line.substr(23U), root);
    if (relative.empty()) {
      continue;
    }
    record(index, relative, AssetRef{sourceGuid, localId}, run);
  }
}

} // namespace

bool build_provenance_index(const char *osRoot, const char *mountPrefix,
                            ProvenanceIndex *out) noexcept {
  if ((osRoot == nullptr) || (osRoot[0] == '\0') || (out == nullptr)) {
    return false;
  }
  out->entries.clear();
  out->dependencies.clear();
  out->overflowed = 0U;

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
        (name.compare(name.size() - suffixLength, suffixLength, kStampSuffix) !=
         0)) {
      continue;
    }
    read_stamp(it->path(), root, mountPrefix, out);
  }

  // Sorted once, so the walk looks each file up by bisection rather than
  // by scanning every output the stamps named.
  std::sort(
      out->entries.begin(), out->entries.end(),
      [](const ProvenanceIndex::Entry &lhs, const ProvenanceIndex::Entry &rhs) {
        return lhs.relativePath < rhs.relativePath;
      });

  if (out->overflowed > 0U) {
    char message[192] = {};
    std::snprintf(message, sizeof(message),
                  "asset provenance: %zu cooked output path(s) are too long "
                  "to record; those assets will report as unidentified",
                  out->overflowed);
    core::log_message(core::LogLevel::Error, "assets", message);
  }
  return true;
}

const ProvenanceIndex::Entry *
find_provenance_entry(const ProvenanceIndex &index,
                      const char *relativePath) noexcept {
  if (relativePath == nullptr) {
    return nullptr;
  }
  const auto found = std::lower_bound(
      index.entries.begin(), index.entries.end(), relativePath,
      [](const ProvenanceIndex::Entry &entry, const char *wanted) {
        return std::strcmp(entry.relativePath.c_str(), wanted) < 0;
      });
  if ((found == index.entries.end()) ||
      (std::strcmp(found->relativePath.c_str(), relativePath) != 0)) {
    return nullptr;
  }
  return &*found;
}

AssetRef provenance_for_output(const ProvenanceIndex &index,
                               const char *relativePath) noexcept {
  const ProvenanceIndex::Entry *entry =
      find_provenance_entry(index, relativePath);
  return (entry != nullptr) ? entry->ref : AssetRef{};
}

} // namespace engine::content
