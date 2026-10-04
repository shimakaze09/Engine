// Implements the document reference scanner and the catalog's table of
// document-to-asset edges.

#include "engine/content/asset_references.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "engine/content/asset_identity.h"
#include "engine/core/diagnostic.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/vfs.h"

namespace engine::content {
namespace {

constexpr const char *kLogChannel = "assets";

/// Deeper than any document nests; bounds the walk of a hostile one.
constexpr int kMaxScanDepth = 64;

/// The longest string worth decoding: a catalogued path fits in an
/// AssetMetadata's filePath, and AssetRef text is shorter still.
constexpr std::size_t kMaxScannedString = 260U;

struct ScanState final {
  const core::JsonParser *parser = nullptr;
  DocumentReferenceVisitor visit = nullptr;
  void *userData = nullptr;
};

void visit_string(const ScanState &state,
                  const core::JsonValue &value) noexcept {
  char text[kMaxScannedString] = {};
  if (!state.parser->copy_string_strict(value, text, sizeof(text))) {
    return;
  }
  DocumentReference reference{};
  if (parse_asset_ref(text, &reference.ref)) {
    reference.isRef = true;
  } else {
    reference.text = text;
  }
  state.visit(reference, state.userData);
}

void scan_value(const ScanState &state, const core::JsonValue &value,
                int depth) noexcept {
  if (depth > kMaxScanDepth) {
    return;
  }
  switch (value.type) {
  case core::JsonValue::Type::String:
    visit_string(state, value);
    return;
  case core::JsonValue::Type::Object: {
    const std::size_t count = state.parser->object_size(value);
    for (std::size_t i = 0U; i < count; ++i) {
      core::JsonValue key{};
      core::JsonValue member{};
      if (state.parser->get_object_member(value, i, &key, &member)) {
        scan_value(state, member, depth + 1);
      }
    }
    return;
  }
  case core::JsonValue::Type::Array: {
    const std::size_t count = state.parser->array_size(value);
    for (std::size_t i = 0U; i < count; ++i) {
      core::JsonValue element{};
      if (state.parser->get_array_element(value, i, &element)) {
        scan_value(state, element, depth + 1);
      }
    }
    return;
  }
  default:
    return;
  }
}

bool ref_less(const core::AssetRef &a, const core::AssetRef &b) noexcept {
  if (a.guid.high != b.guid.high) {
    return a.guid.high < b.guid.high;
  }
  if (a.guid.low != b.guid.low) {
    return a.guid.low < b.guid.low;
  }
  return a.localId < b.localId;
}

/// A catalogued asset's identity, for a lookup by ref.
struct RefEntry final {
  core::AssetRef ref{};
  AssetId id = kInvalidAssetId;
};

/// Resolves AssetRefs to catalogued ids: through a table sorted once when
/// many documents are indexed together, or a scan of the records for one.
struct RefResolver final {
  const AssetCatalog *catalog = nullptr;
  const RefEntry *sorted = nullptr;
  std::size_t sortedCount = 0U;

