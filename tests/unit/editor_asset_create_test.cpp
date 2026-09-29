// Verifies creating assets from the Assets panel on a scratch project. A
// name is checked whole (no separators, reserved characters, control
// characters, edge spaces, a trailing dot, or more than 128 bytes). A
// folder is created empty and listed at once, an empty folder included.
// A material of the engine's defaults, an empty scene and a script with
// the empty hooks are each written with the kind's extension (not
// doubled when typed), given a sidecar identity and catalogued. Nothing
// is created outside the project, over an existing name, or over a
// sidecar left by a deleted asset, and a failed write leaves nothing. On
// headless ImGui frames the panel's empty-space menu offers Create and
// Refresh, no row offers an item that cannot run, and the name prompt
// creates the asset or shows why it could not.

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "editor_asset_create.h"
#include "editor_asset_index.h"
#include "editor_panels_assets.h"
#include "editor_session.h"
#include "engine/content/asset_catalog.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"

namespace {

using namespace engine::editor;
namespace ct = engine::content;
namespace fs = std::filesystem;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

bool on_disk(const fs::path &path) noexcept {
  std::error_code ec{};
  return fs::symlink_status(path, ec).type() != fs::file_type::not_found;
}

std::string read_text(const fs::path &path) noexcept {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

/// True when `path` has a readable sidecar with a valid identity and the
/// catalog holds the asset under it.
bool identified(const ct::AssetCatalog *catalog, const fs::path &path,
                const char *virtualPath) noexcept {
  ct::AssetSidecar sidecar{};
  if ((ct::read_asset_sidecar(path.string().c_str(), &sidecar) !=
       ct::SidecarReadResult::Ok) ||
      !ct::asset_guid_is_valid(sidecar.guid)) {
    return false;
  }
  const ct::AssetMetadata *record =
      ct::find_asset_metadata_by_path(catalog, virtualPath);
  return (record != nullptr) &&
         (record->ref == ct::asset_ref_primary(sidecar.guid));
}

bool lists_folder(const char *parent, const fs::path &folder) noexcept {
  AssetChildFolderCache cache{};
  static_cast<void>(refresh_child_folder_cache(parent, &cache));
  for (const std::string &child : cache.children) {
    if (fs::path(child) == folder) {
      return true;
    }
  }
  return false;
}

void check_names() noexcept {
  const std::string longest(kMaxNewAssetName, 'a');
  check(new_asset_name_valid("New Material") && new_asset_name_valid("a") &&
            new_asset_name_valid("rock.v2") &&
            new_asset_name_valid(longest.c_str()),
        "ordinary names, dots inside and the longest name are valid");
  const std::string tooLong(kMaxNewAssetName + 1U, 'a');
  bool allRefused =
      !new_asset_name_valid(tooLong.c_str()) && !new_asset_name_valid(nullptr);
  for (const char *name :
       {"", ".", "..", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b",
        "a|b", "tab\there", " lead", "trail ", "dot."}) {
    allRefused = allRefused && !new_asset_name_valid(name);
  }
  check(allRefused, "every unusable name is refused");
}

void check_create(const ct::AssetCatalog *catalog) noexcept {
  const fs::path root("assets");
  const NewAssetResult folder =
      create_new_asset(NewAssetKind::Folder, "", "Props");
  check((folder.failure == NewAssetFailure::None) &&
            fs::is_directory(root / "Props") &&
            (fs::path(folder.osPath) == root / "Props") &&
            !on_disk(root / "Props.meta"),
        "a folder is created empty, with no sidecar");
  check(lists_folder("", root / "Props"), "the empty folder is listed at once");
  check(create_new_asset(NewAssetKind::Folder, "", "Props").failure ==
            NewAssetFailure::AlreadyExists,
        "a folder that exists is refused");

  const std::string props = (root / "Props").string();
  const NewAssetResult material =
      create_new_asset(NewAssetKind::Material, props.c_str(), "Rock");
  const std::string materialText = read_text(root / "Props/Rock.mat");
  check((material.failure == NewAssetFailure::None) &&
            (fs::path(material.osPath) == root / "Props/Rock.mat") &&
            (materialText.find("\"version\"") != std::string::npos) &&
            (materialText.find("\"shadingModel\"") != std::string::npos),
        "a material is written with the material codec");
  check(identified(catalog, root / "Props/Rock.mat", "assets/Props/Rock.mat"),
        "the material has its identity and is catalogued");
  check(create_new_asset(NewAssetKind::Material, props.c_str(), "Rock.mat")
                .failure == NewAssetFailure::AlreadyExists,
        "a typed extension is not doubled, and the name is taken");

  const NewAssetResult scene =
      create_new_asset(NewAssetKind::Scene, "", "Level");
  std::unique_ptr<engine::runtime::World> loaded(new (std::nothrow)
                                                     engine::runtime::World());
  check((scene.failure == NewAssetFailure::None) && (loaded != nullptr) &&
            engine::runtime::load_scene(*loaded, "assets/Level.scene") &&
            (loaded->alive_entity_count() == 0U) &&
            identified(catalog, root / "Level.scene", "assets/Level.scene"),
        "a scene is written empty, loads, and is catalogued");

  const NewAssetResult script =
      create_new_asset(NewAssetKind::LuaScript, "", "enemy");
  const std::string scriptText = read_text(root / "enemy.lua");
  check(
      (script.failure == NewAssetFailure::None) &&
          (scriptText.find("function M.on_begin_play") != std::string::npos) &&
          (scriptText.find("return M") != std::string::npos) &&
          identified(catalog, root / "enemy.lua", "assets/enemy.lua"),
      "a script is written with the empty hooks and catalogued");

  bool indexed = false;
  for (std::size_t i = 0U; i < asset_index_count(); ++i) {
    indexed =
        indexed || (std::strcmp(asset_index_entry(i)->name, "enemy.lua") == 0);
  }
  check(indexed, "the index is rebuilt with the new asset");
}

void check_refusals() noexcept {
  const fs::path root("assets");
  check((create_new_asset(NewAssetKind::LuaScript, "", "a/b").failure ==
         NewAssetFailure::InvalidName) &&
            !on_disk(root / "a"),
        "an invalid name writes nothing");
  check((create_new_asset(NewAssetKind::Folder, "..", "Escape").failure ==
         NewAssetFailure::OutsideProject) &&
            !on_disk("Escape"),
        "a folder outside the project is refused");
  check(create_new_asset(NewAssetKind::Folder, "assets/missing", "X").failure ==
            NewAssetFailure::OutsideProject,
        "a folder that does not exist is refused");

  {
    std::ofstream orphan(root / "Ghost.lua.meta");
    orphan << "{}";
  }
  check((create_new_asset(NewAssetKind::LuaScript, "", "Ghost").failure ==
         NewAssetFailure::AlreadyExists) &&
            !on_disk(root / "Ghost.lua"),
        "a sidecar left by a deleted asset is never adopted");

  engine::runtime::set_editor_asset_service(nullptr);
  check((create_new_asset(NewAssetKind::Material, "", "Lost").failure ==
         NewAssetFailure::WriteFailed) &&
            !on_disk(root / "Lost.mat") && !on_disk(root / "Lost.mat.meta"),
        "a material that cannot be written leaves nothing");

  const std::string deep(200, 'd');
  const std::string deepFolder = (root / deep).string();
  std::error_code ec{};
  fs::create_directories(root / deep / deep, ec);
  check(!ec && (create_new_asset(NewAssetKind::LuaScript,
                                 (root / deep / deep).string().c_str(),
                                 std::string(120, 'n').c_str())
                    .failure == NewAssetFailure::TooLong),
        "a path that would not fit whole is refused");
  check((create_new_asset(NewAssetKind::Folder,
                          (root / deep / deep).string().c_str(),
                          std::string(120, 'f').c_str())
             .failure == NewAssetFailure::TooLong) &&
            !on_disk(root / deep / deep / std::string(120, 'f')),
        "a folder whose path would not fit whole is refused");
}

/// Draws one frame of the Assets panel and returns the text it and its
/// menus rendered.
std::string assets_frame() noexcept {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(520.0F, 600.0F));
  draw_asset_browser_panel();
  const std::string text = GImGui->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return text;
}

std::string right_click(ImVec2 at) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  static_cast<void>(assets_frame());
  io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
  static_cast<void>(assets_frame());
  io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
  static_cast<void>(assets_frame());
  return assets_frame();
}

