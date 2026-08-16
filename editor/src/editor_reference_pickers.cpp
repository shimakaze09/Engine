// Implements the editor's searchable entity/asset reference-picker widgets
// declared in editor_reference_pickers.h.

#include "editor_reference_pickers.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"

#include "editor_session.h"

namespace engine::editor {

namespace {

bool contains_ci(const char *haystack, const char *needle) noexcept {
  if ((haystack == nullptr) || (needle == nullptr)) {
    return false;
  }
  if (needle[0] == '\0') {
    return true;
  }
  const std::size_t haystackLen = std::strlen(haystack);
  const std::size_t needleLen = std::strlen(needle);
  if (needleLen > haystackLen) {
    return false;
  }
  for (std::size_t start = 0U; start <= (haystackLen - needleLen); ++start) {
    std::size_t i = 0U;
    for (; i < needleLen; ++i) {
      const unsigned char a =
          static_cast<unsigned char>(std::tolower(haystack[start + i]));
      const unsigned char b =
          static_cast<unsigned char>(std::tolower(needle[i]));
      if (a != b) {
        break;
      }
    }
    if (i == needleLen) {
      return true;
    }
  }
  return false;
}

} // namespace

std::size_t filter_entities_by_name(
    const runtime::World &world, const char *query,
    EntityPickerResult *outResults, std::size_t maxResults,
    bool (*predicate)(const runtime::World &, runtime::Entity) noexcept) noexcept {
  if ((outResults == nullptr) || (maxResults == 0U)) {
    return 0U;
  }
  const char *effectiveQuery = (query != nullptr) ? query : "";
  std::size_t written = 0U;
  world.for_each_alive([&](runtime::Entity entity) noexcept {
    if (written >= maxResults) {
      return;
    }
    if ((predicate != nullptr) && !predicate(world, entity)) {
      return;
    }
    runtime::NameComponent name{};
    if (!world.get_name_component(entity, &name)) {
      return;
    }
    if (!contains_ci(name.name, effectiveQuery)) {
      return;
    }
    const runtime::PersistentId persistentId = world.persistent_id(entity);
    if (persistentId == runtime::kInvalidPersistentId) {
      return;
    }
    EntityPickerResult &result = outResults[written];
    result.persistentId = persistentId;
    std::snprintf(result.name, sizeof(result.name), "%s", name.name);
    ++written;
  });
  return written;
}

bool entity_reference_is_live(const runtime::World &world,
                              runtime::PersistentId persistentId) noexcept {
  if (persistentId == runtime::kInvalidPersistentId) {
    return false;
  }
  return world.find_entity_by_persistent_id(persistentId) !=
        runtime::kInvalidEntity;
}

bool draw_entity_reference_picker(
    const char *label, runtime::PersistentId *value,
    bool (*predicate)(const runtime::World &, runtime::Entity) noexcept) noexcept {
  if ((label == nullptr) || (value == nullptr)) {
    return false;
  }
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }

