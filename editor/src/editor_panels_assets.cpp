// Implements the editor asset browser panel, asset tree, and import inspector.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_assets.h"

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
#include <memory>
#include <vector>

#include "engine/core/atomic_file.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

/// Builds the VFS virtual path ("<mount>/<relative>") for a browser entry;
/// false when the entry is outside the configured asset root.
bool make_asset_virtual_path(const std::filesystem::path &entryPath,
                             char *outPath, std::size_t capacity) noexcept {
  std::error_code ec{};
  const std::filesystem::path relative =
      std::filesystem::relative(entryPath, editor_asset_root(), ec);
  if (ec || relative.empty() || (*relative.begin() == "..")) {
    return false;
  }
  const std::string generic = relative.generic_string();
  const int written = std::snprintf(outPath, capacity, "%s/%s",
                                    active_config().assetMount,
                                    generic.c_str());
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// True when the file name ends in .mesh (the drag-to-viewport spawn type).
bool is_spawnable_mesh_asset(const std::string &filename) noexcept {
  const char *dot = std::strrchr(filename.c_str(), '.');
  return (dot != nullptr) && (std::strcmp(dot, ".mesh") == 0);
}

/// Hard bound on asset tree nesting; combined with never following
/// directory symlinks it keeps browsing loop-free even on filesystems
/// with linked or absurdly deep directory layouts.
constexpr std::size_t kMaxAssetTreeDepth = 32U;

void draw_asset_tree(const std::filesystem::path &dir,
                     std::size_t depth) noexcept {
  if (depth >= kMaxAssetTreeDepth) {
    return;
  }
  std::error_code ec{};
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }

    const std::string filename = entry.path().filename().string();

    std::error_code symlinkEc{};
    const bool isSymlink = entry.is_symlink(symlinkEc) && !symlinkEc;
    if (!isSymlink && entry.is_directory(ec) && !ec) {
      if (ImGui::TreeNode(filename.c_str())) {
        draw_asset_tree(entry.path(), depth + 1U);
        ImGui::TreePop();
      }
    } else if (!ec) {
      const std::string relPath = entry.path().string();
      const bool isSelected =
          (std::strcmp(editor_session().selectedAssetPath, relPath.c_str()) == 0);
      if (ImGui::Selectable(filename.c_str(), isSelected)) {
        std::snprintf(editor_session().selectedAssetPath,
                      sizeof(editor_session().selectedAssetPath), "%s",
                      relPath.c_str());
      }
      if (is_spawnable_mesh_asset(filename) &&
          ImGui::BeginDragDropSource()) {
        char virtualPath[512] = {};
        if (make_asset_virtual_path(entry.path(), virtualPath,
                                    sizeof(virtualPath))) {
          ImGui::SetDragDropPayload("ASSET_VIRTUAL_PATH", virtualPath,
                                    std::strlen(virtualPath) + 1U);
        }
        ImGui::TextUnformatted(filename.c_str());
        ImGui::EndDragDropSource();
      }
    }
  }
}

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


} // namespace

void draw_asset_browser_panel() noexcept {
  if (!ImGui::Begin("Assets")) {
    ImGui::End();
    return;
  }

  const std::filesystem::path assetsDir(editor_asset_root());
  std::error_code ec{};
  if (std::filesystem::is_directory(assetsDir, ec) && !ec) {
    draw_asset_tree(assetsDir, 0U);
  } else {
    ImGui::Text("Asset directory not found: %s", editor_asset_root());
  }

  if (editor_session().selectedAssetPath[0] != '\0') {
    ImGui::Separator();
    ImGui::TextWrapped("Selected: %s", editor_session().selectedAssetPath);

      const std::uint32_t thumbTex = load_thumbnail_texture(editor_session().selectedAssetPath);
    if (thumbTex != 0U) {
      ImGui::Image(
          static_cast<ImTextureID>(static_cast<std::uintptr_t>(thumbTex)),
          ImVec2(64.0F, 64.0F));
    }

      draw_import_settings_inspector(editor_session().selectedAssetPath);
  }

  ImGui::End();
}


} // namespace engine::editor
