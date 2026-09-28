// Implements Find Usages: the whole-string match of an asset's path or
// AssetRef inside a JSON document, the search over the asset index, and
// the window that lists the result.

#include "editor_asset_usages.h"

#include "editor_commands.h"

#include "imgui.h"

#include "engine/content/asset_identity.h"
#include "engine/core/file_read.h"
#include "engine/core/logging.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace engine::editor {

namespace {

/// The most of one document a search reads; a larger file is skipped.
constexpr std::size_t kMaxDocumentBytes = 4U * 1024U * 1024U;

char ascii_lower(char c) noexcept {
  return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c;
}

/// True when the string starting just after an opening quote at `at` is
/// exactly `value` (compared without case when `foldCase`), followed by
/// the closing quote or, when `allowSubAsset`, by '#'.
bool string_matches(const char *at, const char *value, bool foldCase,
                    bool allowSubAsset) noexcept {
  std::size_t i = 0U;
  for (; value[i] != '\0'; ++i) {
    const char a = foldCase ? ascii_lower(at[i]) : at[i];
    const char b = foldCase ? ascii_lower(value[i]) : value[i];
    if ((a == '\0') || (a != b)) {
      return false;
    }
  }
  return (at[i] == '"') || (allowSubAsset && (at[i] == '#'));
}

/// The last search, shown in the Find Usages window. Written only by an
/// explicit request, never per frame.
struct FindUsagesState final {
  bool openRequested = false;
  bool windowOpen = false;
  bool searched = false;
  char targetName[kMaxAssetIndexName] = {};
  AssetUsages usages{};
};
FindUsagesState g_findUsages{};

constexpr const char *kFindUsagesWindow = "Find Usages";

/// The indexed entry at `osPath`, or null when the index no longer has it.
const AssetIndexEntry *find_index_entry(const char *osPath) noexcept {
  const std::size_t count = asset_index_count();
  for (std::size_t i = 0U; i < count; ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry != nullptr) && (std::strcmp(entry->osPath, osPath) == 0)) {
      return entry;
    }
  }
  return nullptr;
}

void log_skipped(const char *path, const char *reason) noexcept {
  char message[kMaxAssetIndexPath + 96U] = {};
  std::snprintf(message, sizeof(message), "Find Usages skipped %s: %s", path,
                reason);
  core::log_message(core::LogLevel::Warning, "editor", message);
}

} // namespace

bool asset_kind_references_assets(content::AssetTypeTag kind) noexcept {
  return (kind == content::AssetTypeTag::Scene) ||
         (kind == content::AssetTypeTag::Prefab) ||
         (kind == content::AssetTypeTag::Material) ||
         (kind == content::AssetTypeTag::AnimationController);
}

bool document_references_asset(const char *text, const char *virtualPath,
                               const core::AssetRef &ref) noexcept {
  if (text == nullptr) {
    return false;
  }
  char refText[content::kAssetRefTextLength + 1U] = {};
  const bool hasRef = core::asset_ref_is_valid(ref) &&
                      content::format_asset_ref(ref, refText, sizeof(refText));
  const bool hasPath = (virtualPath != nullptr) && (virtualPath[0] != '\0');
  if (!hasRef && !hasPath) {
    return false;
  }
  // A primary reference names the whole file, so "<guid>#<id>" (one of
  // its sub-assets) is a usage too; a sub-asset reference is exact.
  const bool primary = (ref.localId == 0U);
  for (const char *quote = std::strchr(text, '"'); quote != nullptr;
       quote = std::strchr(quote + 1, '"')) {
    const char *value = quote + 1;
    if ((hasRef && string_matches(value, refText, true, primary)) ||
        (hasPath && string_matches(value, virtualPath, false, false))) {
      return true;
    }
  }
  return false;
}

bool find_asset_usages(const AssetIndexEntry &target,
                       const core::AssetRef &ref, AssetUsages *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  *out = AssetUsages{};
  const std::unique_ptr<char[]> buffer(new (std::nothrow)
                                           char[kMaxDocumentBytes + 1U]);
  if (buffer == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "Find Usages: out of memory for the search buffer");
    return false;
  }
  const std::size_t count = asset_index_count();
  for (std::size_t i = 0U; i < count; ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry == nullptr) || !asset_kind_references_assets(entry->kind) ||
        (std::strcmp(entry->osPath, target.osPath) == 0)) {
      continue;
    }
    std::size_t size = 0U;
    const core::FileReadResult read = core::read_whole_file(
        entry->osPath, buffer.get(), kMaxDocumentBytes + 1U, &size);
    if (read != core::FileReadResult::Ok) {
      ++out->skipped;
      log_skipped(entry->osPath, (read == core::FileReadResult::TooLarge)
                                     ? "larger than a search reads (4 MB)"
                                     : "could not be read");
      continue;
    }
    if (!document_references_asset(buffer.get(), target.virtualPath, ref)) {
      continue;
    }
    if (out->count == AssetUsages::kMaxAssetUsages) {
      out->truncated = true;
      continue;
    }
    std::snprintf(out->paths[out->count], kMaxAssetIndexPath, "%s",
                  entry->osPath);
    ++out->count;
  }
  return true;
}

void request_find_usages(const AssetIndexEntry &target,
                         const core::AssetRef &ref) noexcept {
  std::snprintf(g_findUsages.targetName, sizeof(g_findUsages.targetName), "%s",
                target.name);
  g_findUsages.searched = find_asset_usages(target, ref, &g_findUsages.usages);
  g_findUsages.openRequested = true;
}

void draw_find_usages_popup() noexcept {
  if (g_findUsages.openRequested) {
    g_findUsages.openRequested = false;
    g_findUsages.windowOpen = true;
    ImGui::OpenPopup(kFindUsagesWindow);
  }
  if (!ImGui::BeginPopupModal(kFindUsagesWindow, &g_findUsages.windowOpen,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  const AssetUsages &usages = g_findUsages.usages;
  ImGui::Text("Documents that use %s:", g_findUsages.targetName);
  ImGui::Separator();
  if (!g_findUsages.searched) {
    ImGui::TextDisabled("The search could not run; the Log says why.");
  } else if (usages.count == 0U) {
    ImGui::TextDisabled("No scene, prefab, material or controller uses it.");
  }
  for (std::size_t i = 0U; i < usages.count; ++i) {
    const AssetIndexEntry *entry = find_index_entry(usages.paths[i]);
    char label[kMaxAssetIndexPath + 32U] = {};
    std::snprintf(label, sizeof(label), "[%s] %s",
                  (entry != nullptr) ? content::asset_type_label(entry->kind)
                                     : "?",
                  usages.paths[i]);
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(label, false,
                          ImGuiSelectableFlags_AllowDoubleClick) &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        (entry != nullptr)) {
      static_cast<void>(execute_asset_open(*entry));
    }
    ImGui::PopID();
  }
  if (usages.truncated) {
    ImGui::TextDisabled("More documents use it than are listed (%zu shown).",
                        usages.count);
  }
  if (usages.skipped > 0U) {
    ImGui::TextDisabled("%zu document(s) could not be searched; the Log "
                        "names them.",
                        usages.skipped);
  }
  if (ImGui::Button("Close")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

const AssetUsages &last_find_usages() noexcept { return g_findUsages.usages; }

} // namespace engine::editor
