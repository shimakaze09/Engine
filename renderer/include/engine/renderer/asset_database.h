// Declares asset database types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "engine/content/metadata_store.h"
#include "engine/core/fixed_hash_table.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/material.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

// The asset identity/metadata vocabulary is content-owned; these
// re-exports keep the renderer's established names for its own headers
// and remaining mixed consumers (the C4 renderer shims are deleted).
using content::AssetId;
using content::kInvalidAssetId;
using content::AssetState;
using content::AssetTypeTag;
using content::AssetMetadata;
using content::AssetRef;
using content::asset_ref_is_valid;
using content::asset_ref_primary;
using content::builtin_asset_guid;
using content::find_asset_metadata_by_ref;
using content::asset_metadata_has_tag;
using content::asset_metadata_add_tag;
using content::write_metadata_path;
using content::asset_metadata_add_dependency;
using content::asset_metadata_has_dependency;
using content::make_asset_id_from_path;
using content::make_asset_id_from_file;

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

  AssetId id = kInvalidAssetId;
  MeshHandle runtimeMesh = kInvalidMeshHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  std::atomic<std::uint64_t> lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  AssetState state = AssetState::Unloaded;
  bool requestedResident = false;
  bool pinned = false;
};

/// One texture slot: id, GPU handle, source path, refcount, state.
struct TextureAssetRecord final {
  AssetId id = kInvalidAssetId;
  TextureHandle runtimeTexture = kInvalidTextureHandle;
  std::array<char, 260U> sourcePath{};
  std::uint32_t refCount = 0U;
  std::uint64_t lastAccessFrame = 0ULL;
  std::uint64_t sizeBytes = 0ULL;
  AssetState state = AssetState::Unloaded;
  bool requestedResident = false;
};

/// Authored texture-slot references for one material (path-derived asset
/// ids; kInvalidAssetId = slot not set). Kept separate from Material so the
/// per-draw-command struct stays a flat parameter/handle block: these ids
/// are only touched by the loader and resolve_material_textures, never
/// per-frame render prep.
struct MaterialTextureSlots final {
  AssetId albedo = kInvalidAssetId;
  AssetId metallicRoughness = kInvalidAssetId;
  AssetId emissive = kInvalidAssetId;
  AssetId occlusion = kInvalidAssetId;
  AssetId opacity = kInvalidAssetId;
};

/// Bits of MaterialAssetRecord::overriddenFields, one per authored field.
namespace material_field {
inline constexpr std::uint16_t kAlbedo = 1U << 0U;
inline constexpr std::uint16_t kEmissive = 1U << 1U;
inline constexpr std::uint16_t kRoughness = 1U << 2U;
inline constexpr std::uint16_t kMetallic = 1U << 3U;
inline constexpr std::uint16_t kOpacity = 1U << 4U;
inline constexpr std::uint16_t kShadingModel = 1U << 5U;
inline constexpr std::uint16_t kAlphaMode = 1U << 6U;
inline constexpr std::uint16_t kAlphaCutoff = 1U << 7U;
inline constexpr std::uint16_t kUvTiling = 1U << 8U;
inline constexpr std::uint16_t kUvOffset = 1U << 9U;
inline constexpr std::uint16_t kAlbedoTexture = 1U << 10U;
inline constexpr std::uint16_t kMetallicRoughnessTexture = 1U << 11U;
inline constexpr std::uint16_t kEmissiveTexture = 1U << 12U;
inline constexpr std::uint16_t kOcclusionTexture = 1U << 13U;
inline constexpr std::uint16_t kOpacityTexture = 1U << 14U;
inline constexpr std::uint16_t kAll = (1U << 15U) - 1U;
} // namespace material_field

/// One material slot: id, source path, and the fully resolved parameters
/// (parent-chain overrides are baked at load time so render prep reads a
/// flat record). textureSlots holds the same chain's resolved texture
/// references; Material's TextureHandle fields are populated from them by
/// resolve_material_textures once GPU upload succeeds.
struct MaterialAssetRecord final {
  AssetId id = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
  Material params{};
  MaterialTextureSlots textureSlots{};
  AssetState state = AssetState::Unloaded;
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
  std::array<MeshAssetRecord, kMaxMeshAssets> meshAssets{};
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
  core::FixedHashTable<AssetId, std::uint32_t, kMeshIndexCapacity> meshIndex{};