  AssetId resolve(const core::AssetRef &ref) const noexcept {
    if (sorted == nullptr) {
      const AssetMetadata *record = find_asset_metadata_by_ref(catalog, ref);
      return (record != nullptr) ? record->assetId : kInvalidAssetId;
    }
    const RefEntry *end = sorted + sortedCount;
    const RefEntry *found = std::lower_bound(
        sorted, end, ref, [](const RefEntry &entry, const core::AssetRef &key) {
          return ref_less(entry.ref, key);
        });
    return ((found != end) && (found->ref == ref)) ? found->id
                                                   : kInvalidAssetId;
  }
};

/// The references one document makes, collected as the scanner visits it.
struct DocumentTargets final {
  static constexpr std::size_t kMaxTargets = 4096U;
  const RefResolver *resolver = nullptr;
  AssetId ids[kMaxTargets] = {};
  std::size_t count = 0U;
  std::size_t dangling = 0U;
  std::size_t dropped = 0U;
};

void collect_target(const DocumentReference &reference,
                    void *userData) noexcept {
  auto &targets = *static_cast<DocumentTargets *>(userData);
  AssetId id = kInvalidAssetId;
  if (reference.isRef) {
    id = targets.resolver->resolve(reference.ref);
    if (id == kInvalidAssetId) {
      ++targets.dangling;
      return;
    }
  } else {
    // Text is a reference only when it is a catalogued asset's path; a
    // name, a state or a clip label is not.
    if (std::strchr(reference.text, '/') == nullptr) {
      return;
    }
    const AssetMetadata *record =
        find_asset_metadata_by_path(targets.resolver->catalog, reference.text);
    if (record == nullptr) {
      return;
    }
    id = record->assetId;
  }
  for (std::size_t i = 0U; i < targets.count; ++i) {
    if (targets.ids[i] == id) {
      return;
    }
  }
  if (targets.count == DocumentTargets::kMaxTargets) {
    ++targets.dropped;
    return;
  }
  targets.ids[targets.count++] = id;
}

/// Reads and indexes one document record, adding to `report`.
void index_one(AssetCatalog *catalog, const AssetMetadata &document,
               const RefResolver &resolver, DocumentTargets &targets,
               DocumentIndexReport *report) noexcept {
  const AssetId documentId = document.assetId;
  const char *path = document.filePath.data();
  void *data = nullptr;
  std::size_t size = 0U;
  const core::Status read = core::vfs_read_binary_bounded(
      path, kMaxReferencingDocumentBytes, &data, &size);
  if (!read.succeeded()) {
    core::log_path_diagnostic(core::LogLevel::Warning, kLogChannel, path,
                              "the document could not be read, or is larger "
                              "than 4 MB, so its references are not indexed");
    static_cast<void>(
        set_document_references(catalog, documentId, nullptr, 0U));
    ++report->failed;
    return;
  }
  targets.resolver = &resolver;
  targets.count = 0U;
  targets.dangling = 0U;
  targets.dropped = 0U;
  const bool scanned = scan_document_references(
      static_cast<const char *>(data), size, &collect_target, &targets);
  core::vfs_free(data);
  if (!scanned) {
    core::log_path_diagnostic(core::LogLevel::Warning, kLogChannel, path,
                              "the document is not JSON, so its references "
                              "are not indexed");
    static_cast<void>(
        set_document_references(catalog, documentId, nullptr, 0U));
    ++report->failed;
    return;
  }
  if (targets.dropped > 0U) {
    core::log_path_diagnostic(core::LogLevel::Warning, kLogChannel, path,
                              "the document references more distinct assets "
                              "than one document's index holds (4096); the "
                              "rest are not indexed");
  }
  if (!set_document_references(catalog, documentId, targets.ids,
                               targets.count)) {
    ++report->failed;
    return;
  }
  ++report->documents;
  report->references += targets.count;
  report->dangling += targets.dangling;
}

} // namespace

bool asset_kind_references_assets(AssetTypeTag kind) noexcept {
  return (kind == AssetTypeTag::Scene) || (kind == AssetTypeTag::Prefab) ||
         (kind == AssetTypeTag::Material) ||
         (kind == AssetTypeTag::AnimationController);
}

bool scan_document_references(const char *text, std::size_t size,
                              DocumentReferenceVisitor visit,
                              void *userData) noexcept {
  if ((text == nullptr) || (visit == nullptr)) {
    return false;
  }
  core::JsonParser parser{};
  if (!parser.parse(text, size) || (parser.root() == nullptr)) {
    return false;
  }
  const core::JsonValue root = *parser.root();
  ScanState state{};
  state.parser = &parser;
  state.visit = visit;
  state.userData = userData;
  scan_value(state, root, 0);
  return true;
}

bool set_document_references(AssetCatalog *catalog, AssetId document,
                             const AssetId *targets,
                             std::size_t count) noexcept {
  if ((catalog == nullptr) || (document == kInvalidAssetId) ||
      ((targets == nullptr) && (count > 0U))) {
    return false;
  }
  std::size_t kept = 0U;
  for (std::size_t i = 0U; i < catalog->referenceCount; ++i) {
    kept += (catalog->references[i].document != document) ? 1U : 0U;
  }
  const std::size_t needed = kept + count;
  if (needed > catalog->referenceCapacity) {
    std::size_t capacity =
        (catalog->referenceCapacity == 0U) ? 256U : catalog->referenceCapacity;
    while (capacity < needed) {
      capacity *= 2U;
    }
    auto *grown = new (std::nothrow) AssetReferenceEdge[capacity];
    if (grown == nullptr) {
      core::log_message(core::LogLevel::Error, kLogChannel,
                        "asset references: out of memory; the document keeps "
                        "the references it had");
      return false;
    }
    core::mem_tracker_alloc(core::MemTag::Assets,
                            capacity * sizeof(AssetReferenceEdge));
    if (catalog->references != nullptr) {
      std::memcpy(grown, catalog->references,
                  catalog->referenceCount * sizeof(AssetReferenceEdge));
      core::mem_tracker_free(core::MemTag::Assets,
                             catalog->referenceCapacity *
                                 sizeof(AssetReferenceEdge));
    }
    delete[] catalog->references;
    catalog->references = grown;
    catalog->referenceCapacity = capacity;
  }
  std::size_t write = 0U;
  for (std::size_t i = 0U; i < catalog->referenceCount; ++i) {
    if (catalog->references[i].document != document) {
      catalog->references[write++] = catalog->references[i];
    }
  }
  for (std::size_t i = 0U; i < count; ++i) {
    const AssetId target = targets[i];
    bool seen = (target == kInvalidAssetId);
    for (std::size_t j = kept; !seen && (j < write); ++j) {
      seen = (catalog->references[j].target == target);
    }
    if (!seen) {
      catalog->references[write++] = AssetReferenceEdge{document, target};
    }
  }
  catalog->referenceCount = write;
  ++catalog->generation;
  return true;
}

std::size_t get_document_references(const AssetCatalog *catalog,
                                    AssetId document, AssetId *outIds,
                                    std::size_t maxIds) noexcept {
  if (catalog == nullptr) {
    return 0U;
  }
  std::size_t found = 0U;
  for (std::size_t i = 0U; i < catalog->referenceCount; ++i) {
    if (catalog->references[i].document == document) {
      if ((outIds != nullptr) && (found < maxIds)) {
        outIds[found] = catalog->references[i].target;
      }
      ++found;
    }
  }
  return found;
}

std::size_t find_asset_referrers(const AssetCatalog *catalog, AssetId target,
                                 AssetId *outIds, std::size_t maxIds) noexcept {
  if (catalog == nullptr) {
    return 0U;
  }
  std::size_t found = 0U;
  for (std::size_t i = 0U; i < catalog->referenceCount; ++i) {
    if (catalog->references[i].target == target) {
      if ((outIds != nullptr) && (found < maxIds)) {
        outIds[found] = catalog->references[i].document;
      }
      ++found;
    }
  }
  return found;
}

std::size_t collect_asset_closure(const AssetCatalog *catalog, AssetId root,
                                  AssetId *outIds,
                                  std::size_t maxIds) noexcept {
  if ((catalog == nullptr) || (root == kInvalidAssetId)) {
    return 0U;
  }
  // The walk's frontier and its seen set in one list: entries before
  // `head` are expanded, the rest wait. Membership is a scan, as the
  // closure of one document is hundreds of assets, not the catalog.
  std::size_t capacity = 256U;
  std::unique_ptr<AssetId[]> found(new (std::nothrow) AssetId[capacity]);
  if (found == nullptr) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "asset closure: out of memory");
    return 0U;
  }
  std::size_t count = 0U;
  const auto add = [&](AssetId id) noexcept {
    if ((id == kInvalidAssetId) || (id == root)) {
      return true;
    }
    for (std::size_t i = 0U; i < count; ++i) {
      if (found[i] == id) {
        return true;
      }
    }
    if (count == capacity) {
      std::unique_ptr<AssetId[]> grown(new (std::nothrow)
                                           AssetId[capacity * 2U]);
      if (grown == nullptr) {
        return false;
      }
      std::memcpy(grown.get(), found.get(), count * sizeof(AssetId));
      found = std::move(grown);
      capacity *= 2U;
    }
    found[count++] = id;
    return true;
  };
  const auto expand = [&](AssetId id) noexcept {
    const AssetMetadata *record = find_asset_metadata(catalog, id);
    if (record != nullptr) {
      for (std::size_t i = 0U; i < record->dependencyCount; ++i) {
        if (!add(record->dependencies[i])) {
          return false;
        }
      }
    }
    for (std::size_t i = 0U; i < catalog->referenceCount; ++i) {
      if ((catalog->references[i].document == id) &&
          !add(catalog->references[i].target)) {
        return false;
      }
    }
    return true;
  };
  bool ok = expand(root);
  for (std::size_t head = 0U; ok && (head < count); ++head) {
    ok = expand(found[head]);
  }
  if (!ok) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "asset closure: out of memory");
    return 0U;
  }
  if (outIds != nullptr) {
    std::memcpy(outIds, found.get(), std::min(count, maxIds) * sizeof(AssetId));
  }
  return count;
}

