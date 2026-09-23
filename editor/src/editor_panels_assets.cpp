// Implements the editor content browser panel: cached-index folder/search
// views, type filters, typed Open dispatch, drag-spawn, and the mesh/gltf
// import settings inspector. Split out of editor.cpp (REVIEW_FINDINGS A3);
// rebuilt on the index/filter-cache backend.

#include "editor_panels_assets.h"

#include "editor_asset_index.h"
#include "editor_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include "editor_import_settings.h"
#include "engine/core/atomic_file.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/world.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

/// Draws the Import Settings inspector for a mesh source.
///
/// Only for a source (.gltf / .glb), because the authored sidecar lives
/// beside the source and that is the file the cook reads. A cooked
/// ".mesh" is derived: it has no settings of its own to edit, and the
/// editor will offer them on it again once cooked outputs record which
/// source produced them, rather than by guessing from the filename.
void draw_import_settings_inspector(const char *assetPath) noexcept {
  if ((assetPath == nullptr) || (assetPath[0] == '\0')) {
    return;
  }
  const content::AssetClassification classification =
      content::classify_asset_path(assetPath);
  if ((classification.tag != content::AssetTypeTag::Mesh) ||
      !classification.source) {
    return;
  }

  // The sidecar is read once per selection, not per frame.
  const ImportSettingsDocument *doc = import_settings_for_asset(assetPath);
  if (doc == nullptr) {
    return;
  }
  switch (doc->state) {
  case ImportSettingsDocument::State::Missing:
    ImGui::TextDisabled("Not imported: no .meta beside this asset");
    return;
  case ImportSettingsDocument::State::Unreadable:
    ImGui::TextDisabled("The .meta beside this asset could not be read");
    return;
  case ImportSettingsDocument::State::Malformed:
    ImGui::TextDisabled("The .meta beside this asset is malformed");
    return;
  case ImportSettingsDocument::State::Valid:
    break;
  }

  ImGui::Separator();
  if (!ImGui::CollapsingHeader("Import Settings",
                               ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  content::MeshImportSettings edited = doc->settings;
  int meshIndex = static_cast<int>(edited.meshIndex);
  int primitiveIndex = static_cast<int>(edited.primitiveIndex);
  int upAxis = static_cast<int>(edited.upAxis);

  bool changed = false;
  changed |= ImGui::InputInt("Mesh Index", &meshIndex);
  changed |= ImGui::InputInt("Primitive Index", &primitiveIndex);
  changed |= ImGui::DragFloat("Scale Factor", &edited.scaleFactor, 0.01F,
                              0.001F, 1000.0F, "%.6g");
  const char *axisLabels[] = {"X (0)", "Y (1)", "Z (2)"};
  if ((upAxis >= 0) && (upAxis <= 2)) {
    changed |= ImGui::Combo("Up Axis", &upAxis, axisLabels, 3);
  }
  changed |= ImGui::Checkbox("Generate Normals", &edited.generateNormals);

  if (!changed) {
    return;
  }

  edited.meshIndex = static_cast<std::int32_t>((meshIndex < 0) ? 0 : meshIndex);
  edited.primitiveIndex =
      static_cast<std::int32_t>((primitiveIndex < 0) ? 0 : primitiveIndex);
  edited.upAxis = static_cast<std::int32_t>(upAxis);
  if (edited.scaleFactor < 0.001F) {
    edited.scaleFactor = 0.001F;
  }

  if (!save_import_settings(assetPath, edited)) {
    core::log_message(core::LogLevel::Error, "editor",
                      "import settings save failed — the .meta on disk is "
                      "unchanged");
  }
}

/// Every filterable asset type in table order, which is also the bit
/// order of the persisted type mask.
constexpr content::AssetTypeTag kFilterKinds[] = {
#define ENGINE_ASSET_FILTER_KIND(Tag, label, policy, action, sources, cooked) \
  content::AssetTypeTag::Tag,
    ENGINE_ASSET_TYPE_TABLE(ENGINE_ASSET_FILTER_KIND)
#undef ENGINE_ASSET_FILTER_KIND
};
static_assert(std::size(kFilterKinds) == content::kAssetTypeCount,
              "one filter checkbox per asset type");

/// Result of the last "Find Usages" scan, shown in a modal popup. Populated
/// only by an explicit context-menu click — never per frame.
struct FindUsagesState final {
  bool armed = false;
  char targetName[kMaxAssetIndexName] = {};
  static constexpr std::size_t kMaxMatches = 16U;
  char matches[kMaxMatches][kMaxAssetIndexPath] = {};
  std::size_t matchCount = 0U;
};
FindUsagesState g_findUsages{};

/// True when the (small, already-bounded-size) file at `path` contains
/// `needle` as a raw byte substring.
bool file_contains_substring(const char *path, const char *needle) noexcept {
  std::FILE *file = nullptr;
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
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  constexpr long kMaxScanBytes = 4L * 1024L * 1024L;
  if ((size <= 0) || (size > kMaxScanBytes)) {
    std::fclose(file);
    return false;
  }
  std::vector<char> buffer(static_cast<std::size_t>(size) + 1U, '\0');
  const std::size_t readCount =
      std::fread(buffer.data(), 1U, static_cast<std::size_t>(size), file);
  std::fclose(file);
  buffer[readCount] = '\0';
  return std::strstr(buffer.data(), needle) != nullptr;
}

/// Scans every indexed Scene entry for a reference to `target`'s virtual
/// path; explicitly user-triggered from the context menu (never per frame)
/// and bounded by the index size, so the O(scenes) file scan is acceptable
/// here even though it would not be on a draw-loop hot path. A lightweight
/// stand-in authoritative dependency graph, which will index
/// usages for every asset kind instead of scene-file substring search.
void run_find_usages(const AssetIndexEntry &target) noexcept {
  g_findUsages = FindUsagesState{};
  std::snprintf(g_findUsages.targetName, sizeof(g_findUsages.targetName), "%s",
               target.name);
  g_findUsages.armed = true;
  if (target.virtualPath[0] == '\0') {
    return;
  }
  const std::size_t count = asset_index_count();
  for (std::size_t i = 0U;
       (i < count) && (g_findUsages.matchCount < FindUsagesState::kMaxMatches);
       ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry == nullptr) || (entry->kind != content::AssetTypeTag::Scene)) {
      continue;
    }
    if (file_contains_substring(entry->osPath, target.virtualPath)) {
      std::snprintf(g_findUsages.matches[g_findUsages.matchCount],
                   kMaxAssetIndexPath, "%s", entry->osPath);
      ++g_findUsages.matchCount;
    }
  }
  ImGui::OpenPopup("Find Usages");
}