void settle() noexcept {
  ImGui::GetIO().AddMousePosEvent(-100.0F, -100.0F);
  ImGui::ClosePopupsExceptModals();
  static_cast<void>(assets_frame());
  static_cast<void>(assets_frame());
}

/// Hovers `at` for a few frames, so a submenu under it opens.
void hover(ImVec2 at) noexcept {
  ImGui::GetIO().AddMousePosEvent(at.x, at.y);
  for (int i = 0; i < 4; ++i) {
    static_cast<void>(assets_frame());
  }
}

void left_click(ImVec2 at) noexcept {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  static_cast<void>(assets_frame());
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  static_cast<void>(assets_frame());
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  static_cast<void>(assets_frame());
  static_cast<void>(assets_frame());
}

ImRect top_popup() noexcept {
  const ImGuiContext &g = *GImGui;
  if (g.OpenPopupStack.empty() || (g.OpenPopupStack.back().Window == nullptr)) {
    return ImRect();
  }
  return g.OpenPopupStack.back().Window->Rect();
}

/// The centre of the `row`th item from the top of `menu`.
ImVec2 menu_row(const ImRect &menu, int row) noexcept {
  const ImGuiStyle &style = ImGui::GetStyle();
  const float step = ImGui::GetFontSize() + style.ItemSpacing.y;
  return ImVec2(menu.Min.x + (menu.GetWidth() * 0.3F),
                menu.Min.y + style.WindowPadding.y +
                    (ImGui::GetFontSize() * 0.5F) +
                    (step * static_cast<float>(row)));
}

