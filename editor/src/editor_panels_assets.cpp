// Implements the editor content browser panel: cached-index folder/search
// views, type filters, typed Open dispatch, drag-spawn, the right-click
// menus with their Create name prompt, and the mesh/gltf import settings
// inspector. Split out of editor.cpp (REVIEW_FINDINGS A3);
// rebuilt on the index/filter-cache backend.

#include "editor_panels_assets.h"
#include "editor_panels_main.h"

#include "editor_asset_create.h"
#include "editor_asset_index.h"
#include "editor_asset_labels.h"
#include "editor_asset_place.h"
#include "editor_asset_usages.h"
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
#include "engine/content/asset_import_settings.h"
#include "engine/content/asset_metadata.h"
#include "engine/core/atomic_file.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/world.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

/// Draws a texture source's import settings and saves an edit at once:
/// the texture loader reads them, and a saved sidecar reloads the texture.
void draw_texture_import_settings(
    const char *assetPath, const content::TextureImportSettings &current) {
  content::TextureImportSettings edited = current;
  int colorSpace = static_cast<int>(edited.colorSpace);
  int filter = static_cast<int>(edited.filter);
  int wrap = static_cast<int>(edited.wrap);
  const char *colorSpaceLabels[] = {"Auto (by material slot)", "sRGB (colour)",
                                    "Linear (data)"};
  const char *filterLabels[] = {"Linear", "Nearest (pixel art)"};
  const char *wrapLabels[] = {"Repeat", "Clamp"};
  bool changed = false;
  changed |= ImGui::Combo("Color Space", &colorSpace, colorSpaceLabels, 3);
  changed |= ImGui::Checkbox("Generate Mip Maps", &edited.generateMips);
  changed |= ImGui::Combo("Filter", &filter, filterLabels, 2);
  changed |= ImGui::Combo("Wrap", &wrap, wrapLabels, 2);
  if (!changed) {
    return;
  }
  edited.colorSpace =
      static_cast<content::TextureColorSpaceSetting>(colorSpace);
  edited.filter = static_cast<content::TextureFilterSetting>(filter);
  edited.wrap = static_cast<content::TextureWrapSetting>(wrap);
  if (!save_import_settings(assetPath, edited)) {
    core::log_message(core::LogLevel::Error, "editor",
                      "import settings save failed — the .meta on disk is "
                      "unchanged");
  }
}