void draw_find_usages_popup() noexcept {
  if (ImGui::BeginPopupModal("Find Usages", &g_findUsages.armed,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Scene references to %s:", g_findUsages.targetName);
    ImGui::Separator();
    if (g_findUsages.matchCount == 0U) {
      ImGui::TextDisabled("No scene references found in the indexed content.");
    }
    for (std::size_t i = 0U; i < g_findUsages.matchCount; ++i) {
      ImGui::TextUnformatted(g_findUsages.matches[i]);
    }
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

/// Context menu for one browsed entry. Open/Show in Folder/Copy
/// Reference/Find Usages are implemented against production entry points;
/// Rename/Move/Duplicate/Delete/Reimport/Find Dependencies stay disabled:
/// they need a stable asset identity and dependency graph to be safe.
void draw_context_menu(const AssetIndexEntry &entry) noexcept {
  if (!ImGui::BeginPopupContextItem()) {
    return;
  }
  if (ImGui::MenuItem("Open")) {
    static_cast<void>(execute_asset_open(entry));
  }
  if (ImGui::MenuItem("Show in Folder")) {
    content_browser_navigate(entry.folder);
  }
  if (ImGui::MenuItem("Copy Reference")) {
    const char *reference =
        (entry.virtualPath[0] != '\0') ? entry.virtualPath : entry.osPath;
    ImGui::SetClipboardText(reference);
  }
  if (ImGui::MenuItem("Find Usages")) {
    run_find_usages(entry);
  }
  ImGui::Separator();
  ImGui::MenuItem("Find Dependencies (needs #150)", nullptr, false, false);
  ImGui::MenuItem("Rename... (needs #150)", nullptr, false, false);
  ImGui::MenuItem("Move... (needs #150)", nullptr, false, false);
  ImGui::MenuItem("Duplicate (needs #150)", nullptr, false, false);
  ImGui::MenuItem("Delete (needs #150)", nullptr, false, false);
  ImGui::MenuItem("Reimport (needs #150)", nullptr, false, false);
  ImGui::EndPopup();
}

/// Draws one folder row in the folder-scoped view; double-click navigates
/// into it (recorded in the back/forward history).
void draw_folder_row(const char *folderOsPath) noexcept {
  const std::filesystem::path path(folderOsPath);
  const std::string name = path.filename().string();
  char label[300] = {};
  std::snprintf(label, sizeof(label), "[Folder] %s", name.c_str());

  ImGui::PushID(folderOsPath);
  if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick) &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    content_browser_navigate(folderOsPath);
  }
  ImGui::PopID();
}

/// Draws one asset row: thumbnail (when cooked), name, drag source for
/// meshes (preserving the pinned ASSET_VIRTUAL_PATH viewport-drop
/// contract), single-click select, double-click typed Open, and the
/// context menu.
void draw_asset_row(const AssetIndexEntry &entry) noexcept {
  ImGui::PushID(entry.osPath);

  if (entry.hasThumbnail) {
    const renderer::DeviceTextureHandle tex =
        load_thumbnail_texture(entry.osPath);
    const std::uint64_t imguiTex = imgui_texture_id(tex);
    if (imguiTex != 0U) {
      ImGui::Image(static_cast<ImTextureID>(imguiTex), ImVec2(20.0F, 20.0F));
      ImGui::SameLine();
    }
  }

  char label[256] = {};
  std::snprintf(label, sizeof(label), "[%s] %s",
               content::asset_type_label(entry.kind), entry.name);
  const bool isSelected =
      std::strcmp(editor_session().selectedAssetPath, entry.osPath) == 0;

  if (ImGui::Selectable(label, isSelected,
                        ImGuiSelectableFlags_AllowDoubleClick)) {
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      static_cast<void>(execute_asset_open(entry));
    } else {
      std::snprintf(editor_session().selectedAssetPath,
                    sizeof(editor_session().selectedAssetPath), "%s",
                    entry.osPath);
    }
  }

  if ((entry.kind == content::AssetTypeTag::Mesh) && !entry.isSource &&
      (entry.virtualPath[0] != '\0') && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("ASSET_VIRTUAL_PATH", entry.virtualPath,
                              std::strlen(entry.virtualPath) + 1U);
    ImGui::TextUnformatted(entry.name);
    ImGui::EndDragDropSource();
  }

  draw_context_menu(entry);
  ImGui::PopID();
}

