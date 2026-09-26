// Implements material parent-chain resolution declared in
// material_inheritance.h.

#include "engine/renderer/material_inheritance.h"

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

namespace {

struct PropagationContext final {
  AssetDatabase *database = nullptr;
  const content::AssetCatalog *catalog = nullptr;
};

/// One dependent of a changed asset. A material that inherits from the
/// cause takes every field it does not override from the cause's current
/// values; a material that names the cause as a texture drops that slot's
/// handle for resolve_material_textures to fetch again. Any other kind of
/// dependent is not the renderer's material to update.
void update_dependent_material(content::AssetId dependent,
                               content::AssetId cause,
                               void *userData) noexcept {
  const auto &context = *static_cast<const PropagationContext *>(userData);
  AssetDatabase *database = context.database;
  const std::uint32_t *slot = database->materialIndex.find(dependent);
  if ((slot == nullptr) || !database->materialOccupied[*slot]) {
    return;
  }
  MaterialAssetRecord &record = database->materialAssets[*slot];
  if (record.state != content::AssetState::Ready) {
    return;
  }

  if (find_material_parent_id(context.catalog, dependent) == cause) {
    const Material *parent = find_material_params(database, cause);
    const MaterialTextureSlots *parentSlots =
        find_material_texture_slots(database, cause);
    if ((parent == nullptr) || (parentSlots == nullptr)) {
      return;
    }
    const MaterialTextureSlots before = record.textureSlots;
    inherit_from(*parent, *parentSlots, record.overriddenFields, &record.params,
                 &record.textureSlots);
    if (slots_differ(before, record.textureSlots)) {
      record.unregisterableTextureSlots = 0U;
    }
    return;
  }

#define ENGINE_MATERIAL_DROP_CHANGED_TEXTURE(name, slot, handle, key)          \
  if (record.textureSlots.slot == cause) {                                     \
    record.params.handle = kInvalidTextureHandle;                              \
  }
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_DROP_CHANGED_TEXTURE)
#undef ENGINE_MATERIAL_DROP_CHANGED_TEXTURE
}

/// Rewrites the material's catalog edges to match its live record: its
/// parent, then every texture slot the material authors itself. An
/// inherited slot is the parent's edge to carry, as at load. Writes only
/// when the edges differ, so an edit that changes no reference leaves the
/// catalog's generation where it was.
void sync_material_edges(const AssetDatabase *database,
                         content::AssetCatalog *catalog,
                         content::AssetId materialId) noexcept {
  const content::AssetMetadata *current =
      content::find_asset_metadata(catalog, materialId);
  const MaterialTextureSlots *slots =
      find_material_texture_slots(database, materialId);
  if ((current == nullptr) || (slots == nullptr)) {
    return;
  }
  content::AssetMetadata next = *current;
  next.dependencyCount = 0U;
  next.dependencies = {};
  const content::AssetId parent = find_material_parent_id(catalog, materialId);
  if (parent != content::kInvalidAssetId) {
    static_cast<void>(content::asset_metadata_add_dependency(&next, parent));
  }
  const std::uint16_t authored = material_overrides(database, materialId);
#define ENGINE_MATERIAL_AUTHORED_EDGE(name, slot, handle, key)                 \
  if (((authored & material_field::k##name) != 0U) &&                          \
      (slots->slot != content::kInvalidAssetId)) {                             \
    static_cast<void>(                                                         \
        content::asset_metadata_add_dependency(&next, slots->slot));           \
  }
  ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_AUTHORED_EDGE)
#undef ENGINE_MATERIAL_AUTHORED_EDGE

  bool same = next.dependencyCount == current->dependencyCount;
  for (std::size_t i = 0U; same && (i < next.dependencyCount); ++i) {
    same = next.dependencies[i] == current->dependencies[i];
  }
  if (!same) {
    static_cast<void>(content::register_asset_metadata(catalog, next));
  }
}

} // namespace

std::size_t
propagate_material_to_dependents(AssetDatabase *database,
                                 const content::AssetCatalog *catalog,
                                 content::AssetId changedId) noexcept {
  if ((database == nullptr) || (catalog == nullptr) ||
      (changedId == content::kInvalidAssetId)) {
    return 0U;
  }
  PropagationContext context{database, catalog};
  return content::notify_asset_changed(catalog, changedId,
                                       &update_dependent_material, &context);
}

bool edit_material_asset(AssetDatabase *database,
                         content::AssetCatalog *catalog,
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
                            content::AssetCatalog *catalog,
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
  sync_material_edges(database, catalog, materialId);
  static_cast<void>(
      propagate_material_to_dependents(database, catalog, materialId));
  return true;
}

} // namespace engine::renderer
