// Expands the asset type table into its descriptors and implements the
// suffix classifier over them. The descriptor order is asserted against
// the enum at compile time so a row and its tag can never disagree.

#include "engine/content/asset_type_table.h"

#include <cctype>
#include <cstring>
#include <iterator>

namespace engine::content {
namespace {

// Each suffix list is stored behind a leading null so an empty list still
// forms a valid array; the descriptors skip that first slot.
#define ENGINE_ASSET_SUFFIX_LIST(...) __VA_ARGS__
#define ENGINE_ASSET_SUFFIX_ARRAYS(Tag, label, policy, action, sources,       \
                                   cooked)                                     \
  constexpr const char *const kSourceSuffixes_##Tag[] = {                      \
      nullptr, ENGINE_ASSET_SUFFIX_LIST sources};                              \
  constexpr const char *const kCookedSuffixes_##Tag[] = {                      \
      nullptr, ENGINE_ASSET_SUFFIX_LIST cooked};
ENGINE_ASSET_TYPE_TABLE(ENGINE_ASSET_SUFFIX_ARRAYS)
#undef ENGINE_ASSET_SUFFIX_ARRAYS

#define ENGINE_ASSET_DESCRIPTOR(Tag, label, policy, action, sources, cooked)  \
  AssetTypeDescriptor{AssetTypeTag::Tag,                                       \
                      label,                                                   \
                      AssetSourcePolicy::policy,                               \
                      AssetPrimaryAction::action,                              \
                      kSourceSuffixes_##Tag + 1,                               \
                      std::size(kSourceSuffixes_##Tag) - 1U,                   \
                      kCookedSuffixes_##Tag + 1,                               \
                      std::size(kCookedSuffixes_##Tag) - 1U},
constexpr AssetTypeDescriptor kDescriptors[] = {
    ENGINE_ASSET_TYPE_TABLE(ENGINE_ASSET_DESCRIPTOR)};
#undef ENGINE_ASSET_DESCRIPTOR
#undef ENGINE_ASSET_SUFFIX_LIST

static_assert(std::size(kDescriptors) == kAssetTypeCount,
              "one descriptor per asset type row");

/// True when every descriptor sits at the index of its own tag.
consteval bool descriptors_follow_enum_order() noexcept {
  for (std::size_t i = 0U; i < std::size(kDescriptors); ++i) {
    if (static_cast<std::size_t>(kDescriptors[i].tag) != i) {
      return false;
    }
  }
  return true;
}
static_assert(descriptors_follow_enum_order(),
              "descriptor order must match the AssetTypeTag order");

/// True when `path` ends with `suffix`, comparing ASCII without case.
bool ends_with_ignoring_case(const char *path, std::size_t pathLength,
                             const char *suffix) noexcept {
  const std::size_t suffixLength = std::strlen(suffix);
  if ((suffixLength == 0U) || (suffixLength > pathLength)) {
    return false;
  }
  const char *tail = path + (pathLength - suffixLength);
  for (std::size_t i = 0U; i < suffixLength; ++i) {
    const int a = std::tolower(static_cast<unsigned char>(tail[i]));
    const int b = std::tolower(static_cast<unsigned char>(suffix[i]));
    if (a != b) {
      return false;
    }
  }
  return true;
}

/// Records `suffix` as the best match so far when it is longer than the
/// current best.
void consider(const char *path, std::size_t pathLength, const char *suffix,
              AssetTypeTag tag, bool source, bool legacy,
              std::size_t *bestLength, AssetClassification *best) noexcept {
  const std::size_t length = std::strlen(suffix);
  if ((length > *bestLength) &&
      ends_with_ignoring_case(path, pathLength, suffix)) {
    *bestLength = length;
    best->tag = tag;
    best->source = source;
    best->legacy = legacy;
  }
}

/// One legacy row in data form.
struct LegacySuffix final {
  const char *suffix;
  AssetTypeTag tag;
  const char *replacement;
};

#define ENGINE_ASSET_LEGACY_ROW(oldSuffix, Tag, newSuffix)                     \
  LegacySuffix{oldSuffix, AssetTypeTag::Tag, newSuffix},
constexpr LegacySuffix kLegacySuffixes[] = {
    ENGINE_ASSET_LEGACY_SUFFIX_TABLE(ENGINE_ASSET_LEGACY_ROW)};
#undef ENGINE_ASSET_LEGACY_ROW

} // namespace

const AssetTypeDescriptor &asset_type_descriptor(AssetTypeTag tag) noexcept {
  const std::size_t index = static_cast<std::size_t>(tag);
  if (index >= kAssetTypeCount) {
    return kDescriptors[static_cast<std::size_t>(AssetTypeTag::Unknown)];
  }
  return kDescriptors[index];
}

const char *asset_type_label(AssetTypeTag tag) noexcept {
  return asset_type_descriptor(tag).label;
}

AssetClassification classify_asset_path(const char *path) noexcept {
  AssetClassification best{};
  if (path == nullptr) {
    return best;
  }
  const std::size_t pathLength = std::strlen(path);
  std::size_t bestLength = 0U;
  for (const AssetTypeDescriptor &row : kDescriptors) {
    for (std::size_t i = 0U; i < row.sourceSuffixCount; ++i) {
      consider(path, pathLength, row.sourceSuffixes[i], row.tag, true, false,
               &bestLength, &best);
    }
    for (std::size_t i = 0U; i < row.cookedSuffixCount; ++i) {
      consider(path, pathLength, row.cookedSuffixes[i], row.tag, false, false,
               &bestLength, &best);
    }
  }
  // A legacy name is strictly longer than the kind suffix it replaced
  // (".scene.json" against ".scene"), so the same longest-match rule
  // picks it without a kind suffix ever losing to one.
  for (const LegacySuffix &row : kLegacySuffixes) {
    consider(path, pathLength, row.suffix, row.tag, true, true, &bestLength,
             &best);
  }
  return best;
}

const char *asset_legacy_replacement_suffix(const char *path) noexcept {
  if (path == nullptr) {
    return nullptr;
  }
  const std::size_t pathLength = std::strlen(path);
  for (const LegacySuffix &row : kLegacySuffixes) {
    if (ends_with_ignoring_case(path, pathLength, row.suffix)) {
      return row.replacement;
    }
  }
  return nullptr;
}

} // namespace engine::content
