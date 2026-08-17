// Implements the editor content browser panel: cached-index folder/search
// views, type filters, typed Open dispatch, drag-spawn, and the mesh/gltf
// import settings inspector. Split out of editor.cpp (REVIEW_FINDINGS A3);
// rebuilt on the index/filter-cache backend for issue #157.

#include "editor_panels_assets.h"

#include "editor_asset_index.h"
#include "editor_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
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

#include "engine/core/atomic_file.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/world.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

/// Draw import settings inspector for mesh assets.
/// Reads the .meta.json sidecar, displays current import settings, and
/// allows editing. On modification only the importSettings field is
/// spliced into the existing document (every other field survives) and
/// the file is replaced atomically; the packer detects the changed
/// import-settings hash on the next cook.
void draw_import_settings_inspector(const char *assetPath) noexcept {
  if (assetPath == nullptr || assetPath[0] == '\0') {
    return;
  }

  const char *dot = std::strrchr(assetPath, '.');
  if (dot == nullptr) {
    return;
  }
  // Accept .mesh or .gltf / .glb source files.
  const bool isMesh = (std::strcmp(dot, ".mesh") == 0);
  const bool isGltf =
      (std::strcmp(dot, ".gltf") == 0) || (std::strcmp(dot, ".glb") == 0);
  if (!isMesh && !isGltf) {
    return;
  }

  // Resolve meta path: <assetPath>.meta.json
  char metaPath[1024] = {};
  std::snprintf(metaPath, sizeof(metaPath), "%s.meta.json", assetPath);

  std::FILE *metaFile = nullptr;
#ifdef _WIN32
  if (fopen_s(&metaFile, metaPath, "rb") != 0) {
    metaFile = nullptr;
  }
#else
  metaFile = std::fopen(metaPath, "rb");
#endif
  if (metaFile == nullptr) {
    ImGui::TextDisabled("No .meta.json found");
    return;
  }

  std::fseek(metaFile, 0, SEEK_END);
  const long fileSize = std::ftell(metaFile);
  std::fseek(metaFile, 0, SEEK_SET);

  if (fileSize <= 0 || fileSize > 65536) {
    std::fclose(metaFile);
    return;
  }

  std::vector<char> metaBuffer(static_cast<std::size_t>(fileSize) + 1U, '\0');
  const std::size_t readCount = std::fread(
      metaBuffer.data(), 1U, static_cast<std::size_t>(fileSize), metaFile);
  std::fclose(metaFile);
  metaBuffer[readCount] = '\0';

  core::JsonParser parser{};
  if (!parser.parse(metaBuffer.data(), readCount)) {
    ImGui::TextDisabled("Failed to parse .meta.json");
    return;
  }

  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    ImGui::TextDisabled("Invalid .meta.json structure");
    return;
  }

  int meshIndex = 0;
  int primitiveIndex = 0;
  float scaleFactor = 1.0F;
  int upAxis = 1;
  bool generateNormals = false;

  const core::JsonValue *importObj =
      parser.get_object_field(*root, "importSettings");
  if ((importObj != nullptr) &&
      (importObj->type == core::JsonValue::Type::Object)) {
    {
      const core::JsonValue *v =
          parser.get_object_field(*importObj, "meshIndex");
      if (v != nullptr) {
        std::uint32_t tmp = 0U;
        if (parser.as_uint(*v, &tmp)) {
          meshIndex = static_cast<int>(tmp);
        }
      }
    }
    {
      const core::JsonValue *v =
          parser.get_object_field(*importObj, "primitiveIndex");
      if (v != nullptr) {
        std::uint32_t tmp = 0U;
        if (parser.as_uint(*v, &tmp)) {
          primitiveIndex = static_cast<int>(tmp);
        }
      }
    }
    {
      const core::JsonValue *v =
          parser.get_object_field(*importObj, "scaleFactor");
      if (v != nullptr) {
        parser.as_float(*v, &scaleFactor);
      }
    }
    {
      const core::JsonValue *v = parser.get_object_field(*importObj, "upAxis");
      if (v != nullptr) {
        std::uint32_t tmp = 1U;
        if (parser.as_uint(*v, &tmp)) {
          upAxis = static_cast<int>(tmp);
        }
      }
    }
    {
      const core::JsonValue *v =
          parser.get_object_field(*importObj, "generateNormals");
      if (v != nullptr) {
        parser.as_bool(*v, &generateNormals);
      }
    }
  }

  ImGui::Separator();
  if (!ImGui::CollapsingHeader("Import Settings",
                               ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  bool changed = false;
  changed |= ImGui::InputInt("Mesh Index", &meshIndex);
  changed |= ImGui::InputInt("Primitive Index", &primitiveIndex);
  changed |= ImGui::DragFloat("Scale Factor", &scaleFactor, 0.01F, 0.001F,
                              1000.0F, "%.6g");

  const char *axisLabels[] = {"X (0)", "Y (1)", "Z (2)"};
  if (upAxis >= 0 && upAxis <= 2) {
    changed |= ImGui::Combo("Up Axis", &upAxis, axisLabels, 3);
  }
  changed |= ImGui::Checkbox("Generate Normals", &generateNormals);

  if (!changed) {
    return;
  }

  if (meshIndex < 0) {
    meshIndex = 0;
  }
  if (primitiveIndex < 0) {
    primitiveIndex = 0;
  }
  if (scaleFactor < 0.001F) {
    scaleFactor = 0.001F;
  }

  // Parse-update-preserve: splice only the importSettings value into the
  // original document so schema, output mappings, and unknown
  // forward-compatible fields survive, validate the result, and replace
  // the file atomically (audit H-21; the old path truncated the meta to
  // an importSettings-only stub with fopen "wb").
  char newSettings[512] = {};
  std::snprintf(newSettings, sizeof(newSettings),
                "{\n"
                "    \"meshIndex\": %d,\n"
                "    \"primitiveIndex\": %d,\n"
                "    \"scaleFactor\": %.6g,\n"
                "    \"upAxis\": %d,\n"
                "    \"generateNormals\": %s\n"
                "  }",
                meshIndex, primitiveIndex, static_cast<double>(scaleFactor),
                upAxis, generateNormals ? "true" : "false");

  // The meta read path caps documents at 64 KiB; this leaves headroom
  // for the spliced settings block. Editor UI runs single-threaded.
  static char updatedDocument[80U * 1024U];
  std::size_t updatedLength = 0U;
  bool staged = core::json_replace_top_level_field(
      metaBuffer.data(), readCount, "importSettings", newSettings,
      updatedDocument, sizeof(updatedDocument), &updatedLength);
  if (staged) {
    core::JsonParser validator{};
    staged = validator.parse(updatedDocument, updatedLength) &&
             (validator.root() != nullptr) &&
             (validator.root()->type == core::JsonValue::Type::Object);
  }
  if (staged) {
    staged = core::atomic_write_file(metaPath, updatedDocument, updatedLength);
  }
  if (!staged) {
    core::log_message(core::LogLevel::Error, "editor",
                      "import settings save failed — .meta.json preserved");
  }
}

/// Every filterable AssetKind, in the fixed order the type-filter row and
/// the "N kinds" persistence mask both use.
constexpr AssetKind kFilterKinds[] = {
    AssetKind::Mesh,     AssetKind::Texture,   AssetKind::Material,
    AssetKind::Script,   AssetKind::Scene,     AssetKind::Animation,
    AssetKind::AnimationController, AssetKind::Sound, AssetKind::Other,
};

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
/// stand-in for #150's authoritative dependency graph, which will index
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
    if ((entry == nullptr) || (entry->kind != AssetKind::Scene)) {
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
/// Rename/Move/Duplicate/Delete/Reimport/Find Dependencies stay disabled
/// (issue #157 depends on #150's stable asset identity and dependency
/// graph for those to be safe — see the destructive-action cut line).
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
               asset_kind_label(entry.kind), entry.name);
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

  if ((entry.kind == AssetKind::Mesh) && (entry.virtualPath[0] != '\0') &&
      ImGui::BeginDragDropSource()) {
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
    const AssetKind kind = kFilterKinds[i];
    bool enabled = (browser.filter.typeMask & asset_kind_bit(kind)) != 0U;
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Checkbox(asset_kind_label(kind), &enabled)) {
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
