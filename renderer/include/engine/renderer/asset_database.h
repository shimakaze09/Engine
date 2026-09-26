// Declares the renderer's asset database: the GPU-side records for meshes,
// textures and materials -- handles, refcounts, residency, load state and
// resolved material parameters. What an asset is, where it lives and what
// it depends on is the content asset catalog's (content/asset_catalog.h),
// which the engine pipeline owns; renderer code that needs it takes the
// catalog as a parameter.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "engine/content/asset_catalog.h"
#include "engine/core/fixed_hash_table.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/material.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

/// One mesh slot: id, GPU handle, source path, refcount, residency. The
/// last-access stamp is atomic because parallel render-prep chunk jobs
/// touch it through resolve_mesh_asset (relaxed ordering: it is an LRU
/// hint read only by the single-threaded eviction pass). Copies transfer
/// the stamp with relaxed loads/stores; slots are only copied during
/// single-threaded slot reset/reuse, never during render prep.
struct MeshAssetRecord final {
  MeshAssetRecord() noexcept = default;
  MeshAssetRecord(const MeshAssetRecord &other) noexcept { *this = other; }
  MeshAssetRecord &operator=(const MeshAssetRecord &other) noexcept {
    id = other.id;
    runtimeMesh = other.runtimeMesh;
    sourcePath = other.sourcePath;
    refCount = other.refCount;
    lastAccessFrame.store(
        other.lastAccessFrame.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    sizeBytes = other.sizeBytes;
    state = other.state;
    requestedResident = other.requestedResident;
    pinned = other.pinned;
    return *this;
  }

  content::AssetId id = content::kInvalidAssetId;
  MeshHandle runtimeMesh = kInvalidMeshHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  std::atomic<std::uint64_t> lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  content::AssetState state = content::AssetState::Unloaded;
  bool requestedResident = false;
  bool pinned = false;
};

/// One texture slot: id, GPU handle, source path, state. Materials own
/// textures: a record lives while some material's texture slots name it,
/// and release_unreferenced_textures frees it once none does.
struct TextureAssetRecord final {
  content::AssetId id = content::kInvalidAssetId;
  TextureHandle runtimeTexture = kInvalidTextureHandle;
  std::array<char, 260U> sourcePath{};
  std::uint64_t lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  content::AssetState state = content::AssetState::Unloaded;
  bool requestedResident = false;
  /// The source file's modification time (core::vfs_file_mtime) when the
  /// record's current state was last loaded or refused; the hot-reload
  /// poll reloads the texture when the file's time moves off it.
  std::int64_t sourceWriteTime = 0;
};

/// Authored texture-slot references for one material (path-derived asset
/// ids; kInvalidAssetId = slot not set). Kept separate from Material so the
/// per-draw-command struct stays a flat parameter/handle block: these ids
/// are only touched by the loader and resolve_material_textures, never
/// per-frame render prep.
struct MaterialTextureSlots final {
  content::AssetId albedo = content::kInvalidAssetId;
  content::AssetId metallicRoughness = content::kInvalidAssetId;
  content::AssetId emissive = content::kInvalidAssetId;
  content::AssetId occlusion = content::kInvalidAssetId;
  content::AssetId opacity = content::kInvalidAssetId;

  bool operator==(const MaterialTextureSlots &) const noexcept = default;
};

/// Every field a material document authors, in document order: the
/// override bit's name, the Material member, and the document key. The
/// override bits, the loader's reads and authored-field scan, parent
/// inheritance, the editor's change detection and the writer all expand
/// from this table and the texture table below, so a field added here
/// reaches every one of them and a field left out reaches none.
#define ENGINE_MATERIAL_PARAM_FIELDS(X)                                        \
  X(Albedo, albedo, "albedo")                                                  \
  X(Emissive, emissive, "emissive")                                            \
  X(Roughness, roughness, "roughness")                                         \
  X(Metallic, metallic, "metallic")                                            \
  X(Opacity, opacity, "opacity")                                               \
  X(ShadingModel, shadingModel, "shadingModel")                                \
  X(AlphaMode, alphaMode, "alphaMode")                                         \
  X(AlphaCutoff, alphaCutoff, "alphaCutoff")                                   \
  X(UvTiling, uvTiling, "uvTiling")                                            \
  X(UvOffset, uvOffset, "uvOffset")

/// Every texture slot, in MaterialTextureSlots order: the override bit's
/// name, the MaterialTextureSlots member, the Material handle it resolves
/// into, and the key under the document's "textures" object.
#define ENGINE_MATERIAL_TEXTURE_FIELDS(X)                                      \
  X(AlbedoTexture, albedo, albedoTexture, "albedo")                            \
  X(MetallicRoughnessTexture, metallicRoughness, metallicRoughnessTexture,     \
    "metallicRoughness")                                                       \
  X(EmissiveTexture, emissive, emissiveTexture, "emissive")                    \
  X(OcclusionTexture, occlusion, occlusionTexture, "occlusion")                \
  X(OpacityTexture, opacity, opacityTexture, "opacity")

/// Bits of MaterialAssetRecord::overriddenFields, one per authored field,
/// generated from the two tables above.
namespace material_field {
enum FieldIndex : std::uint8_t {
#define ENGINE_MATERIAL_FIELD_INDEX(name, ...) k##name##Index,
  ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_FIELD_INDEX)
      ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_FIELD_INDEX)
#undef ENGINE_MATERIAL_FIELD_INDEX
          kFieldCount
};
static_assert(kFieldCount <= 16U, "overriddenFields holds one bit per field");