  static constexpr std::size_t kMaxTextureAssets = 512U;
  std::array<TextureAssetRecord, kMaxTextureAssets> textureAssets{};
  std::array<bool, kMaxTextureAssets> textureOccupied{};

  static constexpr std::size_t kMaxMaterialAssets = 1024U;
  std::array<MaterialAssetRecord, kMaxMaterialAssets> materialAssets{};
  std::array<bool, kMaxMaterialAssets> materialOccupied{};

  // The generic identity/tag/dependency table is content-owned;
  // this database embeds one store and delegates the metadata API to it.
  static constexpr std::size_t kMaxMetadata =
      content::MetadataStore::kMaxMetadata;
  content::MetadataStore metadataStore{};

  std::uint64_t currentFrame = 0ULL;
};

/// Bumps the frame counter used for last-access stamps.
void advance_asset_database_frame(AssetDatabase *database) noexcept;

// Minimum frames since last access before a mesh record may be evicted, so
// budget pressure never drops meshes that were just uploaded or drawn.
inline constexpr std::uint64_t kMeshEvictionMinAgeFrames = 60ULL;

/// Records the loaded payload size used for cache-budget accounting.
bool set_mesh_asset_size(AssetDatabase *database, AssetId id,
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
bool register_mesh_asset(AssetDatabase *database, AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept;
/// Marks a mesh asset as requested and loading without queuing a sync load.
bool request_mesh_asset_streaming_load(AssetDatabase *database, AssetId id,
                                       const char *sourcePath) noexcept;
/// Lifecycle state for the id (Unloaded when unknown).
AssetState mesh_asset_state(const AssetDatabase *database, AssetId id) noexcept;
/// Sets the requested value for mesh asset state.
bool set_mesh_asset_state(AssetDatabase *database, AssetId id, AssetState state,
                          MeshHandle runtimeMesh) noexcept;
/// True when a streaming load was requested for the id.
bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   AssetId id) noexcept;
/// GPU handle for the id (touches last-access); invalid unless Ready.
MeshHandle resolve_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Increments the refcount; false when the id is unknown.
bool retain_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Decrements the refcount; false when unknown or already zero.
bool release_mesh_asset(AssetDatabase *database, AssetId id) noexcept;
/// Resets every table to empty.
void clear_asset_database(AssetDatabase *database) noexcept;

// Low-level mesh slot access shared by the database and the asset manager.
/// Returns the record slot for an id, or kMaxMeshAssets when absent.
std::size_t find_mesh_asset_record_slot(const AssetDatabase *database,
                                        AssetId id) noexcept;
/// Finds the id's slot or claims a free one (occupied is set and the id
/// written for fresh claims). Returns kMaxMeshAssets when full.
std::size_t claim_mesh_asset_record_slot(AssetDatabase *database,
                                         AssetId id) noexcept;
/// Whether a record may give up its slot once its mesh is unloaded: not
/// requested resident, not pinned, and holding no reference beyond the
/// request's own, the same one eviction ignores.
bool mesh_asset_record_releasable(const MeshAssetRecord &record) noexcept;
/// Frees a mesh record slot for reuse. Requires refCount == 0 and no live
/// runtimeMesh (unload first). Every other record keeps its slot.
bool unregister_mesh_asset(AssetDatabase *database, AssetId id) noexcept;

// Material asset management. Materials are CPU parameter blocks; records
// hold parent-resolved values, so lookups are flat and mutation-free
// (safe from parallel render-prep jobs).
/// Inserts or updates a material record; false when the table is full.
bool register_material_asset(AssetDatabase *database, AssetId id,
                             const char *sourcePath,
                             const Material &params) noexcept;
/// Resolved parameters for the id, or nullptr when absent (no access
/// stamps are touched — safe to call from parallel jobs).
const Material *find_material_params(const AssetDatabase *database,
                                     AssetId id) noexcept;
/// Lifecycle state for the material id (Unloaded when unknown).
AssetState material_asset_state(const AssetDatabase *database,
                                AssetId id) noexcept;
/// Overwrites an already-registered material's texture-slot references;
/// false when the id is unknown. Called by the loader after
/// register_material_asset so a reload can update both parts atomically
/// from the caller's perspective (register first, then slots — either both
/// land or the reload was already rejected before either call).
bool set_material_texture_slots(AssetDatabase *database, AssetId id,
                                const MaterialTextureSlots &slots) noexcept;
/// Authored texture-slot references for the id, or nullptr when absent.
const MaterialTextureSlots *
find_material_texture_slots(const AssetDatabase *database, AssetId id) noexcept;
/// Records which fields the material authors (material_field bits); false
/// when the id is unknown.
bool set_material_overrides(AssetDatabase *database, AssetId id,
                            std::uint16_t overriddenFields) noexcept;
/// The material's material_field override bits; kAll when the id is
/// unknown, as a material with nothing to inherit authors everything.
std::uint16_t material_overrides(const AssetDatabase *database,
                                 AssetId id) noexcept;

// Texture asset management.
bool register_texture_asset(AssetDatabase *database, AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept;
/// True when register_texture_asset or register_texture_asset_failed can
/// record this id: it is already in the table, or the table has room.
bool texture_asset_slot_available(const AssetDatabase *database,
                                  AssetId id) noexcept;
/// Registers (or updates) a texture id as permanently Failed with no GPU
/// handle, so resolve_material_textures does not retry it every frame; the
/// source path is kept for diagnostics.
bool register_texture_asset_failed(AssetDatabase *database, AssetId id,
                                   const char *sourcePath) noexcept;
/// Lifecycle state for the texture id (Unloaded when unknown).
AssetState texture_asset_state(const AssetDatabase *database,
                               AssetId id) noexcept;
/// Sets the requested value for texture asset state.
bool set_texture_asset_state(AssetDatabase *database, AssetId id,
                             AssetState state,
                             TextureHandle runtimeTexture) noexcept;
/// GPU texture handle for the id; invalid unless Ready.
TextureHandle resolve_texture_asset(AssetDatabase *database,
                                    AssetId id) noexcept;
/// Increments the texture refcount; false when unknown.
bool retain_texture_asset(AssetDatabase *database, AssetId id) noexcept;
/// Decrements the texture refcount; false when unknown or zero.
bool release_texture_asset(AssetDatabase *database, AssetId id) noexcept;

// Metadata management.
bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept;
/// Finds the matching object or resource for asset metadata.
const AssetMetadata *find_asset_metadata(const AssetDatabase *database,
                                         AssetId id) noexcept;
/// Adds a tag to the id's metadata; false when unknown or tags full.
bool add_asset_tag(AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
/// True when the id's metadata carries the tag.
bool asset_has_tag(const AssetDatabase *database, AssetId id,
                   const char *tag) noexcept;
/// Collects up to maxIds ids carrying the tag; returns the count.
std::size_t query_assets_by_tag(const AssetDatabase *database, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept;
/// Collects up to maxIds ids of the given type; returns the count.
std::size_t query_assets_by_type(const AssetDatabase *database,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept;

// Dependency queries.
std::size_t get_dependencies(const AssetDatabase *database, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept;

/// Records a directed dependency edge id -> depId; false when full.
bool add_asset_dependency(AssetDatabase *database, AssetId id,
                          AssetId depId) noexcept;

/// Load an asset and all its dependencies (depth-first, dependency-first).
/// Returns false if a cycle is detected or if any dependency fails to resolve.
/// The `loadCallback` is invoked for each asset that needs loading, in
/// dependency order. It receives the AssetId and should return true if the
/// load succeeds.
bool load_with_dependencies(AssetDatabase *database, AssetId rootId,
                            bool (*loadCallback)(AssetDatabase *db, AssetId id,
                                                 void *userData),
                            void *userData) noexcept;

} // namespace engine::renderer
