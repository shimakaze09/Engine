// Implements material parent-chain resolution declared in
// material_inheritance.h.

#include "engine/renderer/material_inheritance.h"

#include <array>

namespace engine::renderer {

namespace {

bool same(const math::Vec3 &lhs, const math::Vec3 &rhs) noexcept {
  return (lhs.x == rhs.x) && (lhs.y == rhs.y) && (lhs.z == rhs.z);
}

bool same(const math::Vec2 &lhs, const math::Vec2 &rhs) noexcept {
  return (lhs.x == rhs.x) && (lhs.y == rhs.y);
}

/// Scalars and enums compare as themselves.
template <typename T> bool same(T lhs, T rhs) noexcept { return lhs == rhs; }

/// material_field bits for the fields where `next` differs from `current`.
std::uint16_t changed_fields(const Material &current,
                             const MaterialTextureSlots &currentSlots,
                             const Material &next,
                             const MaterialTextureSlots &nextSlots) noexcept {
  std::uint16_t changed = 0U;
#define ENGINE_MATERIAL_MARK_PARAM(name, member, key)                          \
  if (!same(current.member, next.member)) {                                    \
    changed = static_cast<std::uint16_t>(changed | material_field::k##name);   \
  }
  ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_MARK_PARAM)
#undef ENGINE_MATERIAL_MARK_PARAM
#define ENGINE_MATERIAL_MARK_TEXTURE(name, slot, handle, key)                  \
  if (currentSlots.slot != nextSlots.slot) {                                   \
    changed = static_cast<std::uint16_t>(changed | material_field::k##name);   \
  }
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_MARK_TEXTURE)
#undef ENGINE_MATERIAL_MARK_TEXTURE
  return changed;
}

/// Takes every field `overridden` does not cover from the parent. A texture
/// handle survives only while its slot keeps the same asset; any other is
/// cleared for resolve_material_textures to fill on its next pass.
void inherit_from(const Material &parent,
                  const MaterialTextureSlots &parentSlots,
                  std::uint16_t overridden, Material *params,
                  MaterialTextureSlots *slots) noexcept {
#define ENGINE_MATERIAL_INHERIT_PARAM(name, member, key)                       \
  if ((overridden & material_field::k##name) == 0U) {                          \
    params->member = parent.member;                                            \
  }
  ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_INHERIT_PARAM)
#undef ENGINE_MATERIAL_INHERIT_PARAM
#define ENGINE_MATERIAL_INHERIT_TEXTURE(name, slot, handle, key)               \
  if (((overridden & material_field::k##name) == 0U) &&                        \
      (slots->slot != parentSlots.slot)) {                                     \
    slots->slot = parentSlots.slot;                                            \
    params->handle = kInvalidTextureHandle;                                    \
  }
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_INHERIT_TEXTURE)
#undef ENGINE_MATERIAL_INHERIT_TEXTURE
}

/// Whether any texture slot names a different asset.
bool slots_differ(const MaterialTextureSlots &lhs,
                  const MaterialTextureSlots &rhs) noexcept {
  bool differ = false;
#define ENGINE_MATERIAL_SLOT_DIFFERS(name, slot, handle, key)                  \
  differ = differ || (lhs.slot != rhs.slot);
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_SLOT_DIFFERS)
#undef ENGINE_MATERIAL_SLOT_DIFFERS
  return differ;
}

} // namespace

content::AssetId find_material_parent_id(const content::AssetCatalog *catalog,
                                         content::AssetId materialId) noexcept {
  constexpr std::size_t kMaxDeps = content::AssetMetadata::kMaxDependencies;
  content::AssetId deps[kMaxDeps] = {};
  const std::size_t depCount =
      get_dependencies(catalog, materialId, deps, kMaxDeps);
  for (std::size_t i = 0U; i < depCount; ++i) {
    const content::AssetMetadata *metadata =
        find_asset_metadata(catalog, deps[i]);
    if ((metadata != nullptr) &&
        (metadata->typeTag == content::AssetTypeTag::Material)) {
      return deps[i];
    }
  }
  return content::kInvalidAssetId;
}

bool material_chain_contains(const content::AssetCatalog *catalog,
                             content::AssetId from,
                             content::AssetId target) noexcept {
  // A loaded chain is never deeper than the loader's limit; the bound only
  // stops a walk through a cycle that got in some other way.
  content::AssetId current = from;
  for (std::size_t depth = 0U; (depth <= AssetDatabase::kMaxMaterialAssets) &&
                               (current != content::kInvalidAssetId);
       ++depth) {
    if (current == target) {
      return true;
    }
    current = find_material_parent_id(catalog, current);
  }
  return false;
}

std::size_t
propagate_material_to_dependents(AssetDatabase *database,
                                 const content::AssetCatalog *catalog,
                                 content::AssetId changedId) noexcept {
  if ((database == nullptr) || (catalog == nullptr) ||
      (changedId == content::kInvalidAssetId)) {
    return 0U;
  }

  // Breadth first from the changed material; every material enters the
  // list once, so a cycle an edit introduced still ends.
  std::array<content::AssetId, AssetDatabase::kMaxMaterialAssets + 1U>
      visited{};
  std::size_t visitedCount = 0U;
  visited[visitedCount++] = changedId;
  const auto seen = [&visited, &visitedCount](content::AssetId id) noexcept {
    for (std::size_t i = 0U; i < visitedCount; ++i) {
      if (visited[i] == id) {
        return true;
      }
    }
    return false;
  };

  for (std::size_t next = 0U; next < visitedCount; ++next) {
    const content::AssetId parentId = visited[next];
    const Material *parent = find_material_params(database, parentId);
    const MaterialTextureSlots *parentSlots =
        find_material_texture_slots(database, parentId);
    if ((parent == nullptr) || (parentSlots == nullptr)) {
      continue;
    }
    for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
      MaterialAssetRecord &record = database->materialAssets[i];
      if (!database->materialOccupied[i] ||
          (record.state != content::AssetState::Ready) || seen(record.id) ||
          (find_material_parent_id(catalog, record.id) != parentId)) {
        continue;
      }
      const MaterialTextureSlots before = record.textureSlots;
      inherit_from(*parent, *parentSlots, record.overriddenFields,
                   &record.params, &record.textureSlots);
      if (slots_differ(before, record.textureSlots)) {
        record.unregisterableTextureSlots = 0U;
      }
      visited[visitedCount++] = record.id;
    }
  }
  return visitedCount - 1U;
}