#define ENGINE_MATERIAL_FIELD_BIT(name, ...)                                   \
  inline constexpr std::uint16_t k##name =                                     \
      static_cast<std::uint16_t>(1U << k##name##Index);
ENGINE_MATERIAL_PARAM_FIELDS(ENGINE_MATERIAL_FIELD_BIT)
ENGINE_MATERIAL_TEXTURE_FIELDS(ENGINE_MATERIAL_FIELD_BIT)
#undef ENGINE_MATERIAL_FIELD_BIT

inline constexpr std::uint16_t kAll =
    static_cast<std::uint16_t>((1U << kFieldCount) - 1U);
} // namespace material_field

/// One material slot: id, source path, and the fully resolved parameters,
/// so render prep reads a flat record. The parent chain is resolved into
/// it at load and again whenever a parent changes (overriddenFields says
/// which fields are the material's own). textureSlots holds the same
/// chain's resolved texture references; Material's TextureHandle fields
/// are populated from them by resolve_material_textures once GPU upload
/// succeeds.
struct MaterialAssetRecord final {
  content::AssetId id = content::kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
  Material params{};
  MaterialTextureSlots textureSlots{};
  content::AssetState state = content::AssetState::Unloaded;
  /// Texture slots resolve_material_textures gave up on because the
  /// texture table had no room to record them, one bit per slot in
  /// MaterialTextureSlots order. Runtime-only; cleared whenever the slots
  /// are assigned again, so a reload or an edit tries once more.
  std::uint8_t unregisterableTextureSlots = 0U;
  /// material_field bits for the fields this material authors itself; the
  /// rest are its parent's, re-resolved whenever the parent changes. A
  /// material with no parent authors everything.
  std::uint16_t overriddenFields = material_field::kAll;
};

/// Fixed-slot asset tables (meshes, textures, materials, metadata).
struct AssetDatabase final {
  static constexpr std::size_t kMaxMeshAssets = 4096U;
  std::array<MeshAssetRecord, kMaxMeshAssets> meshAssets =
      std::array<MeshAssetRecord, kMaxMeshAssets>();
  std::array<bool, kMaxMeshAssets> occupied{};
  // Mesh claims refused for want of a record since the last eviction pass,
  // which frees that many of the coldest evictable records even under the
  // byte budget, so small cached meshes cannot hold every record while a
  // request goes unserved. Runtime-only.
  std::size_t refusedMeshClaims = 0U;
  // Id -> meshAssets slot, looked up per visible mesh from the parallel
  // render-prep jobs. Twice the record capacity, and rebuilt once erases
  // leave a quarter of it tombstoned, so it is never more than three
  // quarters full and a probe, hit or miss, stays a few slots long.
  static constexpr std::size_t kMeshIndexCapacity = 2U * kMaxMeshAssets;
  core::FixedHashTable<content::AssetId, std::uint32_t, kMeshIndexCapacity>
      meshIndex{};

