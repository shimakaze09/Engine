// Implements asset labels in the editor: the sidecar read-modify-write, the
// follow-up to the index and the catalog, and the label row.

#include "editor_asset_labels.h"

#include "editor_asset_index.h"
#include "editor_session.h"

#include "imgui.h"

#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"
#include "engine/runtime/editor_bridge.h"

#include <cstdio>
#include <cstring>

namespace engine::editor {

namespace {

/// The label being typed, and why the last add was refused; both belong
/// to the asset they were typed for and are cleared when it changes.
struct LabelEditState final {
  char assetOsPath[kMaxAssetIndexPath] = {};
  char typed[content::AssetMetadata::kMaxTagLength + 8U] = {};
  const char *problem = nullptr;
};
LabelEditState g_edit{};

const AssetIndexEntry *find_entry(const char *osPath) noexcept {
  for (std::size_t i = 0U; i < asset_index_count(); ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry != nullptr) && (std::strcmp(entry->osPath, osPath) == 0)) {
      return entry;
    }
  }
  return nullptr;
}

void log_label_problem(const char *assetOsPath, const char *what) noexcept {
  char message[kMaxAssetIndexPath + 128U] = {};
  std::snprintf(message, sizeof(message), "labels for %s: %s", assetOsPath,
                what);
  core::log_message(core::LogLevel::Error, "editor", message);
}

} // namespace

bool save_asset_labels(const char *assetOsPath,
                       const content::AssetLabels &labels) noexcept {
  if (assetOsPath == nullptr) {
    return false;
  }
  content::AssetSidecar sidecar{};
  if (content::read_asset_sidecar(assetOsPath, &sidecar) !=
      content::SidecarReadResult::Ok) {
    log_label_problem(assetOsPath, "the asset has no readable sidecar, so "
                                   "there is nowhere to keep them");
    return false;
  }
  const content::AssetLabels before = sidecar.labels;
  sidecar.labels = labels;
  if (!content::write_asset_sidecar(assetOsPath, sidecar)) {
    log_label_problem(assetOsPath, "the sidecar could not be written; the "
                                   "labels it had are kept");
    return false;
  }
  static_cast<void>(set_asset_index_labels(assetOsPath, labels));
  const AssetIndexEntry *entry = find_entry(assetOsPath);
  if ((entry != nullptr) && (entry->virtualPath[0] != '\0')) {
    // The catalog may not know the asset (no runtime service in a tool,
    // or an asset outside the mount); the sidecar is the truth either way.
    static_cast<void>(
        runtime::editor_retag_asset(entry->virtualPath, before, labels));
  }
  return true;
}

void draw_asset_labels_row(const char *assetOsPath) noexcept {
  if ((assetOsPath == nullptr) || (assetOsPath[0] == '\0')) {
    return;
  }
  if (std::strcmp(g_edit.assetOsPath, assetOsPath) != 0) {
    g_edit = LabelEditState{};
    std::snprintf(g_edit.assetOsPath, sizeof(g_edit.assetOsPath), "%s",
                  assetOsPath);
  }
  const AssetIndexEntry *entry = find_entry(assetOsPath);
  if (entry == nullptr) {
    return;
  }
  ImGui::PushID("AssetLabels");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Labels:");
  if (!entry->hasSidecar) {
    ImGui::SameLine();
    ImGui::TextDisabled("labels belong to the source this was made from");
    ImGui::PopID();
    return;
  }

  // Copied first: removing one rewrites the entry being iterated.
  const content::AssetLabels labels = entry->labels;
  for (std::size_t i = 0U; i < labels.count; ++i) {
    ImGui::SameLine();
    char button[content::AssetMetadata::kMaxTagLength + 8U] = {};
    std::snprintf(button, sizeof(button), "%s  x", labels.names[i].data());
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton(button)) {
      content::AssetLabels edited = labels;
      static_cast<void>(
          content::asset_labels_remove(&edited, labels.names[i].data()));
      static_cast<void>(save_asset_labels(assetOsPath, edited));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Remove the label; search for it with l:%s",
                        labels.names[i].data());
    }
    ImGui::PopID();
  }

  ImGui::SetNextItemWidth(editor_px(160.0F));
  if (ImGui::InputTextWithHint("##AddLabel", "Add label", g_edit.typed,
                               sizeof(g_edit.typed),
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
    content::AssetLabels edited = labels;
    if (!content::asset_label_is_valid(g_edit.typed)) {
      g_edit.problem = "A label is 1 to 31 bytes of letters (CJK, kana and "
                       "Hangul included), digits, '_', '-' or '.'.";
    } else if (content::asset_labels_has(labels, g_edit.typed)) {
      g_edit.problem = nullptr;
      g_edit.typed[0] = '\0';
    } else if (!content::asset_labels_add(&edited, g_edit.typed)) {
      g_edit.problem = "An asset carries at most 16 labels.";
    } else if (save_asset_labels(assetOsPath, edited)) {
      g_edit.problem = nullptr;
      g_edit.typed[0] = '\0';
    } else {
      g_edit.problem = "The sidecar could not be written; the Log says why.";
    }
    ImGui::SetKeyboardFocusHere(-1);
  }
  if (g_edit.problem != nullptr) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s", g_edit.problem);
  }
  ImGui::PopID();
}

} // namespace engine::editor