  bool changed = false;
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);

  const bool resolved = entity_reference_is_live(*world, *value);
  if ((*value != runtime::kInvalidPersistentId) && !resolved) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.2F, 1.0F),
                       "Missing entity (id %u)",
                       static_cast<unsigned>(*value));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      *value = runtime::kInvalidPersistentId;
      changed = true;
    }
  }

  char previewText[40] = "<none>";
  if (resolved) {
    const runtime::Entity resolvedEntity =
        world->find_entity_by_persistent_id(*value);
    runtime::NameComponent name{};
    if (world->get_name_component(resolvedEntity, &name)) {
      std::snprintf(previewText, sizeof(previewText), "%s", name.name);
    } else {
      std::snprintf(previewText, sizeof(previewText), "Entity %u",
                    static_cast<unsigned>(*value));
    }
  } else if (*value != runtime::kInvalidPersistentId) {
    std::snprintf(previewText, sizeof(previewText), "<missing>");
  }

  static char query[128] = {};
  if (ImGui::BeginCombo("##picker", previewText)) {
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##search", "Search...", query, sizeof(query));

    if (ImGui::Selectable("<none>", *value == runtime::kInvalidPersistentId)) {
      *value = runtime::kInvalidPersistentId;
      changed = true;
    }

    constexpr std::size_t kMaxHits = 64U;
    EntityPickerResult hits[kMaxHits];
    const std::size_t hitCount =
        filter_entities_by_name(*world, query, hits, kMaxHits, predicate);
    for (std::size_t i = 0U; i < hitCount; ++i) {
      ImGui::PushID(static_cast<int>(i));
      const bool isSelected = (*value == hits[i].persistentId);
      if (ImGui::Selectable(hits[i].name, isSelected)) {
        *value = hits[i].persistentId;
        changed = true;
      }
      ImGui::PopID();
    }
    if (hitCount == kMaxHits) {
      ImGui::TextDisabled("More results than shown -- refine the search.");
    }
    ImGui::EndCombo();
  }

  ImGui::PopID();
  return changed;
}

bool draw_asset_reference_picker(const char *label,
                                 renderer::AssetTypeTag typeTag,
                                 std::uint64_t *value) noexcept {
  if ((label == nullptr) || (value == nullptr)) {
    return false;
  }

  bool changed = false;
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);

  char displayPath[260] = {};
  const bool resolved =
      (*value != 0ULL) &&
      runtime::editor_asset_display_path(*value, displayPath,
                                         sizeof(displayPath));
  if ((*value != 0ULL) && !resolved) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.2F, 1.0F),
                       "Missing asset (id 0x%016llx)",
                       static_cast<unsigned long long>(*value));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      *value = 0ULL;
      changed = true;
    }
  }

  static char query[128] = {};
  const char *previewText =
      resolved ? displayPath : ((*value == 0ULL) ? "<none>" : "<missing>");
  if (ImGui::BeginCombo("##picker", previewText)) {
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##search", "Search...", query, sizeof(query));

    if (ImGui::Selectable("<none>", *value == 0ULL)) {
      *value = 0ULL;
      changed = true;
    }

    constexpr std::size_t kMaxHits = 64U;
    runtime::EditorAssetSearchResult hits[kMaxHits];
    const std::size_t hitCount =
        runtime::editor_query_assets(typeTag, query, hits, kMaxHits);
    for (std::size_t i = 0U; i < hitCount; ++i) {
      ImGui::PushID(static_cast<int>(i));
      const bool isSelected = (*value == hits[i].assetId);
      if (ImGui::Selectable(hits[i].path, isSelected)) {
        *value = hits[i].assetId;
        changed = true;
      }
      ImGui::PopID();
    }
    if (hitCount == kMaxHits) {
      ImGui::TextDisabled("More results than shown -- refine the search.");
    }
    ImGui::EndCombo();
  }

  ImGui::PopID();
  return changed;
}