  static constexpr std::size_t kMaxTextureAssets = 512U;
  std::array<TextureAssetRecord, kMaxTextureAssets> textureAssets =
      std::array<TextureAssetRecord, kMaxTextureAssets>();
  std::array<bool, kMaxTextureAssets> textureOccupied{};
  // Id -> textureAssets slot, the same shape as meshIndex: twice the
  // records, so a probe stays short however full the table is.
  static constexpr std::size_t kTextureIndexCapacity = 2U * kMaxTextureAssets;
  core::FixedHashTable<content::AssetId, std::uint32_t, kTextureIndexCapacity>
      textureIndex{};
  // Where the next texture hot-reload poll resumes: the poll checks a
  // bounded run of slots per call, so a full table costs a sweep over
  // several polls rather than every file in one frame. Runtime-only.
  std::uint32_t textureReloadCursor = 0U;
  // Set whenever a material's texture slots change, so the next
  // release_unreferenced_textures looks for textures no material names any
  // more; nothing else can leave one unreferenced. Runtime-only.
  bool textureReferencesChanged = false;

  static constexpr std::size_t kMaxMaterialAssets = 1024U;
  std::array<MaterialAssetRecord, kMaxMaterialAssets> materialAssets =
      std::array<MaterialAssetRecord, kMaxMaterialAssets>();
  std::array<bool, kMaxMaterialAssets> materialOccupied{};
  // Id -> materialAssets slot, looked up per draw from render prep.
  static constexpr std::size_t kMaterialIndexCapacity = 2U * kMaxMaterialAssets;
  core::FixedHashTable<content::AssetId, std::uint32_t, kMaterialIndexCapacity>
      materialIndex{};

  std::uint64_t currentFrame = 0ULL;
};

/// Bumps the frame counter used for last-access stamps.
void advance_asset_database_frame(AssetDatabase *database) noexcept;

// Minimum frames since last access before a mesh record may be evicted, so
// budget pressure never drops meshes that were just uploaded or drawn.
inline constexpr std::uint64_t kMeshEvictionMinAgeFrames = 60ULL;

/// Records the loaded payload size used for cache-budget accounting.
bool set_mesh_asset_size(AssetDatabase *database, content::AssetId id,
                         std::uint64_t sizeBytes) noexcept;

/// Marks the coldest unpinned streamed meshes non-resident (LRU order, age
/// hysteresis, retained records skipped) until the resident total fits the
/// budget and every refused claim has a record on its way (see
/// refusedMeshClaims); the asset manager unloads and releases them on its
/// next residency sync. Returns the number of records marked.
std::size_t evict_mesh_assets_over_budget(AssetDatabase *database,
                                          std::uint64_t budgetBytes) noexcept;

// make_asset_id_from_path / make_asset_id_from_file live in
// engine::content (re-exported by asset_metadata.h); the file-hash
// fallback contract stays pinned by asset_database_test.

/// Inserts or updates a mesh record; false when the table is full. Records
/// registered here are already Ready with no streaming reload path
/// (builtin/synchronous meshes), so they are pinned: counted against the
/// cache budget but never evicted.
bool register_mesh_asset(AssetDatabase *database, content::AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept;
/// Marks a mesh asset as requested and loading without queuing a sync load.
bool request_mesh_asset_streaming_load(AssetDatabase *database,
                                       content::AssetId id,
                                       const char *sourcePath) noexcept;
/// Lifecycle state for the id (Unloaded when unknown).
content::AssetState mesh_asset_state(const AssetDatabase *database,
                                     content::AssetId id) noexcept;
/// Sets the requested value for mesh asset state.
bool set_mesh_asset_state(AssetDatabase *database, content::AssetId id,
                          content::AssetState state,
                          MeshHandle runtimeMesh) noexcept;
/// True when a streaming load was requested for the id.
bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   content::AssetId id) noexcept;
/// GPU handle for the id (touches last-access); invalid unless Ready.
MeshHandle resolve_mesh_asset(AssetDatabase *database,
                              content::AssetId id) noexcept;
/// Increments the refcount; false when the id is unknown.
bool retain_mesh_asset(AssetDatabase *database, content::AssetId id) noexcept;
/// Decrements the refcount; false when unknown or already zero.
bool release_mesh_asset(AssetDatabase *database, content::AssetId id) noexcept;
/// Resets every table to empty.
void clear_asset_database(AssetDatabase *database) noexcept;

// Low-level mesh slot access shared by the database and the asset manager.
/// Returns the record slot for an id, or kMaxMeshAssets when absent.
std::size_t find_mesh_asset_record_slot(const AssetDatabase *database,
                                        content::AssetId id) noexcept;
/// Finds the id's slot or claims a free one (occupied is set and the id
/// written for fresh claims). Returns kMaxMeshAssets when full.
std::size_t claim_mesh_asset_record_slot(AssetDatabase *database,
                                         content::AssetId id) noexcept;