/// Type-filter checkbox row; toggling a box mutates and persists the
/// session's browser filter mask (an infrequent user action, not a
/// per-frame write).
void draw_type_filters(ContentBrowserState &browser) noexcept {
  for (std::size_t i = 0U; i < std::size(kFilterKinds); ++i) {
    if (i != 0U) {
      ImGui::SameLine();
    }
    const content::AssetTypeTag kind = kFilterKinds[i];
    bool enabled = (browser.filter.typeMask & asset_kind_bit(kind)) != 0U;
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Checkbox(content::asset_type_label(kind), &enabled)) {
      if (enabled) {
        browser.filter.typeMask |= asset_kind_bit(kind);
      } else {
        browser.filter.typeMask &= ~asset_kind_bit(kind);
      }
      content_browser_state_persist();
    }
    ImGui::PopID();
  }
}

/// Navigation/rescan/search toolbar shared by the folder and flat-search
/// views.
void draw_toolbar(ContentBrowserState &browser) noexcept {
  ImGui::BeginDisabled(!content_browser_can_go_back());
  if (ImGui::Button("<")) {
    content_browser_go_back();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!content_browser_can_go_forward());
  if (ImGui::Button(">")) {
    content_browser_go_forward();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) {
    static_cast<void>(rebuild_asset_index());
  }
  ImGui::SameLine();
  const char *shownFolder =
      (browser.filter.folder[0] != '\0') ? browser.filter.folder : "/";
  ImGui::TextDisabled("%s", shownFolder);

  bool flatSearch = browser.filter.flatSearch;
  if (ImGui::Checkbox("Search all folders", &flatSearch)) {
    browser.filter.flatSearch = flatSearch;
  }

  char query[sizeof(browser.filter.query)] = {};
  std::snprintf(query, sizeof(query), "%s", browser.filter.query);
  ImGui::SetNextItemWidth(-1.0F);
  if (ImGui::InputTextWithHint("##content_browser_search",
                               "Search by name or path...", query,
                               sizeof(query))) {
    std::snprintf(browser.filter.query, sizeof(browser.filter.query), "%s",
                 query);
  }

  draw_type_filters(browser);
}

} // namespace

void draw_asset_browser_panel() noexcept {
  if (!ImGui::Begin("Assets")) {
    ImGui::End();
    return;
  }

  // Cold: only runs once per process (or on the explicit Rescan button),
  // never per frame.
  if (!asset_index_built()) {
    static_cast<void>(rebuild_asset_index());
  }
  content_browser_state_load_once();

  ContentBrowserState &browser = editor_session().contentBrowser;
  draw_toolbar(browser);
  ImGui::Separator();

  // Change-driven: both caches only recompute when the filter/folder or
  // the index generation actually changed since their last apply.
  refresh_asset_filter_cache(browser.filter, &browser.filterCache);

  if (!browser.filter.flatSearch) {
    refresh_child_folder_cache(browser.filter.folder,
                               &browser.childFolderCache);
    for (const std::string &child : browser.childFolderCache.children) {
      draw_folder_row(child.c_str());
    }
    if (!browser.childFolderCache.children.empty()) {
      ImGui::Separator();
    }
  }

  for (const std::size_t matchIndex : browser.filterCache.matches) {
    const AssetIndexEntry *entry = asset_index_entry(matchIndex);
    if (entry != nullptr) {
      draw_asset_row(*entry);
    }
  }

  draw_find_usages_popup();

  if (editor_session().selectedAssetPath[0] != '\0') {
    ImGui::Separator();
    ImGui::TextWrapped("Selected: %s", editor_session().selectedAssetPath);

    const std::uint64_t thumbTex = imgui_texture_id(
        load_thumbnail_texture(editor_session().selectedAssetPath));
    if (thumbTex != 0U) {
      ImGui::Image(static_cast<ImTextureID>(thumbTex), ImVec2(64.0F, 64.0F));
    }

    draw_import_settings_inspector(editor_session().selectedAssetPath);
  }

  ImGui::End();
}

} // namespace engine::editor
