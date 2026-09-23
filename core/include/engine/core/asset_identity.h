// Declares the value types by which every module names an asset: the
// 128-bit persistent GUID and the {guid, localId} reference. Only the
// values and their pure operations live here — no generation, no text
// form, no path or content hashing — so a component that a module below
// content defines (the world component types math hosts for scripting)
// can carry a reference. Everything that decides what a GUID is worth
// belongs to content, in engine/content/asset_identity.h.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// 128-bit persistent logical asset identity, held as two 64-bit halves
/// in big-endian reading order: `high` is the first eight bytes of the
/// canonical text form, `low` the last eight.
struct AssetGuid final {
  std::uint64_t high = 0U;
  std::uint64_t low = 0U;

  friend constexpr bool operator==(const AssetGuid &,
                                   const AssetGuid &) = default;
};

/// The all-zero GUID, which no asset owns. A nil UUID is neither a valid
/// generated value nor a derived one, so it can never collide with either.
inline constexpr AssetGuid kNilAssetGuid{};

/// True for any GUID an asset may own.
constexpr bool asset_guid_is_valid(const AssetGuid &guid) noexcept {
  return !((guid.high == 0U) && (guid.low == 0U));
}

/// A 64-bit hash of the GUID, for keying a fixed-capacity table. Not an
/// identity: two different GUIDs may hash alike, so a table must still
/// compare the full value.
std::uint64_t asset_guid_hash(const AssetGuid &guid) noexcept;

/// Deterministic total order over GUIDs, for reporting a set of them in a
/// stable sequence. Never use it to choose between two assets that claim
/// one GUID: that collision is an error to report, not to resolve.
bool asset_guid_precedes(const AssetGuid &a, const AssetGuid &b) noexcept;

/// Names one asset among the several a single source file can produce.
/// One glTF yields a mesh, a skeleton and several clips, and its import
/// settings select a mesh and primitive inside it, so a GUID naming the
/// file cannot on its own name "the walk clip of character". `guid`
/// identifies the source, `localId` the asset within it — the same split
/// Unity makes with its {guid, fileID} pairs.
///
/// localId 0 is the source's primary asset, which is the only one most
/// sources have. Sub-assets take content's asset_local_id, so the value
/// is derived from the name rather than assigned from a counter: it stays
/// the same across recooks, across machines, and across a rebuild from
/// scratch.
struct AssetRef final {
  AssetGuid guid{};
  std::uint64_t localId = 0U;

  friend constexpr bool operator==(const AssetRef &, const AssetRef &) = default;
};

/// The primary asset of `guid`.
constexpr AssetRef asset_ref_primary(const AssetGuid &guid) noexcept {
  return AssetRef{guid, 0U};
}

/// True when the reference names an asset at all.
constexpr bool asset_ref_is_valid(const AssetRef &ref) noexcept {
  return asset_guid_is_valid(ref.guid);
}

} // namespace engine::core
