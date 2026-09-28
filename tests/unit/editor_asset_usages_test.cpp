// Verifies Find Usages against an indexed scratch tree: a scene, a prefab
// and a material that name an asset by its AssetRef are found (the search
// that matched only the path in scenes found none of them), a script named
// by path is still found, a primary reference matches its sub-assets while
// a sub-asset reference is exact, an unrelated asset and a longer path
// that begins with the target's are not matched, the result cap is
// reported, and a document too large to search is skipped with a warning.
// On a headless ImGui frame, a request made from inside another window's
// ID scope, as a row's context menu makes it, opens the Find Usages
// window where the Assets panel draws it (opened in the requester's scope,
// the window never appeared).

#include "editor_asset_index.h"
#include "editor_asset_usages.h"

#include "engine/content/asset_identity.h"
#include "engine/core/logging.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "../test_harness.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;

constexpr const char *kRoot = "assets/engine_asset_usages_test";
constexpr const char *kGuid = "5a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";
constexpr const char *kOtherGuid = "0f0e0d0c-0b0a-4908-8706-050403020100";
constexpr const char *kSubId = "432408a2e33116bc";

std::size_t g_warnings = 0U;

void count_warnings(engine::core::LogLevel level, const char *channel,
                    const char *, void *) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (channel != nullptr) &&
      (std::strcmp(channel, "editor") == 0)) {
    ++g_warnings;
  }
}

bool write_file(const char *leaf, const std::string &text) noexcept {
  const std::filesystem::path path = std::filesystem::path(kRoot) / leaf;
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool written =
      std::fwrite(text.data(), 1U, text.size(), file) == text.size();
  return (std::fclose(file) == 0) && written;
}

engine::core::AssetRef parse_ref(const char *text) noexcept {
  engine::core::AssetRef ref{};
  static_cast<void>(engine::content::parse_asset_ref(text, &ref));
  return ref;
}

const AssetIndexEntry *entry_named(const char *leaf) noexcept {
  for (std::size_t i = 0U; i < asset_index_count(); ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if ((entry != nullptr) && (std::strcmp(entry->name, leaf) == 0)) {
      return entry;
    }
  }
  return nullptr;
}

bool usages_hold(const AssetUsages &usages, const char *leaf) noexcept {
  for (std::size_t i = 0U; i < usages.count; ++i) {
    const std::string path(usages.paths[i]);
    if ((path.size() >= std::strlen(leaf)) &&
        (path.compare(path.size() - std::strlen(leaf), std::strlen(leaf),
                      leaf) == 0)) {
      return true;
    }
  }
  return false;
}

/// A scratch project: hero.gltf (kGuid) cooks hero.mesh (kGuid#kSubId);
/// level.scene and hero.prefab use the mesh, skin.mat names the glTF's
/// GUID in upper case, hero.lua is referenced by path, and the rest must
/// not match.
bool build_tree() noexcept {
  std::error_code ec{};
  std::filesystem::remove_all(kRoot, ec);
  std::filesystem::create_directories(kRoot, ec);
  if (ec) {
    return false;
  }
  const std::string meshRef = std::string(kGuid) + "#" + kSubId;
  std::string upperGuid(kGuid);
  for (char &c : upperGuid) {
    c = static_cast<char>(((c >= 'a') && (c <= 'f')) ? (c - 'a' + 'A') : c);
  }
  return write_file("hero.gltf", "{}") && write_file("hero.mesh", "mesh") &&
         write_file("hero.lua", "-- script") &&
         write_file("level.scene",
                    "{\"version\":6,\"entities\":[{\"components\":{"
                    "\"MeshComponent\":{\"mesh\":\"" +
                        meshRef +
                        "\"},\"ScriptComponent\":{\"scriptPath\":\"" +
                        std::string(kRoot) + "/hero.lua\"}}}]}") &&
         write_file("hero.prefab",
                    "{\"entities\":[{\"MeshComponent\":{\"mesh\":\"" +
                        meshRef + "\"}}]}") &&
         write_file("skin.mat",
                    "{\"albedoTexture\":\"" + upperGuid + "\"}") &&
         write_file("other.scene", "{\"mesh\":\"" + std::string(kOtherGuid) +
                                       "#" + kSubId + "\",\"note\":\"" +
                                       std::string(kRoot) +
                                       "/hero.lua.bak\"}") &&
         write_file("mention.scene",
                    "{\"text\":\"not a ref: " + std::string(kGuid) + "\"}");
}

void check_documents(engine::tests::TestContext &t) noexcept {
  const engine::core::AssetRef primary = parse_ref(kGuid);
  const std::string meshText = std::string(kGuid) + "#" + kSubId;
  const engine::core::AssetRef mesh = parse_ref(meshText.c_str());
  const engine::core::AssetRef otherMesh =
      parse_ref((std::string(kGuid) + "#0000000000000001").c_str());
  t.check(engine::core::asset_ref_is_valid(primary) &&
              engine::core::asset_ref_is_valid(mesh) &&
              engine::core::asset_ref_is_valid(otherMesh),
          "the references parse");

  const std::string doc = "{\"mesh\":\"" + meshText + "\"}";
  t.check(document_references_asset(doc.c_str(), "", mesh),
          "a sub-asset reference matches itself exactly");
  t.check(document_references_asset(doc.c_str(), "", primary),
          "a primary reference matches its sub-assets");
  t.check(!document_references_asset(doc.c_str(), "", otherMesh),
          "another sub-asset of the same file does not match");
  t.check(!document_references_asset(
              ("{\"mesh\":\"" + std::string(kGuid) + "\"}").c_str(), "", mesh),
          "a sub-asset reference does not match its file's primary");
  t.check(document_references_asset("{\"s\":\"assets/a.lua\"}", "assets/a.lua",
                                    engine::core::AssetRef{}) &&
              !document_references_asset("{\"s\":\"assets/a.lua.bak\"}",
                                         "assets/a.lua",
                                         engine::core::AssetRef{}) &&
              !document_references_asset("{\"s\":\"x/assets/a.lua\"}",
                                         "assets/a.lua",
                                         engine::core::AssetRef{}),
          "a path matches only as a whole string");
  t.check(!document_references_asset(doc.c_str(), "", engine::core::AssetRef{}) &&
              !document_references_asset(nullptr, "assets/a.lua", mesh),
          "no path and no reference, or no text, match nothing");
}