void check_panel() noexcept {
  static_cast<void>(rebuild_asset_index());
  settle();
  const ImGuiWindow *const window = ImGui::FindWindowByName(kAssetsWindow);
  check(window != nullptr, "the Assets panel draws");
  if (window == nullptr) {
    return;
  }
  const ImVec2 empty(window->Pos.x + (window->Size.x * 0.5F),
                     window->Pos.y + window->Size.y - 20.0F);
  const std::string menu = right_click(empty);
  check((menu.find("Create") != std::string::npos) &&
            (menu.find("Refresh") != std::string::npos),
        "the empty-space menu offers Create and Refresh");
  check(menu.find("needs #150") == std::string::npos,
        "no item that cannot run is offered");

  // Create is the first item; its first entry is Folder...
  const ImVec2 createRow = menu_row(top_popup(), 0);
  hover(createRow);
  const std::string submenu = assets_frame();
  check((submenu.find("Folder...") != std::string::npos) &&
            (submenu.find("Material...") != std::string::npos) &&
            (submenu.find("Scene...") != std::string::npos) &&
            (submenu.find("Lua Script...") != std::string::npos),
        "Create lists a folder, a material, a scene and a Lua script");
  const ImVec2 folderRow = menu_row(top_popup(), 0);
  hover(ImVec2(folderRow.x, createRow.y));
  left_click(folderRow);
  const std::string prompt = assets_frame();
  check(prompt.find("New Folder") != std::string::npos,
        "Folder... asks for a name, starting from New Folder");

  // A name already taken keeps the prompt open with the reason.
  ImGuiIO &io = ImGui::GetIO();
  io.AddInputCharactersUTF8("Props");
  static_cast<void>(assets_frame());
  io.AddKeyEvent(ImGuiKey_Enter, true);
  static_cast<void>(assets_frame());
  io.AddKeyEvent(ImGuiKey_Enter, false);
  const std::string refused = assets_frame();
  check(refused.find("already there") != std::string::npos,
        "a taken name keeps the prompt open and says why");

  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_A, true);
  static_cast<void>(assets_frame());
  io.AddKeyEvent(ImGuiKey_A, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  io.AddInputCharactersUTF8("Audio");
  static_cast<void>(assets_frame());
  io.AddKeyEvent(ImGuiKey_Enter, true);
  static_cast<void>(assets_frame());
  io.AddKeyEvent(ImGuiKey_Enter, false);
  static_cast<void>(assets_frame());
  static_cast<void>(assets_frame());
  check(fs::is_directory("assets/Audio") && GImGui->OpenPopupStack.empty(),
        "Enter creates the folder and closes the prompt");
  check(lists_folder("", fs::path("assets") / "Audio"),
        "the new folder is listed");
  settle();
}

} // namespace

int main() {
  // The index walks the relative "assets" root; a private working
  // directory keeps it to this suite's own files under ctest -j.
  constexpr const char *kWorkDir = "engine_asset_create_test_wd";
  std::error_code ec{};
  const fs::path previous = fs::current_path(ec);
  fs::remove_all(kWorkDir, ec);
  fs::create_directories(fs::path(kWorkDir) / "assets", ec);
  fs::current_path(kWorkDir, ec);
  if (ec || !engine::core::initialize_logging() ||
      !engine::core::initialize_vfs() ||
      !engine::core::mount("assets", "assets")) {
    return 2;
  }
  std::unique_ptr<ct::AssetCatalog> catalog(new (std::nothrow)
                                                ct::AssetCatalog());
  if (catalog == nullptr) {
    return 3;
  }
  engine::runtime::EngineAssetDatabaseService service{};
  service.catalog = catalog.get();
  engine::runtime::set_editor_asset_service(&service);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 1.0F / 60.0F;
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  check_names();
  check_create(catalog.get());
  check_panel();
  check_refusals();

  engine::runtime::set_editor_asset_service(nullptr);
  ImGui::DestroyContext();
  asset_index_reset();
  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  fs::current_path(previous, ec);
  fs::remove_all(kWorkDir, ec);
  return g_tests.finish("editor_asset_create");
}