/// Draws the Import Settings inspector for a source whose type has import
/// settings (asset_import_settings.h): a mesh's, which its cook reads, or
/// a texture's, which its load applies.
///
/// Only for a source, because the authored sidecar lives beside the
/// source. A cooked ".mesh" is derived: it has no settings of its own to
/// edit, and the editor will offer them on it again once cooked outputs
/// record which source produced them, rather than by guessing from the
/// filename.
void draw_import_settings_inspector(const char *assetPath) noexcept {
  if ((assetPath == nullptr) || (assetPath[0] == '\0')) {
    return;
  }
  const content::AssetClassification classification =
      content::classify_asset_path(assetPath);
  const content::ImportSettingsKind kind =
      content::import_settings_kind(classification.tag);
  if ((kind == content::ImportSettingsKind::None) || !classification.source) {
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
  if (kind == content::ImportSettingsKind::Texture) {
    draw_texture_import_settings(assetPath, doc->textureSettings);
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

/// Searches the indexed documents for references to `target`, by the
/// persistent reference the catalog holds for it (what scenes, prefabs and
/// materials write) and by its path (what scripts and controllers use).
void run_find_usages(const AssetIndexEntry &target) noexcept {
  request_find_usages(
      target, runtime::editor_asset_ref(
                  content::make_asset_id_from_path(target.virtualPath)));
}

/// The name prompt a Create menu item opens, as Godot's FileSystem dock
/// asks for a new folder's, scene's or script's name before writing it.
struct NewAssetPrompt final {
  bool openRequested = false;
  NewAssetKind kind = NewAssetKind::Folder;
  char folder[kMaxAssetIndexPath] = {};
  char name[kMaxNewAssetName + 1U] = {};
  /// Why the last Create was refused, shown until the name changes.
  char error[320] = {};
  /// Gives the name field the keyboard: when the prompt opens, and after a
  /// refusal, since Enter ends the field's editing.
  bool focusName = false;
};

NewAssetPrompt g_newAsset{};

constexpr const char *kNewAssetPopup = "Create Asset";

/// Draws the Create submenu's items; the one chosen asks for a name for a
/// new asset of its kind in `folder` ("" is the asset root).
void draw_create_menu(const char *folder) noexcept {
  if (!ImGui::BeginMenu("Create")) {
    return;
  }
  for (const NewAssetKind kind :
       {NewAssetKind::Folder, NewAssetKind::Material, NewAssetKind::Scene,
        NewAssetKind::LuaScript}) {
    char label[64] = {};
    std::snprintf(label, sizeof(label), "%s...", new_asset_label(kind));
    if (ImGui::MenuItem(label)) {
      g_newAsset.openRequested = true;
      g_newAsset.kind = kind;
      std::snprintf(g_newAsset.folder, sizeof(g_newAsset.folder), "%s",
                    (folder != nullptr) ? folder : "");
      std::snprintf(g_newAsset.name, sizeof(g_newAsset.name), "%s",
                    new_asset_default_name(kind));
      g_newAsset.error[0] = '\0';
    }
  }
  ImGui::EndMenu();
}

/// Draws the name prompt while it is open. Create (or Enter) makes the
/// asset and selects it; a refusal keeps the prompt open with the reason.
void draw_new_asset_prompt() noexcept {
  if (g_newAsset.openRequested) {
    g_newAsset.openRequested = false;
    g_newAsset.focusName = true;
    ImGui::OpenPopup(kNewAssetPopup);
  }
  if (!ImGui::BeginPopupModal(kNewAssetPopup, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::Text("Name of the new %s:", new_asset_label(g_newAsset.kind));
  if (g_newAsset.focusName) {
    g_newAsset.focusName = false;
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(editor_px(280.0F));
  bool create = ImGui::InputText(
      "##new_asset_name", g_newAsset.name, sizeof(g_newAsset.name),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  if (ImGui::IsItemEdited()) {
    g_newAsset.error[0] = '\0';
  }
  if (g_newAsset.error[0] != '\0') {
    ImGui::TextWrapped("%s", g_newAsset.error);
  }
  create = ImGui::Button("Create") || create;
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    ImGui::CloseCurrentPopup();
  } else if (create) {
    const NewAssetResult result =
        create_new_asset(g_newAsset.kind, g_newAsset.folder, g_newAsset.name);
    if (result.failure == NewAssetFailure::None) {
      if (g_newAsset.kind != NewAssetKind::Folder) {
        std::snprintf(editor_session().selectedAssetPath,
                      sizeof(editor_session().selectedAssetPath), "%s",
                      result.osPath);
      }
      ImGui::CloseCurrentPopup();
    } else {
      std::snprintf(g_newAsset.error, sizeof(g_newAsset.error), "%s",
                    new_asset_failure_text(result.failure));
      g_newAsset.focusName = true;
    }
  }
  ImGui::EndPopup();
}

/// Context menu for one browsed entry: Open, Show in Folder, Copy
/// Reference and Find Usages. Rename, Move, Duplicate and Delete wait for
/// asset moves that keep every reference whole.
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
  ImGui::EndPopup();
}

/// Draws one folder row in the folder-scoped view; double-click navigates
/// into it (recorded in the back/forward history), and its menu opens it
/// or creates inside it.
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
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Open")) {
      content_browser_navigate(folderOsPath);
    }
    draw_create_menu(folderOsPath);
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

/// Draws one asset row: thumbnail (when cooked), name, drag source for
/// meshes (preserving the pinned ASSET_VIRTUAL_PATH viewport-drop
/// contract), single-click select, double-click typed Open, and the
/// context menu. A search result also names the folder it lives in, since
/// results come from every folder at once.
void draw_asset_row(const AssetIndexEntry &entry, bool showFolder) noexcept {
  ImGui::PushID(entry.osPath);

  if (entry.hasThumbnail) {
    const renderer::DeviceTextureHandle tex =
        load_thumbnail_texture(entry.osPath);
    const std::uint64_t imguiTex = imgui_texture_id(tex);
    if (imguiTex != 0U) {
      ImGui::Image(static_cast<ImTextureID>(imguiTex),
                   ImVec2(editor_px(20.0F), editor_px(20.0F)));
      ImGui::SameLine();
    }
  }

  // Display only: an overlong label truncates, the row keeps its identity
  // through entry.osPath.
  char label[512] = {};
  const char *slash = std::strrchr(entry.virtualPath, '/');
  if (showFolder && (slash != nullptr)) {
    std::snprintf(label, sizeof(label), "[%s] %s   in %.*s",
                  content::asset_type_label(entry.kind), entry.name,
                  static_cast<int>(slash - entry.virtualPath),
                  entry.virtualPath);
  } else {
    std::snprintf(label, sizeof(label), "[%s] %s",
                  content::asset_type_label(entry.kind), entry.name);
  }
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

  // A prefab, mesh or model drags into the Scene view or the Entities
  // panel to place it.
  asset_place_drag_source(entry);

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
  // The index notices no change made outside the editor, so Refresh stays
  // a visible button as well as a menu item, named as the menu names it.
  same_line_if_button_fits("Refresh");
  if (ImGui::Button("Refresh")) {
    static_cast<void>(rebuild_asset_index());
  }
  ImGui::SameLine();
  const char *shownFolder =
      (browser.filter.folder[0] != '\0') ? browser.filter.folder : "/";
  ImGui::TextDisabled("%s", shownFolder);

  char query[sizeof(browser.filter.query)] = {};
  std::snprintf(query, sizeof(query), "%s", browser.filter.query);
  ImGui::SetNextItemWidth(-1.0F);
  if (ImGui::InputTextWithHint("##content_browser_search",
                               "Search the whole project (l:label)...", query,
                               sizeof(query))) {
    std::snprintf(browser.filter.query, sizeof(browser.filter.query), "%s",
                 query);
  }

  draw_type_filters(browser);
}

} // namespace

void draw_asset_browser_panel() noexcept {
  if (!ImGui::Begin(kAssetsWindow)) {
    ImGui::End();
    return;
  }

  // Cold: only runs once per process (or on the explicit Refresh button),
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

  const bool searching = browser.filter.query[0] != '\0';
  if (!searching) {
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
      draw_asset_row(*entry, searching);
    }
  }
  if (searching && browser.filterCache.matches.empty()) {
    ImGui::TextDisabled("No assets match \"%s\"", browser.filter.query);
  }

  // Right-clicking the empty space, as in Unity's Project window.
  if (ImGui::BeginPopupContextWindow("assets_space_menu",
                                     ImGuiPopupFlags_MouseButtonRight |
                                         ImGuiPopupFlags_NoOpenOverItems)) {
    draw_create_menu(browser.filter.folder);
    ImGui::Separator();
    if (ImGui::MenuItem("Refresh")) {
      static_cast<void>(rebuild_asset_index());
    }
    ImGui::EndPopup();
  }
  draw_new_asset_prompt();
  draw_find_usages_popup();

  if (editor_session().selectedAssetPath[0] != '\0') {
    ImGui::Separator();
    ImGui::TextWrapped("Selected: %s", editor_session().selectedAssetPath);

    const std::uint64_t thumbTex = imgui_texture_id(
        load_thumbnail_texture(editor_session().selectedAssetPath));
    if (thumbTex != 0U) {
      ImGui::Image(static_cast<ImTextureID>(thumbTex),
                   ImVec2(editor_px(64.0F), editor_px(64.0F)));
    }

    draw_asset_labels_row(editor_session().selectedAssetPath);
    draw_import_settings_inspector(editor_session().selectedAssetPath);
  }

  ImGui::End();
}

} // namespace engine::editor