DocumentIndexReport index_document_references(AssetCatalog *catalog,
                                              AssetId document) noexcept {
  DocumentIndexReport report{};
  const AssetMetadata *record = find_asset_metadata(catalog, document);
  if ((record == nullptr) || !asset_kind_references_assets(record->typeTag)) {
    return report;
  }
  std::unique_ptr<DocumentTargets> targets(new (std::nothrow)
                                               DocumentTargets());
  if (targets == nullptr) {
    ++report.failed;
    return report;
  }
  RefResolver resolver{};
  resolver.catalog = catalog;
  // Copied: indexing writes the reference table, never the records, but
  // the record is read after the document's bytes are.
  const AssetMetadata copy = *record;
  index_one(catalog, copy, resolver, *targets, &report);
  return report;
}

DocumentIndexReport index_catalogued_documents(AssetCatalog *catalog) noexcept {
  DocumentIndexReport report{};
  const std::size_t records = asset_catalog_record_count(catalog);
  if (records == 0U) {
    return report;
  }
  std::unique_ptr<DocumentTargets> targets(new (std::nothrow)
                                               DocumentTargets());
  std::unique_ptr<RefEntry[]> sorted(new (std::nothrow) RefEntry[records]);
  if ((targets == nullptr) || (sorted == nullptr)) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "asset references: out of memory; no document is "
                      "indexed");
    return report;
  }
  // Every ref sorted once, so each reference resolves in a binary search
  // rather than a scan of every record.
  std::size_t sortedCount = 0U;
  for (std::size_t i = 0U; i < records; ++i) {
    const AssetMetadata *record = asset_catalog_record(catalog, i);
    if ((record != nullptr) && core::asset_ref_is_valid(record->ref)) {
      sorted[sortedCount++] = RefEntry{record->ref, record->assetId};
    }
  }
  RefEntry *const entries = sorted.get();
  std::sort(entries, entries + sortedCount,
            [](const RefEntry &a, const RefEntry &b) {
              return ref_less(a.ref, b.ref);
            });
  RefResolver resolver{};
  resolver.catalog = catalog;
  resolver.sorted = entries;
  resolver.sortedCount = sortedCount;
  for (std::size_t i = 0U; i < records; ++i) {
    const AssetMetadata *record = asset_catalog_record(catalog, i);
    if ((record != nullptr) && asset_kind_references_assets(record->typeTag)) {
      const AssetMetadata copy = *record;
      index_one(catalog, copy, resolver, *targets, &report);
    }
  }
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "asset references: %zu documents name %zu assets; %zu "
                "references name nothing catalogued; %zu documents not "
                "indexed",
                report.documents, report.references, report.dangling,
                report.failed);
  core::log_message(core::LogLevel::Info, kLogChannel, message);
  return report;
}

} // namespace engine::content