void check_search(engine::tests::TestContext &t) noexcept {
  t.check(build_tree() && rebuild_asset_index(), "the scratch tree indexes");
  const AssetIndexEntry *heroMesh = entry_named("hero.mesh");
  const AssetIndexEntry *heroSource = entry_named("hero.gltf");
  const AssetIndexEntry *heroScript = entry_named("hero.lua");
  if ((heroMesh == nullptr) || (heroSource == nullptr) ||
      (heroScript == nullptr)) {
    t.fail("the scratch assets are indexed");
    return;
  }

  AssetUsages usages{};
  const std::string meshText = std::string(kGuid) + "#" + kSubId;
  t.check(find_asset_usages(*heroMesh, parse_ref(meshText.c_str()), &usages),
          "the mesh search runs");
  t.check(usages_hold(usages, "level.scene") &&
              usages_hold(usages, "hero.prefab"),
          "a scene and a prefab naming the mesh by reference are found");
  t.check(!usages_hold(usages, "other.scene") &&
              !usages_hold(usages, "mention.scene") &&
              !usages_hold(usages, "skin.mat"),
          "an unrelated reference, a GUID inside prose and the file's other "
          "sub-assets' users are not");

  t.check(find_asset_usages(*heroSource, parse_ref(kGuid), &usages) &&
              usages_hold(usages, "level.scene") &&
              usages_hold(usages, "hero.prefab") &&
              usages_hold(usages, "skin.mat"),
          "the source is used wherever it or its sub-assets are named, a "
          "material's upper-case GUID included");

  t.check(find_asset_usages(*heroScript, engine::core::AssetRef{}, &usages) &&
              (usages.count == 1U) && usages_hold(usages, "level.scene") &&
              !usages.truncated && (usages.skipped == 0U),
          "a script named by path is found, and only there");
}

void check_limits(engine::tests::TestContext &t) noexcept {
  const std::string meshText = std::string(kGuid) + "#" + kSubId;
  const std::string scene = "{\"mesh\":\"" + meshText + "\"}";
  bool written = true;
  for (std::size_t i = 0U; i <= AssetUsages::kMaxAssetUsages; ++i) {
    char leaf[64] = {};
    std::snprintf(leaf, sizeof(leaf), "extra_%02zu.scene", i);
    written = written && write_file(leaf, scene);
  }
  // One byte over the size a search reads.
  std::string large(4U * 1024U * 1024U + 1U, ' ');
  large.replace(0U, scene.size(), scene);
  written = written && write_file("huge.scene", large);
  t.check(written && rebuild_asset_index(), "the larger tree indexes");
  const AssetIndexEntry *heroMesh = entry_named("hero.mesh");
  if (heroMesh == nullptr) {
    t.fail("the mesh is indexed");
    return;
  }
  AssetUsages usages{};
  g_warnings = 0U;
  t.check(find_asset_usages(*heroMesh, parse_ref(meshText.c_str()), &usages),
          "the search runs");
  t.check((usages.count == AssetUsages::kMaxAssetUsages) && usages.truncated,
          "more users than the list holds are reported as truncated");
  t.check((usages.skipped == 1U) && (g_warnings == 1U),
          "a document too large to search is skipped with one warning");
}

void check_window(engine::tests::TestContext &t) noexcept {
  const AssetIndexEntry *heroMesh = entry_named("hero.mesh");
  if (heroMesh == nullptr) {
    t.fail("the mesh is indexed");
    return;
  }
  const std::string meshText = std::string(kGuid) + "#" + kSubId;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0F, 720.0F);
  io.DeltaTime = 0.05F;
  io.IniFilename = nullptr;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  bool open = false;
  for (int frame = 0; frame < 3; ++frame) {
    ImGui::NewFrame();
    ImGui::Begin("Assets");
    if (frame == 0) {
      // The row's context menu: its own popup window, under the row's id.
      ImGui::Begin("Row context");
      ImGui::PushID(7);
      request_find_usages(*heroMesh, parse_ref(meshText.c_str()));
      ImGui::PopID();
      ImGui::End();
    }
    draw_find_usages_popup();
    open = ImGui::IsPopupOpen("Find Usages");
    ImGui::End();
    ImGui::Render();
  }
  t.check(open, "a request from another scope opens the Find Usages window");
  t.check(usages_hold(last_find_usages(), "level.scene"),
          "and it holds the search's result");
  ImGui::DestroyContext();
}

} // namespace

int main() {
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_warnings, nullptr)) {
    return 1;
  }
  engine::tests::TestContext t;
  check_documents(t);
  check_search(t);
  check_limits(t);
  check_window(t);

  std::error_code ec{};
  std::filesystem::remove_all(kRoot, ec);
  engine::core::log_unregister_sink(&count_warnings, nullptr);
  engine::core::shutdown_logging();
  return t.finish("editor_asset_usages");
}