bool edit_material_asset(AssetDatabase *database,
                         const content::AssetCatalog *catalog,
                         content::AssetId materialId, const Material &params,
                         const MaterialTextureSlots &textureSlots) noexcept {
  const Material *current = find_material_params(database, materialId);
  const MaterialTextureSlots *currentSlots =
      find_material_texture_slots(database, materialId);
  if ((current == nullptr) || (currentSlots == nullptr)) {
    return false;
  }

  const std::uint16_t overridden = static_cast<std::uint16_t>(
      material_overrides(database, materialId) |
      changed_fields(*current, *currentSlots, params, textureSlots));
  return restore_material_asset(database, catalog, materialId, params,
                                textureSlots, overridden);
}

bool restore_material_asset(AssetDatabase *database,
                            const content::AssetCatalog *catalog,
                            content::AssetId materialId, const Material &params,
                            const MaterialTextureSlots &textureSlots,
                            std::uint16_t overriddenFields) noexcept {
  if (find_material_params(database, materialId) == nullptr) {
    return false;
  }
  const content::AssetMetadata *metadata =
      find_asset_metadata(catalog, materialId);
  const char *sourcePath =
      (metadata != nullptr) ? metadata->filePath.data() : nullptr;
  // None of these can fail for a material found above.
  if (!register_material_asset(database, materialId, sourcePath, params) ||
      !set_material_texture_slots(database, materialId, textureSlots) ||
      !set_material_overrides(database, materialId, overriddenFields)) {
    return false;
  }
  static_cast<void>(
      propagate_material_to_dependents(database, catalog, materialId));
  return true;
}

} // namespace engine::renderer