namespace {

/// Hard bound on the path picker's directory walk (mirrors the asset
/// browser panel's kMaxAssetTreeDepth so both stay loop-free on the same
/// class of pathological filesystem layouts).
constexpr std::size_t kMaxPickerScanDepth = 32U;

/// Collects up to maxResults VFS virtual paths ("<mount>/rel/...") under
/// the editor asset root ending in `extension`, filtered by a case-
/// insensitive substring `query`; returns the count written.
std::size_t scan_paths_by_extension(const std::filesystem::path &dir,
                                    std::size_t depth, const char *extension,
                                    const char *query,
                                    char outPaths[][196],
                                    std::size_t maxResults,
                                    std::size_t written) noexcept {
  if ((depth >= kMaxPickerScanDepth) || (written >= maxResults)) {
    return written;
  }
  std::error_code ec{};
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    return written;
  }
  for (const auto &entry : it) {
    if (written >= maxResults) {
      break;
    }
    std::error_code entryEc{};
    if (entry.is_directory(entryEc) && !entryEc &&
        !entry.is_symlink(entryEc)) {
      written = scan_paths_by_extension(entry.path(), depth + 1U, extension,
                                        query, outPaths, maxResults, written);
      continue;
    }
    if (!entry.is_regular_file(entryEc) || entryEc) {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if ((extension != nullptr) && (extension[0] != '\0')) {
      const std::size_t extLen = std::strlen(extension);
      if ((filename.size() < extLen) ||
          (filename.compare(filename.size() - extLen, extLen, extension) !=
           0)) {
        continue;
      }
    }
    std::error_code relEc{};
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), editor_asset_root(), relEc);
    if (relEc || relative.empty()) {
      continue;
    }
    const std::string generic = relative.generic_string();
    if (!contains_ci(generic.c_str(), query)) {
      continue;
    }
    std::snprintf(outPaths[written], 196, "%s/%s",
                  engine::active_config().assetMount, generic.c_str());
    ++written;
  }
  return written;
}

/// Resolves a "<mount>/rel/..." virtual path to an OS path under the asset
/// root, or an empty path when it does not carry the expected mount prefix
/// (never true for anything this picker itself wrote, but a defensive
/// check against hand-edited/legacy component data).
std::filesystem::path resolve_virtual_path(const char *virtualPath) noexcept {
  const char *mount = engine::active_config().assetMount;
  const std::size_t mountLen = std::strlen(mount);
  const std::size_t pathLen = std::strlen(virtualPath);
  if ((pathLen <= mountLen + 1U) ||
      (std::strncmp(virtualPath, mount, mountLen) != 0) ||
      (virtualPath[mountLen] != '/')) {
    return {};
  }
  return std::filesystem::path(editor_asset_root()) /
        (virtualPath + mountLen + 1U);
}

} // namespace

bool draw_path_reference_picker(const char *label, char *pathBuffer,
                                std::size_t pathBufferSize,
                                const char *extension) noexcept {
  if ((label == nullptr) || (pathBuffer == nullptr) ||
      (pathBufferSize == 0U)) {
    return false;
  }

  bool changed = false;
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);

  const bool hasPath = pathBuffer[0] != '\0';
  const std::filesystem::path resolvedOsPath =
      hasPath ? resolve_virtual_path(pathBuffer) : std::filesystem::path();
  const bool resolvesOnDisk =
      hasPath && !resolvedOsPath.empty() &&
      std::filesystem::exists(resolvedOsPath);
  if (hasPath && !resolvesOnDisk) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.2F, 1.0F), "Missing file: %s",
                       pathBuffer);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      pathBuffer[0] = '\0';
      changed = true;
    }
  }

  static char query[128] = {};
  const char *previewText = hasPath ? pathBuffer : "<none>";
  if (ImGui::BeginCombo("##picker", previewText)) {
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##search", "Search...", query, sizeof(query));

    if (ImGui::Selectable("<none>", !hasPath)) {
      pathBuffer[0] = '\0';
      changed = true;
    }

    constexpr std::size_t kMaxHits = 64U;
    static char hits[kMaxHits][196];
    const std::size_t hitCount = scan_paths_by_extension(
        editor_asset_root(), 0U, extension, query, hits, kMaxHits, 0U);
    for (std::size_t i = 0U; i < hitCount; ++i) {
      ImGui::PushID(static_cast<int>(i));
      const bool isSelected = (std::strcmp(pathBuffer, hits[i]) == 0);
      if (ImGui::Selectable(hits[i], isSelected)) {
        std::snprintf(pathBuffer, pathBufferSize, "%s", hits[i]);
        changed = true;
      }
      ImGui::PopID();
    }
    if (hitCount == kMaxHits) {
      ImGui::TextDisabled("More results than shown -- refine the search.");
    }
    ImGui::EndCombo();
  }

  ImGui::PopID();
  return changed;
}

} // namespace engine::editor