/// Whether a record may give up its slot once its mesh is unloaded: not
/// requested resident, not pinned, and holding no reference beyond the
/// request's own, the same one eviction ignores.
bool mesh_asset_record_releasable(const MeshAssetRecord &record) noexcept;
/// Frees a mesh record slot for reuse. Requires refCount == 0 and no live
/// runtimeMesh (unload first). Every other record keeps its slot.
bool unregister_mesh_asset(AssetDatabase *database,
                           content::AssetId id) noexcept;

// Material asset management. Materials are CPU parameter blocks; records
// hold parent-resolved values, so lookups are flat and mutation-free (safe
// from parallel render-prep jobs). Records change only on the main thread,
// outside render prep: a load, a reload, or an editor edit and the
// re-resolution of dependents it triggers.
/// Inserts or updates a material record; false when the table is full.
bool register_material_asset(AssetDatabase *database, content::AssetId id,
                             const char *sourcePath,
                             const Material &params) noexcept;
/// Resolved parameters for the id, or nullptr when absent (no access
/// stamps are touched — safe to call from parallel jobs).
const Material *find_material_params(const AssetDatabase *database,
                                     content::AssetId id) noexcept;
/// Lifecycle state for the material id (Unloaded when unknown).
content::AssetState material_asset_state(const AssetDatabase *database,
                                         content::AssetId id) noexcept;
/// Overwrites an already-registered material's texture-slot references;
/// false when the id is unknown. Called by the loader after
/// register_material_asset so a reload can update both parts atomically
/// from the caller's perspective (register first, then slots — either both
/// land or the reload was already rejected before either call).
bool set_material_texture_slots(AssetDatabase *database, content::AssetId id,
                                const MaterialTextureSlots &slots) noexcept;
/// Authored texture-slot references for the id, or nullptr when absent.
const MaterialTextureSlots *
find_material_texture_slots(const AssetDatabase *database,
                            content::AssetId id) noexcept;
/// Records which fields the material authors (material_field bits); false
/// when the id is unknown.
bool set_material_overrides(AssetDatabase *database, content::AssetId id,
                            std::uint16_t overriddenFields) noexcept;
/// The material's material_field override bits; kAll when the id is
/// unknown, as a material with nothing to inherit authors everything.
std::uint16_t material_overrides(const AssetDatabase *database,
                                 content::AssetId id) noexcept;

// Texture asset management.
bool register_texture_asset(AssetDatabase *database, content::AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept;
/// True when register_texture_asset or register_texture_asset_failed can
/// record this id: it is already in the table, or the table has room.
bool texture_asset_slot_available(const AssetDatabase *database,
                                  content::AssetId id) noexcept;
/// True when register_material_asset can record this id: it is already in
/// the table, or the table has room.
bool material_asset_slot_available(const AssetDatabase *database,
                                   content::AssetId id) noexcept;
/// Registers (or updates) a texture id as Failed with no GPU handle, so
/// resolve_material_textures does not retry it every frame; the source
/// path is kept for diagnostics. It stays Failed until its file changes and
/// the editor's hot-reload poll loads it again (texture_hot_reload.h).
bool register_texture_asset_failed(AssetDatabase *database, content::AssetId id,
                                   const char *sourcePath) noexcept;
/// Lifecycle state for the texture id (Unloaded when unknown).
content::AssetState texture_asset_state(const AssetDatabase *database,
                                        content::AssetId id) noexcept;
/// Records the source file time the texture's current state came from;
/// ignored for an id the table does not hold.
void set_texture_source_write_time(AssetDatabase *database, content::AssetId id,
                                   std::int64_t writeTime) noexcept;
/// Sets the requested value for texture asset state.
bool set_texture_asset_state(AssetDatabase *database, content::AssetId id,
                             content::AssetState state,
                             TextureHandle runtimeTexture) noexcept;
/// GPU texture handle for the id; invalid unless Ready.
TextureHandle resolve_texture_asset(AssetDatabase *database,
                                    content::AssetId id) noexcept;
/// Frees a texture record slot for reuse. Requires no live runtimeTexture
/// (release the handle and set the state first); false when the id is
/// unknown or still holds one. Every other record keeps its slot.
bool unregister_texture_asset(AssetDatabase *database,
                              content::AssetId id) noexcept;

} // namespace engine::renderer
